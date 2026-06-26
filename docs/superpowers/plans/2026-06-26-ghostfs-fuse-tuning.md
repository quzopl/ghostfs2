# ghostfs — strojenie FUSE: plan

> BRAMKA: integration.sh + integration_v2.sh przechodzą (mount+round-trip bajt-exact), re-odczyt
> koherentny (auto_cache), user override max_read respektowany, regresja make test 0 failed, build OK.
> Spec: `...fuse-tuning-design.md`. Zmiana tylko `src/fuse_main.c`.

## Task 1: max_read + readahead + auto_cache + testy
**Files:** Modify `src/fuse_main.c` (gf_init + main arg injection); Test: rozszerz `tests/integration_v2.sh`.
- [ ] `gf_init`: dodaj `conn->max_read = 1u<<20;`, `conn->max_readahead = 8u<<20;`, `cfg->auto_cache = 1;`
  (zostaw writeback/splice/max_write). Komentarz PL wyjaśniający.
- [ ] `main`: przed budową fargv, skanuj argv[2..] za `max_read` (podciąg w którymkolwiek `-o ...`);
  jeśli BRAK → dołącz do fargv `"-o"` `"max_read=1048576"` (fargc+2). Jeśli user podał → nie dubluj.
  Alokacja fargv o 2 większa. Zachowaj przekazanie reszty argv.
- [ ] `tests/integration_v2.sh`: dodaj (a) round-trip DUŻEGO pliku ≥64 MiB przez mount: dd/losowe →
  put przez mount (cp do MNT), drop_caches NIE (brak root w CI — użyj sync), sha256 MNT==źródło;
  (b) re-odczyt: zapisz plik, odczytaj, nadpisz inną treścią, odczytaj → nowa treść (auto_cache koher.).
  ZACHOWAJ istniejące sprawdzenia.
- [ ] BRAMKA: `make cli fuse` build OK; `./tests/integration.sh` i `./tests/integration_v2.sh`
  PRZECHODZĄ (exit 0, wszystkie OK); `make test` 0 failed (rdzeń nietknięty); mount z `-o
  max_read=131072` (override) działa.
- [ ] Commit.

## Self-Review
Ryzyka: (1) wstrzyknięcie -o max_read poprawne składniowo, respekt override (skan argv); (2) max_read
init==mount (libfuse3 wymóg) — conn->max_read=1MiB == -o 1048576; (3) auto_cache koherentny (auto-inwal.
po mtime — re-odczyt test); (4) brak zmiany semantyki IO (tylko rozmiary); (5) integracja bajt-exact
(mount round-trip); (6) rdzeń/locking nietknięte.
