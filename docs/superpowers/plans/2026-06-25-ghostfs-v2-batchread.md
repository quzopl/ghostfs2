# ghostfs v2 — batch-read sekwencyjny: plan

> BRAMKA: round-trip bajt-exact (== stara ścieżka per-blok), dziury=zera, csum→EIO, redukcja
> descentów sekwencyjnego, regresja v2 0 failed, ASan, read nie mutuje. Spec: `...v2-batchread-design.md`.

## Task 1: range-read w gh2_fs_read + testy
**Files:** Modify `src/v2/gh2_fs.c` (gh2_fs_read/gh2_fs_read_subvol ścieżka per-blok); Test `tests/test_v2bread.c`.
- [ ] `gh2_fs_read_subvol` (i gh2_fs_read jeśli osobna) ścieżka bez --compress: zamień pętlę
  per-blok extent_lookup_at na: memset(buf,0,len); gh2_btree_iterate_range(min=(ino,EXTENT_DATA,
  align(off)), max=(ino,EXTENT_DATA,align(off+len-1)), cb, ctx); cb dekoduje extent, data_block_read
  (csum→dup/-EIO), kopiuje przecięcie do buf. Zwróć min(len,size-off). Klamra do size + memset dziur.
  Błąd w cb → propaguj (-EIO). (--compress bez zmian.)
- [ ] ctx: {fs, fs_root*, buf, off, len, err}. Uważaj: data_block_read w cb czyta bloki danych
  (gh_disk_read) — OK (read-only). iterate_range czyta węzły.
- [ ] `tests/test_v2bread.c` (+stdlib.h): round-trip bajt-exact różne rozmiary/offsety/dziury/granice
  liści/plik wielopoziomowy vs model; csum→EIO; sparse=zera; read poza size; dowód redukcji descentów
  (licznik gh_disk_read węzłów ≈ liście, nie ≈N); read po batch-write/truncate. RUN_TEST.
- [ ] `make build/test_v2bread && ./build/test_v2bread` 0 failed; ASan; **regresja `make test` 0
  failed (wszystkie v2 round-trip)**; read nie mutuje (mapa==przed); crash-sweep niezmieniony. Commit.

## Self-Review
Ryzyka: (1) round-trip bajt-exact (dziury=zera, częściowy blok, niewyrów. — memset+iterate_range+
size-klamra); (2) csum/dup/-EIO zachowane; (3) read read-only (brak mutacji); (4) redukcja descentów;
(5) reuzycie iterate_range (przetestowany) minimalizuje ryzyko; (6) --compress nietknięty.
