# ghostfs v2 — xattr: plan

> BRAMKA: set/get/list/remove round-trip, zwalnianie przy unlink (wyciek=0, fsck spójne),
> persystencja, integracja setfattr/getfattr przez FUSE, regresja v1+v2. Spec: `...v2xattr-design.md`.
> Wzór pakowania: dir_add_entry_hashed (gh2_fs.c:568) + fnv1a64 (:77).

## Task 1: xattr v2 (core + fasada + FUSE/CLI + testy)
- [ ] `GH2_XATTR_ITEM`=7 (gh2_fs.h). Klucz (ino,7,fnv1a64(name)). Wartość: spakowana lista wpisów
  `{u16 name_len; u32 value_len; name[]; value[]}` (kolizje→pakowanie, jak dir). enkod/dekod/
  pack/unpack helpery (wzoruj na dir_item).
- [ ] `gh2_fs_setxattr(fs,path,name,val,size,flags)` (flagi CREATE=1/REPLACE=2 jak v1; -EEXIST/
  -ENODATA), `gh2_fs_getxattr`(→len; size0→rozmiar; -ENODATA; -ERANGE), `gh2_fs_listxattr`(nazwy
  null-sep; -ERANGE), `gh2_fs_removexattr`(-ENODATA; usuń item gdy pusty). Atomowość per-op
  (savepoint/rollback, na lokalnym root). Limit: name+value+nagłówki ≤ pojemność liścia → -E2BIG.
- [ ] Zwalnianie: w ścieżce usuwania i-węzła (unlink nlink→0, rmdir) usuń wszystkie (ino,7,*)
  range-scan + delete (jak free_extents). 
- [ ] Fasada gh2_vfs: gfs_setxattr/getxattr/listxattr/removexattr v2 → core (usuń -ENOTSUP).
- [ ] FUSE fuse_main.c: handlery setxattr/getxattr/listxattr/removexattr już wołają gfs_* —
  potwierdź że działają dla v2 (były ENOTSUP). CLI: opcjonalnie (FUSE wystarcza do setfattr).
- [ ] `tests/test_v2xattr.c` (+stdlib.h): set/get round-trip (rozmiary, binarne); list (null-sep);
  remove; -ENODATA; wiele xattr; CREATE/REPLACE; kolizja hash (pakowanie); size0→rozmiar; -ERANGE;
  -E2BIG za duże; **zwalnianie**: unlink z xattr→item usunięty (mapa==mark-sweep, wyciek=0, fsck0);
  persystencja remount; snapshot (xattr izolowane); +--compress/+szyfr round-trip. RUN_TEST.
- [ ] integration_v2.sh: setfattr/getfattr/getfattr -d/setfattr -x na zamontowanym v2 (FUSE).
- [ ] `make build/test_v2xattr && ./build/test_v2xattr` 0 failed; ASan; `make cli fuse`; regresja
  `make test` 0 failed; integration_v2.sh + integration.sh (v1 xattr) zielone. Commit.

## Self-Review
Pokrycie: items+pack (T1); set/get/list/remove (T1); zwalnianie (T1); fasada+FUSE (T1); testy
+integracja. Ryzyka: (1) pakowanie/kolizje (jak dir); (2) zwalnianie przy unlink (wyciek/sieroty
xattr); (3) atomowość per-op; (4) -ERANGE/-ENODATA/-E2BIG errno; (5) regresja v1 xattr + nie-xattr v2.
