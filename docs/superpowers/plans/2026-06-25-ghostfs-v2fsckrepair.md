# ghostfs v2 — fsck --repair: plan

> BRAMKA: naprawa trwała po remount (fsck==0), atomowość, dane zdrowe nienaruszone, regresja.
> Spec: `...v2fsckrepair-design.md`. Wzór: v1 sub-projekt N (gh_fsck repair).

## Task 1: gh2_fsck repair + CLI + testy
- [ ] `gh2_fsck(fs, int repair, int *issues)` (gh2_fs.c/.h): dodaj param. Po detekcji, jeśli repair:
  savepoint (gh2_txn_alloc_mark) + lokalny `root=fs->fs_root`; (a) iteruj DIR_ITEM (callback) usuń
  wpisy z !is_inode[child] (dir_remove_entry na &root); (b) dla is_inode&&!reachable → free i-węzeł
  (INODE_ITEM+ekstenty/chunki+XATTR+SYMLINK_DATA+własne DIR_ITEM jeśli dir) na &root; (c) is_inode&&
  reachable&&nlink!=expect → update INODE_ITEM nlink na &root; błąd→gh2_txn_alloc_rollback+zwróć;
  sukces→fs->fs_root=root. *issues=wykryte przed naprawą.
- [ ] gfs_fsck(g,repair,*issues): v2→gh2_fsck(repair). cli.c cmd_fsck: usuń blokadę v2-read-only;
  v2+--repair→gfs_fsck(1)+gfs_sync, komunikat „(naprawiono)".
- [ ] `tests/test_v2fsckrepair.c`: wstrzyknij wiszący/sierotę/zły-nlink (seam lub surowo) →
  fsck issues>0 → repair → **remount fsck==0 (trwałe)** + struktura/refcount/wyciek=0; atomowość
  (ENOSPC→nietknięte); repair czysty→0; realistyczny mix→fsck0+zdrowe dane bajt-exact. RUN_TEST.
- [ ] integration_v2.sh: fsck --repair na uszkodzonym v2 (jeśli da się uszkodzić przez CLI/seam).
- [ ] `make build/test_v2fsckrepair && ./build/test_v2fsckrepair` 0 failed; ASan; regresja `make
  test` 0 failed (v1 N + v2 read-only); `make cli`; integration zielone. Commit.

## Self-Review
Ryzyka: (1) naprawa TRWAŁA (commit) i remount fsck==0 = bramka; (2) atomowość (savepoint/rollback);
(3) zwalnianie sieroty obejmuje WSZYSTKIE itemy (ekstenty/chunki/xattr/symlink/dir) — brak wycieku;
(4) repair nie psuje zdrowych danych; (5) regresja v1 N.
