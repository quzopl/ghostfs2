# ghostfs v2 — read-side node cache: plan

> BRAMKA: wszystkie v2 0 failed (read-cache zwraca tę samą treść), spójność csum-keyed, self-healing
> zachowany, read-only, redukcja CRC-verify hot węzłów, ASan brak wycieku. Spec: `...v2-readcache-design.md`.

## Task 1: read-side node cache (csum-keyed) + testy
**Files:** Modify `src/v2/gh2_btree.{c,h}` (read-cache + gh2_node_read), `src/v2/gh2_fs.c` (init/free
przy mount/unmount), `src/block.h` (pole gh_dev->v2_rcache); Test `tests/test_v2rcache.c`.
- [ ] Struktura `gh2_rcache`: hash/LRU `block -> {uint8_t buf[4096]; uint32_t csum;}`, ograniczony
  (np. GH2_RCACHE_CAP=2048 wpisów, eksmisja LRU/clock). API: rcache_create/destroy/get(block,csum,buf)
  (zwraca 1 gdy hit i csum pasuje, kopiuje buf)/put(block,csum,buf). Pole `void *v2_rcache` w gh_dev.
- [ ] `gh2_node_read`: po sprawdzeniu v2_ncache (dirty, jest), DODAJ: jeśli `v2_rcache` i
  `rcache_get(rc, bptr->block, bptr->csum, buf)` → `gh2_node_validate(buf)` RETURN (pomiń disk+CRC).
  Inaczej istniejąca ścieżka (gh_disk_read + gh_crc32==csum + dup/repair); po UDANEJ weryfikacji
  (block lub po repair) → `rcache_put(rc, bptr->block, bptr->csum, buf)`. (dup_block: opcjonalnie też put.)
- [ ] `gh2_fs_mount`: rcache_create → dev->v2_rcache; `gh2_fs_unmount`: rcache_destroy + NULL.
- [ ] `tests/test_v2rcache.c` (+stdlib.h): korektność (read przez cache == bez cache, round-trip
  bajt-exact); **spójność csum-keyed** (write węzeł→read(cache)→CoW(nowy blok, blok reused?)→read
  nowy=nowa treść; stary bptr→stara treść); **self-healing** (cache ma dobry, zbitrotuj disk→read=dobry;
  brak w cache→dup/repair; oba złe+brak→-EIO); **dowód** (licznik gh_crc32 węzłów dla N losowych
  odczytów << N×wysokość — górne węzły hit); read-only (mapa/refcount niezmienione); ASan brak wycieku.
  RUN_TEST.
- [ ] `make build/test_v2rcache && ./build/test_v2rcache` 0 failed; ASan; **regresja `make test` 0
  failed (wszystkie v2 — read-cache transparentny)**; crash-sweep fsck==0; read-only. Commit.

## Self-Review
Ryzyka: (1) csum-keyed spójność (stary blok reused → miss przez różny csum — BRAMKA test); (2)
self-healing zachowany (cache dobrej treści pomaga; dup/repair gdy miss); (3) read-cache transparentny
(wszystkie testy round-trip 0 failed); (4) read-only (brak mutacji); (5) wyciek pamięci (LRU bounded,
free przy unmount); (6) interakcja z dirty ncache (dirty pierwszy).
