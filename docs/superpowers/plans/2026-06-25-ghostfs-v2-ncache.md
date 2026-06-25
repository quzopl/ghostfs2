# ghostfs v2 — write-back cache węzłów: plan

> BRAMKA: wszystkie testy v2 0 failed (read-your-writes), crash-sweep fsck==0, refcount==mark-sweep,
> wyciek=0, ASan, DOWÓD redukcji zapisów węzłów. Spec: `...v2-ncache-design.md`. KRYTYCZNE: crash-
> consistency niezmieniona (superblok ostatni). Algorytm B-drzewa (split/merge/insert/delete) BEZ
> zmian — tylko node I/O (write/read/free) + commit + cache.

## Task 1: cache + write/read/free/commit/capacity + dowód
**Files:** Modify `src/v2/gh2_btree.{c,h}`, `src/v2/gh2_space.{c,h}` (free-this-txn), `src/v2/gh2_fs.c`
(commit flush, mount/unmount cache, capacity), `src/block.h` (pole cache na gh_dev jeśli tam);
Test `tests/test_v2ncache.c` + istniejące.
- [ ] Struktura cache: mapa block→bufor(4096) brudnych węzłów (hash/dyn), licznik. Inicjalizacja
  przy mount (gh2_fs_mount), zwolnienie przy unmount. Umieść dostępne dla gh2_node_read/write
  (np. pole na gh_dev: `void *v2_ncache`).
- [ ] `gh2_node_write`: alloc + WSTAW do cache (memcpy bufor), NIE gh_disk_write; out_bptr jak
  dotąd (csum z bufora). DUP: 2 bloki, obie kopie w cache.
- [ ] `gh2_node_read`: jeśli block w cache → memcpy z cache + validate; inaczej gh_disk_read +
  csum (+dup/read-repair). (dup_block też sprawdź w cache.)
- [ ] Free: cow_free/vtable — blok w cache → usuń z cache + immediate free (space_set wolny/
  ref_dec do 0 natychmiast, NIE defer); blok nie w cache (committed) → defer_dec jak dotąd.
- [ ] `gh2_fs_commit`: PRZED podmianą SB — flush cache (gh_disk_write wszystkich brudnych → +
  szyfr.) ; potem fsync → commit_super → txn_alloc_commit → WYCZYŚĆ cache. Kolejność barier zach.
- [ ] Capacity: gdy cache > próg (np. 4096 węzłów) podczas operacji → wymuś gh2_fs_commit (lub
  flush+SB-swap) by ograniczyć pamięć. (Operacje mutujące sprawdzają.)
- [ ] `tests/test_v2ncache.c`: read-your-writes (write→read z cache przed commit); persystencja
  (commit+remount); **dowód**: licznik gh_disk_write węzłów dla „1000 losowych zapisów+1 commit"
  << 1000×wysokość (pośrednie nie zapisane); capacity (długa txn→pamięć OK, fsck==0); crash w
  trakcie flushu cache (fail_after) → remount stary-albo-nowy, fsck==0. RUN_TEST.
- [ ] `make build/test_v2ncache && ./build/test_v2ncache` 0 failed; ASan; **regresja `make test`
  0 failed (wszystkie v2 — read-your-writes przez cache)**; **test_v2crash crash-sweep fsck==0**;
  refcount==mark-sweep; wyciek pamięci/bloków=0; `make cli fuse` czysto. Commit.

## Self-Review
Ryzyka: (1) crash-consistency — superblok ostatni po flushu; crash-sweep BRAMKA; (2) read-your-
writes przez cache (inaczej mutacje niewidoczne w tej samej txn); (3) immediate-free this-txn
węzłów (reuse) bez psucia refcount/mapy; (4) capacity bound (pamięć); (5) flush przy commit
kompletny (wszystkie brudne na dysk przed SB); (6) DUP/szyfr/snapshot semantyka niezmieniona;
(7) B-drzewo algorytm nietknięty.
