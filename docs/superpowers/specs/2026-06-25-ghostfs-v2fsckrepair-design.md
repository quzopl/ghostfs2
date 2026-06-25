# ghostfs v2 — fsck --repair: design

**Data:** 2026-06-25 **Część:** v2 follow-up (analog v1 N, ale dla drzewa v2).

## Co naprawiamy (to co gh2_fsck wykrywa)
W CoW-FS mapa/refcount odbudowywane mark-sweep przy mount (zawsze spójne), więc naprawialne są
problemy POZIOMU DRZEWA:
1. **Sieroty** — INODE_ITEM nieosiągalny z roota → zwolnij i-węzeł (INODE_ITEM + EXTENT_DATA/chunk
   + XATTR_ITEM + SYMLINK_DATA + jego DIR_ITEM jeśli katalog).
2. **Wiszące DIR_ITEM** — wpis → nieistniejący i-węzeł (!is_inode[child]) → usuń wpis.
3. **Zły nlink** — ustaw na poprawny (katalog: 2+podkatalogi; plik: liczba wpisów wskazujących).

## API
`gh2_fsck(fs, int repair, int *issues)` (dodaj param repair; dziś read-only):
- detekcja jak dotąd (zlicza is_inode/reachable/nlink/links_seen/subdirs);
- jeśli `repair` → pass naprawczy w JEDNEJ transakcji (savepoint na początku, na lokalnym fs_root):
  (a) iteruj DIR_ITEM, usuń wpisy o child gdzie !is_inode[child] (wiszące);
  (b) zwolnij sieroty (is_inode && !reachable) — gh2_inode_free-podobne (wszystkie itemy i-węzła);
  (c) popraw nlink i-węzłów (is_inode && reachable && mismatch) → update INODE_ITEM;
  przy błędzie → rollback (fs_root nietknięty), zwróć błąd; sukces → fs->fs_root=nowy.
  `*issues` = liczba wykrytych (przed naprawą). repair=0 → bez zmian (read-only, jak dotąd).
- Po `gh2_fsck(repair=1)` wołający robi `gh2_fs_commit` (trwałość; jak v1 N).

## CLI/fasada
- gfs_fsck(g, repair, *issues): v2 → gh2_fsck(repair); v1 → gh_fsck(repair) (jest).
- cli.c cmd_fsck: USUŃ blokadę „v2 read-only"; v2 + --repair → gfs_fsck(repair=1) + gfs_sync;
  komunikat „(naprawiono)". 

## Testy (`tests/test_v2fsckrepair.c`)
1. **Wstrzyknij niespójność** (surowo/seam): wiszący DIR_ITEM (wpis→ino bez INODE), sierota
   (INODE_ITEM bez DIR_ITEM), zły nlink → gh2_fsck issues>0; `gh2_fsck(repair=1)` + commit →
   **remount → gh2_fsck issues==0 (TRWAŁE)**; struktura poprawna (sierota zwolniona, wiszący
   usunięty, nlink poprawny); refcount==mark-sweep, wyciek bloków=0 (sierota zwolniła bloki).
2. Atomowość: błąd w trakcie repair (ENOSPC) → fs_root/mapa nietknięte (brak częściowej naprawy).
3. Repair na CZYSTYM FS → issues==0, nic nie zepsute (idempotentne).
4. Realistyczny: zbuduj FS, wstrzyknij kilka niespójności różnych typów → repair → fsck==0,
   dane pozostałych (zdrowych) plików nienaruszone bajt-exact.
5. CLI: fsck --repair na uszkodzonym v2 → naprawia, świeży montaż fsck 0 niespójności.
6. Regresja: v1 fsck --repair (N) + v2 read-only fsck nietknięte. ASan czyste.
