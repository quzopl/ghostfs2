# ghostfs pod-projekt N — fsck repair durable: plan

**Goal:** Naprawy `fsck --repair` trwałe (przetrwać remount). Bug z group commit (I).

## Task 1 (jedyne)
- [x] `src/fs.c` `gh_fsck`: `if (repair) gh_jrnl_op_begin(&fs->dev);` na początku;
  `if (repair) gh_jrnl_op_commit(&fs->dev);` przed `return 0`.
- [x] `tests/test_fs.c` `test_fsck_repair_persists` (+RUN_TEST): uszkodzenie (nlink/sierota/
  wyciek) → fsck wykrywa → repair → UNMOUNT → REMOUNT → fsck==0; nlink naprawiony, sierota wolna.
- [x] `make test` + `make test-asan` `0 failed`; demo CLI (`fsck --repair` na uszkodzonym
  obrazie → świeży montaz fsck==0).
- [x] commit.
