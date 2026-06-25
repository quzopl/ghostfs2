# ghostfs v2 — write-back cache węzłów: design

**Data:** 2026-06-25 **Część:** v2 optymalizacja (diagnoza z benchmarkv2.md).

## Problem
`gh2_node_write` jest **write-through** — każda mutacja CoW od razu `gh_disk_write` węzła na dysk.
Losowy zapis 4K = ścieżka CoW (liść + węzły wewn. + korzeń FS + korzeń-tree), każdy zapisywany
NATYCHMIAST. Dla 32768 zapisów korzeń przepisywany ~32768× (liczy się tylko drzewo finalne przy
commicie). ~6× amplifikacja zapisów węzłów (+ szyfr. + suma każdego) → benchmark: losowy zapis v2
~3× wolniejszy od v1, drogie szyfrowanie na losowym 4K.

## Rozwiązanie: write-back cache brudnych węzłów (technika CoW-FS jak btrfs)
Brudne węzły CoW trzymane W PAMIĘCI; zapis na dysk dopiero przy commicie. Mutacje re-CoW-ują
ścieżkę w pamięci (tanio); commit zapisuje FINALNE drzewo raz. **Węzły pośrednie (CoW nadpisany
przed commitem) NIGDY nie trafiają na dysk** — kluczowy zysk.

### Zachowanie (plumbing dowolny — cache na gh_dev/gh2_fs, per-montaż)
1. **gh2_node_write:** alokuj blok, **wstaw bufor węzła do cache (block→bufor 4096B), NIE
   gh_disk_write**. Ustaw out_bptr (block, csum, dup_block jeśli DUP — DUP: 2 bloki, obie kopie w
   cache). 
2. **gh2_node_read:** najpierw sprawdź cache (block w cache → memcpy z pamięci, zwróć — read-your-
   writes); inaczej gh_disk_read + weryfikuj csum (+ dup/read-repair jak dotąd).
3. **Free węzła (cow_free / vtable):** jeśli blok JEST w cache (this-txn, niezapisany, niezacommit.)
   → **usuń z cache + zwolnij blok NATYCHMIAST (reuse, refcount→0)**; jeśli blok zacommitowany
   (nie w cache) → defer_dec (jak dotąd, stary committed węzeł żyje do commitu). To eliminuje
   zapis pośrednich węzłów ORAZ pozwala reużyć ich bloki w tej samej transakcji.
4. **gh2_fs_commit:** **flush cache** (gh_disk_write WSZYSTKICH brudnych węzłów = finalne drzewo)
   → fsync danych (bariera) → podmiana superbloku (ping-pong) → fsync → txn_alloc_commit (defer
   committed-free) → **wyczyść cache**. (Superblok ostatni — atomowość bez zmian.)
5. **Capacity bound:** gdy cache > próg (np. N węzłów / M bajtów) → wymuś commit (jak v1 group-
   commit capacity flush) — ogranicza pamięć przy wielkich transakcjach (np. 32768 zapisów do
   różnych liści gromadzi brudne liście). Po capacity-commit cache wyczyszczony, węzły committed.

## Crash-consistency (NIEZMIENIONA — bramka)
Superblok pisany OSTATNI, po flushu brudnych węzłów (finalne drzewo durable). Awaria przed
podmianą SB → stary SB → stare (zacommitowane) drzewo, którego węzły są na dysku z poprzedniego
commitu; nowe brudne (część sflushowana, część nie) nieosiągalne ze starego SB → nieszkodliwe.
Awaria w trakcie podmiany SB (rozdarcie) → drugi slot (stary) ważny. Zawsze stary-albo-nowy.
**test_v2crash (crash-sweep N=1..M) MUSI dawać fsck==0** — to bramka.

## Interakcje
- **Dane (ekstenty/chunki):** pisane jak dotąd (gh_disk_write per-write; cache dot. WĘZŁÓW).
  Free starego bloku DANYCH this-txn: może zostać immediate (jeśli łatwo wykryć) lub defer (jak
  dotąd) — bez wpływu na poprawność; główny zysk to węzły.
- **Refcount/mark-sweep:** liczone z osiągalnych z superbloku (committed) drzew — niezmienione.
  Brudne węzły w cache mają refcount w mapie (alokowane). Przy commit→flush spójne. refcount==
  mark-sweep PO commicie/remount = bramka.
- **DUP/szyfrowanie/kompresja/snapshot:** bez zmian semantyki (DUP: obie kopie w cache→flush;
  szyfr. przy gh_disk_write na flushu; snapshot inc/dec na committed drzewie).

## Testy
1. **Korektność:** WSZYSTKIE istniejące testy v2 (v2fs/v2snap/v2comp/v2heal/v2enc/v2xattr/
   v2fsckrepair/v2btree) 0 failed — read-your-writes przez cache, persystencja po remount.
2. **Crash-sweep (BRAMKA):** test_v2crash N=1..M fsck==0, stan stary-albo-nowy (capacity-commit +
   final commit pokryte; awaria w trakcie flushu cache → stary SB → spójne).
3. **refcount==mark-sweep** po dowolnej sekwencji + remount; wyciek bloków=0.
4. **Dowód optymalizacji:** instrumentuj licznik gh_disk_write węzłów; workload „N losowych
   zapisów 4K do różnych pozycji + 1 commit" → liczba zapisów WĘZŁÓW << N×wysokość (write-through
   = N×~6; write-back = ~rozmiar_drzewa); pośrednie węzły nigdy zapisane.
5. Capacity bound: bardzo długa transakcja bez commitu → pamięć ograniczona (capacity-commit), fsck==0.
6. ASan/UBSan czyste; brak wycieku pamięci (cache zwolniony przy unmount).

## Świadome ograniczenia
- Cache węzłów w pamięci (per-montaż), ograniczony capacity-commitem. To NIE zbliża do ext4
  (FUSE/jądro strukturalne), ale odzyskuje główną naprawialną stratę v2-vs-v1 (losowy zapis).
- Dane pisane per-write (nieamortyzowalne — każdy blok inna treść).
