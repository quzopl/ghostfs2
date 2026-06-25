# ghostfs pod-projekt O — fast statfs: plan

## Task 1: gh_fs_statfs O(bloki) zamiast O(bitów)
**Files:** Modify `src/fs.c`; Test `tests/test_fs.c`.

- [ ] Step 1: w `gh_fs_statfs` zastąp pętlę free_blocks wersją keszującą blok mapy:
  czytaj `gh_block_read(bitmap_start + (b/8)/BS)` tylko gdy blok się zmienia; licz bity
  `!(bm[(b/8)%BS] & (1<<(b%8)))`. Zastąp pętlę free_inodes wersją keszującą blok tablicy
  i-węzłów; czytaj `type` przez `memcpy(ib + (i%INODES_PER_BLK)*INODE_SIZE, 2)`. (`string.h`
  jest w fs.c.)
- [ ] Step 2: test cross-check `test_statfs_fast` w test_fs.c (+RUN_TEST): sformatuj, utwórz
  kilka plików/katalogów; policz free_blocks/free_inodes BRUTE-FORCE (gh_bitmap_test per
  blok, gh_inode_read per i-węzeł) i porównaj z `gh_fs_statfs` — muszą być równe.
- [ ] Step 3: `make clean && make test && make test-asan` → 0 failed (test_statfs_fast,
  test_statfs_sync, regresja A–N).
- [ ] Step 4: commit (`src/fs.c tests/test_fs.c`).
