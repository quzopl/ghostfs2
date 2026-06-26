# ghostfs — strojenie FUSE (domknięcie podatku FUSE): design

**Data:** 2026-06-26 **Część:** sterownik FUSE (`src/fuse_main.c`). Diagnoza z benchmarkv2.md:
jedyna luka v2 vs btrfs/ext4 to „podatek FUSE" na sekwencyjnych (~2,5–3×). User wybrał: najpierw
wycisnąć FUSE (przenośnie, niskie ryzyko) zanim rozważać moduł jądra.

## Stan obecny (już jest)
`gf_init` włącza: WRITEBACK_CACHE, SPLICE_READ/WRITE/MOVE, `max_write=1 MiB`. fuse_main (wysokopoziomowe,
domyślnie wielowątkowe). Global rwlock: odczyt rdlock (równoległy), zapis wrlock.

## Czego brakuje (szczery zakres)
1. **`max_read` NIE ustawiony** → jądro wysyła odczyty domyślnym rozmiarem (~128 KiB) zamiast 1 MiB
   → sekwencyjny odczyt robi ~8× więcej round-tripów userspace↔jądro. **Główny lewar dla COLD
   sekwencyjnego odczytu** (benchmark czyści cache, więc liczy się rozmiar żądania + prefetch).
2. **`max_readahead` niepodbity** → jądro prefetchuje mało; brak nakładania latencji FUSE na
   konsumpcję aplikacji. **Drugi duży lewar dla sekwencyjnego odczytu** (pipeline).
3. **Brak retencji page-cache dla re-odczytów** (`cfg->auto_cache`) — bezpieczne (auto-inwalidacja
   po mtime/size), pomaga REALNYM workloadom (nie benchmarkowi cold-single-pass).

## Świadome ograniczenie zakresu
- **Sekwencyjny ZAPIS już strojony** (max_write=1 MiB + writeback). Luka write vs btrfs to userspace
  data path (kopia+syscall per blok) — NIE do zbicia opcjami FUSE. Nie obiecujemy tu poprawy zapisu.
- Realny zysk: **sekwencyjny ODCZYT** (mniej round-tripów + prefetch). Random bez zmian (jądro tłumi
  read-ahead przy losowym). Liczby zmierzy user na NVMe.

## Zmiany (`src/fuse_main.c`)
1. `main`: WSTRZYKNIJ do argv FUSE `-o max_read=1048576` (1 MiB) — tylko jeśli user sam nie podał
   `max_read` (skan argv; jeśli podał, uszanuj). To ustawia rozmiar żądań odczytu w fuse_session_new.
2. `gf_init`: `conn->max_read = 1u<<20;` (zgodne z -o, wymagane przez libfuse3); `conn->max_readahead
   = 8u<<20;` (8 MiB prefetch — negocjowane z capable); `cfg->auto_cache = 1;` (retencja+koherencja).
   Zostaw istniejące writeback/splice/max_write. (NIE kernel_cache=1 — auto_cache bezpieczniejsze:
   inwaliduje po zmianie mtime/size, koherentne dla naszego FS.)
3. Komentarze: wyjaśnij że max_read tnie round-tripy odczytu, readahead pipeline'uje, auto_cache
   koherentny (single-writer + flock + auto-inwalidacja).

## Poprawność (kluczowe — to ścieżka mount/IO)
- max_read/max_write/max_readahead: tylko ROZMIARY żądań — semantyka bez zmian (gfs_read/write
  obsługują dowolny rozmiar/offset). Brak utraty/przekłamania danych.
- auto_cache: jądro cache'uje dane, inwaliduje po zmianie mtime/size (getattr) — koherentne, bo my
  jedyny pisarz (flock); read-after-write w obrębie mounta widzi świeże (mtime rośnie).
- Wstrzyknięcie -o max_read: nie może zepsuć montażu (poprawna składnia; respektuj user override).

## Testy
1. **Integracja (BRAMKA, /dev/fuse jest):** `tests/integration.sh` + `integration_v2.sh` PRZECHODZĄ
   bez zmian — mount, round-trip dużego pliku (bajt-exact), edycja in-place, katalogi, rm/mv/symlink/
   hardlink, truncate, df, kill-9+remount→fsck czysty. (Walidują że strojony mount działa poprawnie.)
2. **Round-trip dużego pliku** (sekwencyjny zapis+odczyt przez mount) bajt-exact — duży plik (≥64 MiB)
   by wymusić wielo-MiB odczyty z nowym max_read; sha256 zapis==odczyt.
3. **Re-odczyt (auto_cache koherencja):** zapisz plik, odczytaj, NADPISZ (przez mount), odczytaj
   ponownie → NOWA treść (nie stara z cache); auto-inwalidacja po mtime.
4. **Override:** mount z user `-o max_read=131072` → nie wstrzykujemy drugiego (respekt); mount działa.
5. Regresja: `make test` 0 failed (zmiana tylko fuse_main.c — rdzeń nietknięty); `make cli fuse` build OK.

## Świadome ograniczenia
- Zysk głównie sekwencyjny odczyt; zapis i tak przy ceiling FUSE. Moduł jądra = osobna decyzja (później).
- Nie ruszamy lockingu/rdzenia — czysto strojenie negocjacji FUSE.
