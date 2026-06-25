# ghostfs pod-projekt Q — strojenie FUSE: plan

## Task 1: callback .init
**Files:** Modify `src/fuse_main.c`; Test `tests/integration.sh` (bramka, bez zmian kodu testu)

- [ ] Step 1: dodaj `gf_init` (jak w spec) PRZED tablicą `ops`; potrzebne pola w
  `struct fuse_conn_info` są w `<fuse.h>` (już dołączony przez FUSE_USE_VERSION 31).
- [ ] Step 2: dodaj `.init = gf_init,` do `ops`.
- [ ] Step 3: `make fuse 2>&1 | tail` — kompiluje czysto (-Werror). Jeśli `fuse_config`
  nieużywany ostrzega — `(void)cfg;` już jest.
- [ ] Step 4: `make test` (rdzeń) → 0 failed (bez zmian).
- [ ] Step 5: `./tests/integration.sh` — WSZYSTKIE PRZESZŁY (round-trip, edycja in-place,
  kill-9+recover→fsck czysty, at-rest enc, współbieżność). To BRAMKA poprawności writeback/
  splice. Jeśli FAIL na writeback (rozmiar/mtime/danych) → usuń linię WRITEBACK_CACHE,
  zostaw splice+max_write, powtórz.
- [ ] Step 6: `./tests/integration_blockdev.sh` zielony (może wymagać dangerouslyDisableSandbox).
- [ ] Step 7: commit.
