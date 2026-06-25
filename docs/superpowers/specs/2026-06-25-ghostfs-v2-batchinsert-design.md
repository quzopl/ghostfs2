# ghostfs v2 — batch-insert (zapis sekwencyjny): design

**Data:** 2026-06-25 **Część:** v2 optymalizacja (diagnoza z benchmarkv2.md po ncache).

## Problem
Po write-back cache (ncache) zapis LOSOWY skoczył 5,5× (przebił v1), ale zapis SEKWENCYJNY spadł
−46%. Przyczyna: `gh2_fs_write` (ścieżka per-blok, kontener bez --compress) pisze **blok-po-bloku
i CoW-uje CAŁĄ ścieżkę B-drzewa na KAŻDY 4 KB**. Zapis 1 MB (256 bloków) = 256 przejść+CoW ścieżki.
Z cache to ogromny churn w pamięci (alloc+memcpy 4 KB+hash ×256 na 1 MB). Random tego nie czuje
(1 blok = 1 ścieżka).

## Rozwiązanie: bulk-insert (CoW ścieżki RAZ na liść)
Dodaj **additive** `gh2_btree_insert_run` — wstawia POSORTOWANY ciąg itemów, schodząc do liścia
RAZ i wstawiając wszystkie itemy danego liścia w JEDNYM CoW (split gdy liść pełny). **Istniejący
`gh2_btree_insert` (single) NIETKNIĘTY** → 8 mln property-checks dalej ważne. `gh2_fs_write`
(ścieżka per-blok) zapisuje bloki danych (write-through jak dotąd, zbiera itemy ekstentów), potem
wstawia WSZYSTKIE itemy jednym `insert_run` → ścieżka CoW raz na liść, nie na blok. ~100× mniej
CoW węzłów dla zapisu sekwencyjnego.

## API B-drzewa (additive)
```c
struct gh2_kv { struct gh2_key key; const void *val; uint32_t len; };
int gh2_btree_insert_run(struct gh_dev*, struct gh2_alloc*, const struct gh2_bptr *root,
                         uint64_t gen, const struct gh2_kv *items, uint32_t n,
                         struct gh2_bptr *out_root);
```
- `items` POSORTOWANE rosnąco po kluczu, UNIKALNE (caller gwarantuje). Każdy item: insert lub
  update (gdy klucz istnieje). CoW: descend do liścia items[0]; wstaw itemy należące do tego liścia
  (key < klucz-rozdzielający następnego rodzeństwa) w jednym buforze; split gdy przepełniony
  (promuj, CoW ścieżki); przejdź do następnego liścia dla pozostałych itemów. Zwróć nowy root.
- GWARANCJA: `insert_run(root, items, n)` daje **identyczną ZAWARTOŚĆ** (te same klucze/wartości) +
  **poprawne niezmienniki B-drzewa** co n kolejnych `gh2_btree_insert`. STRUKTURA identyczna dla
  ŚWIEŻEGO drzewa / append sekwencyjnego; przy grow-update do istniejących liści granice liści mogą
  się różnić (greedy repacking), ale drzewo pozostaje poprawne — dla CoW B-drzewa kształt nie wpływa
  na poprawność. Wartości ≤ GH2_LEAF_MAX_VAL.

## gh2_fs_write (ścieżka per-blok, bez --compress)
- Dla zapisu: pętla po blokach — alokuj blok danych (CoW), zapisz dane (gh_disk_write), zbuduj
  item EXTENT_DATA(ino, file_off) z csum; jeśli NADPISANIE istniejącego bloku → free starego
  (defer/immediate jak dotąd). ZBIERZ wszystkie itemy (posortowane po file_off — naturalnie
  rosnące). Potem JEDEN `gh2_btree_insert_run` wszystkich itemów → nowy root. (Zamiast 256×
  write_block z osobnym CoW.) inode.size/mtime jak dotąd. Atomowość per-op (savepoint/rollback).
- Bufor itemów: ograniczony (np. wsadami po ≤K itemów jeśli zapis ogromny — pamięć), reszta
  kolejnym insert_run. Bloki danych pisane od razu (write-through).
- Ścieżka --compress (chunk) bez zmian (chunki 8-blokowe — mniej churnu; opcjonalnie później).
- Odczyt/truncate/unlink bez zmian.

## Testy
1. **insert_run ≡ single (BRAMKA, property-based):** dla losowych posortowanych ciągów (różne
   rozmiary, wymuszające split liścia, wiele liści, wartości zmiennej długości) drzewo z
   `insert_run` == drzewo z n `gh2_btree_insert` (iteracja bajt-exact wszystkich itemów + te same
   niezmienniki: key[0]=min, posortowanie, brak przepełnień); wyciek bloków=0; CoW (stary root ważny).
2. **gh2_fs_write korektność:** round-trip bajt-exact (rozmiary sub-blok/blok/wielo-blok/niewyrów.
   /dziury/granice); NADPISANIE (CoW, stary blok zwolniony); RMW częściowy; truncate/unlink po. 
3. **Dowód:** zapis sekwencyjny N bloków → liczba CoW WĘZŁÓW ≈ liczba liści (NIE ≈N); insert_run
   wołane O(rozmiar/itemów-na-liść) razy.
4. **Crash-sweep** (test_v2crash) fsck==0; refcount==mark-sweep; wyciek=0; ncache redukcja (random)
   zachowana; regresja WSZYSTKIE v2 0 failed; ASan.
5. **Random niezmieniony:** losowy zapis 1-blokowy → insert_run z 1 itemem == single insert (zysk random zachowany).

## Świadome ograniczenia
- Bulk-insert tylko dla zapisu danych per-blok (najczęstszy + benchmark). Dir/xattr/chunk single.
- insert_run additive — nie rusza single-insert (crown). Wartości małe (≤ pojemność liścia).
