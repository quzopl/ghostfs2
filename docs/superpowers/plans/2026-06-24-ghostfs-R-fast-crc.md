# ghostfs pod-projekt R — szybszy CRC: plan

## Task 1: slice-by-8 w gh_crc32_update
**Files:** Modify `src/csum.c`; Test `tests/test_csum.c`

- [ ] Step 1: w `src/csum.c` zamień `table[256]` na `t[8][256]`; w `build()` zbuduj `t[0]`
  (jak dotąd) i `t[k][i] = (t[k-1][i] >> 8) ^ t[0][t[k-1][i] & 0xFF]` dla k=1..7.
- [ ] Step 2: `gh_crc32_update` — slice-by-8 (8 bajtów/iter, little-endian: 4 bajty XOR do
  crc, 4 do hi, 8 lookupów `t[7][crc&0xFF]^...^t[0][hi>>24]`), ogon bajtowo przez `t[0]`.
  `gh_crc32` bez zmian (init/final XOR). Zachowaj `built`/`build()`.
- [ ] Step 3: test `test_crc_slice_matches` w `tests/test_csum.c` (+RUN_TEST): referencyjny
  CRC bajt-po-bajcie (stary algorytm inline) vs gh_crc32 dla długości {0,1,2,7,8,9,15,16,17,
  255,256,257,1000,4095,4096,4097} na danych pseudolosowych; CHECK_EQ; oraz
  CHECK_EQ(gh_crc32("123456789",9), 0xCBF43926u); oraz inkrementalność (split vs jeden call).
- [ ] Step 4: `make clean && make test && make test-asan` → 0 failed (test_csum, test_journal,
  test_crash, regresja A–Q).
- [ ] Step 5: commit.
