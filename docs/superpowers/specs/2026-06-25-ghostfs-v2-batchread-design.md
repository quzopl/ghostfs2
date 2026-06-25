# ghostfs v2 — batch-read sekwencyjny: design

**Data:** 2026-06-25 **Część:** v2 optymalizacja (diagnoza z benchmarkv2.md).

## Problem
Po batch-insert zapis jest mocny, ale ODCZYT słabszy od v1 (sekw. −31%, losowy −46%). Przyczyna
(sekw.): `gh2_fs_read` (ścieżka per-blok, bez --compress) robi `extent_lookup_at` (**descent
B-drzewa z korzenia**) na KAŻDY blok 4 KB. Odczyt 1 MB = 256 descentów (memcpy+CRC węzłów),
choć kolejne bloki są w tym samym liściu. v1 miał direct/indirect (1 indyrekcja).

## Rozwiązanie: range-read (descent RAZ na liść)
`gh2_fs_read` (ścieżka per-blok) używa **`gh2_btree_iterate_range`** (już istnieje, dobrze
przetestowany — readdir) by przejść WSZYSTKIE itemy EXTENT_DATA w zakresie odczytu w JEDNYM
descenscie + in-order po liściach, zamiast re-descentu per blok. ~256× mniej descentów dla
odczytu sekwencyjnego.

## gh2_fs_read / gh2_fs_read_subvol (ścieżka bez --compress)
- Klamruj do inode.size (jak dotąd). **memset bufor wyjściowy 0** (dziury = zera domyślnie).
- `gh2_btree_iterate_range(dev, &fs_root, min, max, cb, ctx)` gdzie
  min=(ino, GH2_EXTENT_DATA, align_down(off)), max=(ino, GH2_EXTENT_DATA, align_down(off+len-1)).
- callback per item EXTENT_DATA: dekoduj `gh2_extent`; `data_block_read` (deszyfr.+weryfikuj csum
  → dup/-EIO jak dotąd); skopiuj przecięcie [off,off+len) ∩ [item.file_off, item.file_off+blok) do
  bufora na pozycji (item.file_off - off + boff). (Klamra do size + memset dziur daje bajt-exact
  z dotychczasowym per-blok read.) Błąd odczytu bloku → przerwij iterację (-EIO/-błąd).
- Zwróć min(len, size-off). Atomowość: read-only (bez mutacji/commit).
- Ścieżka --compress (chunk) BEZ ZMIAN (chunk_read już czyta 8-blokowe chunki — grubsze).

## Testy (`tests/test_v2bread.c` + istniejące)
1. **Round-trip bajt-exact (BRAMKA):** odczyt == zapis dla DOWOLNYCH danych/rozmiarów/offsetów
   niewyrównanych/przez granice liści/dziury(=zera)/częściowy ostatni blok/plik > 1 liść
   (wiele poziomów). Porównaj z modelem referencyjnym ORAZ ze starą ścieżką per-blok (te same bajty).
2. csum: przekręć bit disk_block → read -EIO (dup=0); dup OK → odczyt+repair (jak dotąd).
3. **Dowód:** odczyt sekwencyjny N bloków → liczba descentów/odczytów WĘZŁÓW ≈ liczba liści (NIE ≈N).
4. Sparse: zapis na offsecie (dziura) → odczyt dziury = zera; odczyt poza size = obcięty/0.
5. Interakcje: read na zaszyfrowanym/dup/snapshot (read_subvol) — bajt-exact; po batch-WRITE; po truncate.
6. Regresja WSZYSTKIE v2 0 failed (v2fs/v2comp/v2snap/v2heal/v2enc/v2xattr round-trip); ASan; brak mutacji (mapa/refcount niezmienione przez read).

## Świadome ograniczenia
- Tylko ścieżka per-blok (default+benchmark). --compress chunk-read już grubszy.
- Losowy odczyt (różne liście) batch nie pomaga — to osobny lewar (read-side node cache).
