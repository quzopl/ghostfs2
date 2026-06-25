# ghostfs v2 — batch-insert: plan

> BRAMKA: insert_run ≡ N single inserts (cross-check property), gh2_fs_write round-trip bajt-exact,
> crash-sweep fsck==0, redukcja CoW węzłów dla sekwencyjnego, random zachowany, regresja v2 0 failed,
> ASan. Spec: `...v2-batchinsert-design.md`. single-insert (gh2_btree_insert) NIETKNIĘTY.

## Task 1: gh2_btree_insert_run + gh2_fs_write batch + testy
**Files:** Modify `src/v2/gh2_btree.{c,h}` (NOWA insert_run, single bez zmian), `src/v2/gh2_fs.c`
(gh2_fs_write per-blok batch); Test `tests/test_v2batch.c` + istniejące.
- [ ] `struct gh2_kv` + `gh2_btree_insert_run` (gh2_btree.{h,c}): posortowany ciąg, CoW ścieżki raz
  na liść, split przy przepełnieniu, promocja, nowy korzeń; insert/update per item. Reuzyj
  istniejące helpery liścia/split GDZIE BEZPIECZNIE bez zmiany single-insert. Wartość > limit → -EFBIG.
- [ ] `gh2_fs_write` (ścieżka bez --compress): zapisz bloki danych (write-through) + zbierz itemy
  EXTENT_DATA (posortowane, free starych przy nadpisaniu), JEDEN insert_run (wsady ≤K przy ogromnym
  zapisie). Atomowość per-op. (--compress bez zmian.)
- [ ] `tests/test_v2batch.c`: **cross-check insert_run≡N single** (property, losowe ciągi z splitami/
  wieloma liśćmi/zmienną długością — drzewo identyczne: iteracja bajt-exact + niezmienniki + wyciek0
  + CoW stary root); gh2_fs_write round-trip (rozmiary/dziury/nadpisanie/RMW); dowód redukcji CoW
  węzłów sekwencyjnego (≈liście, nie ≈N); random 1-item == single. RUN_TEST.
- [ ] `make build/test_v2batch && ./build/test_v2batch` 0 failed; ASan; **regresja `make test` 0
  failed (v2btree single-insert nietknięty, v2fs round-trip, v2ncache redukcja random, v2comp/snap/
  heal/enc/xattr/fsckrepair/crash)**; crash-sweep fsck==0; refcount==mark-sweep; wyciek=0. Commit.

## Self-Review
Ryzyka: (1) insert_run poprawność = cross-check ≡ single (BRAMKA — łapie split/promocję/multi-leaf);
(2) single-insert NIETKNIĘTY (8M property-checks ważne); (3) gh2_fs_write korektność (nadpisanie CoW,
dziury, RMW); (4) redukcja CoW węzłów sekwencyjnego (dowód); (5) random zachowany; (6) crash-sweep +
refcount==mark-sweep niezmienione.
