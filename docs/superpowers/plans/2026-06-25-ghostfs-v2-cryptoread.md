# ghostfs — szybsze deszyfrowanie: plan

> BRAMKA: bajt-exact (nowy xts == stary), thread-safe (N wątków równolegle bajt-exact), re-key raz
> na wątek (dowód licznik), regresja crypto+v1enc+v2enc 0 failed, at-rest zachowane, ASan/UBSan brak
> wycieku. Spec: `...v2-cryptoread-design.md`.

## Task 1: thread-local EVP context w xts + testy
**Files:** Modify `src/crypto.c` (xts → TLS ctx); Test `tests/test_cryptoreuse.c`.
- [ ] W `src/crypto.c`: TLS `EVP_CIPHER_CTX*` + 64-bit hash klucza, `pthread_key_create`
  (pthread_once) z destruktorem `EVP_CIPHER_CTX_free`. `xts(c,blkno,in,out,enc)`: pobierz TLS ctx
  (lazy create); hash klucza c->key; jeśli != zapisany → `EVP_<enc>Init_ex(ctx, EVP_aes_256_xts(),
  NULL, c->key, NULL)` (re-key), zapisz hash; tweak z blkno (LE, jak dotąd); `EVP_<enc>Init_ex(ctx,
  NULL,NULL,NULL, tweak)`; `EVP_<enc>Update(ctx,out,&outl,in,GH_BLOCK_SIZE)`; `EVP_<enc>Final_ex`.
  Zwróć 0/−EIO. Fallback (init fail) → stara ścieżka new+init+free. Link `-lpthread` (sprawdź Makefile — już jest dla v1).
- [ ] `tests/test_cryptoreuse.c` (+pthread): bajt-exact nowy==stary (zachowaj ref-impl per-blok
  init w teście do porównania); round-trip; wiele bloków różne tweaki; zmiana klucza w wątku;
  **N wątków równolegle** (np. 8) enc/dec różne bloki → bajt-exact (brak wyścigu); dowód re-key=1
  dla N bloków/wątek (licznik test-only via wrapper lub zliczanie). RUN_TEST.
- [ ] `make build/test_cryptoreuse && ./build/test_cryptoreuse` 0 failed; ASan+UBSan (detect_leaks=1,
  +TSan osobno jeśli łatwo) 0 failed; **regresja: test_crypto, v1 enc (test_fs zaszyfrowany jeśli jest),
  test_v2enc, `make test` 0 failed**; at-rest (marker nieobecny w surowym kontenerze — istniejący test).
  Commit.

## Self-Review
Ryzyka: (1) bajt-exact (re-tweak-only == pełny init — ten sam klucz+tweak; BRAMKA porównanie z
ref-impl); (2) thread-safety (ctx per wątek, brak współdzielenia — N-wątkowy test); (3) zmiana klucza
(hash detekcja re-key); (4) wyciek (pthread_key destruktor — ASan); (5) regresja v1+v2 enc (wspólne
crypto); (6) fallback przy init-fail (poprawność).
