# ghostfs — pod-projekt M: leniwy `flush` (wydajność na wolnym flashu/USB)

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–L ✓ → **M (leniwy flush)**.

## Problem

Na wolnym nośniku (pendrive USB) kopiowanie nawet maleńkiego pliku trwa minuty. Przyczyna:
`gf_flush` (handler FUSE wołany przy **każdym `close()`**) wymusza pełny **trwały commit
dziennika** — `gh_jrnl_flush` z **4× `fsync`** + dodatkowy `fsync(fd)`. Na taniej pamięci
USB pojedynczy `fsync` trwa sekundy (brak podtrzymywanego cache, prymitywny FTL,
synchronizacja kasowania/programowania flasha), więc ~5 fsynców na każdy zamknięty plik =
dziesiątki sekund–minuty. To **niweczy group commit (I)** — każde `cp` płaci pełnym kosztem
dziennika, zamiast batchować.

POSIX **nie wymaga** trwałości przy `close()` — tylko przy `fsync`/`fdatasync`. Wymuszanie
trwałości na każdym zamknięciu jest więc zbędne.

## Rozwiązanie

### Zmiana 1 (główna): leniwy `flush`

`gf_flush` (FUSE, na `close()`) przestaje wymuszać trwały commit — staje się **no-op
(`return 0`)**. Dane pozostają w bieżącej (running) transakcji w pamięci i są trwale
zapisywane (`gh_jrnl_flush`) jak dotąd na:
- **`fsync`/`fdatasync`** aplikacji (`gf_fsync` → `gh_fs_sync`),
- **zapełnienie bufora** (`txn_begin` przy `n > cap/2`),
- **odmontowanie** (`gh_fs_unmount`).

Skutek: `cp` wielu plików batchuje się w jedną transakcję; trwały zapis (z fsyncami)
następuje raz — przy fsync/pojemności/unmount — zamiast na każdym zamknięciu. Maleńki plik
kopiuje się w milisekundy.

**Read-your-writes zachowane:** odczyty w obrębie montażu widzą bufor running txn — plik
zapisany i zamknięty (bez fsync) jest natychmiast czytelny w tym samym montażu; trwały na
dysku dopiero po commit.

### Zmiana 2 (pomniejsza): redukcja `fsync` w commit

`gh_jrnl_flush` ma 4 fazy z `fsync`: (1) dziennik, (2) nagłówek committed=1 [punkt
zatwierdzenia], (3) checkpoint, (4) wyczyszczenie nagłówka. **Faza 4 (czyszczenie nagłówka)
nie wymaga `fsync`**: jeśli po awarii nagłówek pozostanie `committed=1`, recover **ponownie
odtworzy** transakcję (idempotentnie — te same obrazy na te same cele), a **CRC dziennika
(J)** chroni przed odtworzeniem rozdartej paczki. Zostawiamy zapis wyczyszczonego nagłówka,
ale **bez `fsync`** (leniwe czyszczenie). 3 fsynce zamiast 4 w `gh_jrnl_flush`. (Przy
jawnym `fsync` końcowy `fsync(fd)` z `gh_fs_sync` i tak utrwala wyczyszczony nagłówek.)

## Semantyka trwałości (świadoma, POSIX)

- Operacja zwraca sukces, ale jest trwała dopiero po `fsync`/pojemności/`unmount` — zgodne
  z POSIX (już od pod-projektu I; M tylko przestaje fałszywie wymuszać trwałość przy close).
- **Crash-consistency niezmieniona:** awaria między commitami → utrata niezflushowanej
  paczki, FS spójny w stanie ostatniego flush (recover). `unmount`/`fusermount3 -u` flushuje
  — czyste odmontowanie jest trwałe.
- **Praktyka:** przed wyciągnięciem pendrive'a należy **odmontować** (lub `sync`) — standard
  dla każdego FS na USB.

## Strategia testowania

1. **Crash-consistency (bramka dla Zmiany 2):** `tests/test_crash.c` — sweep awarii N=1..40
   musi dalej dawać `fsck==0` (atomowość paczki mimo leniwego czyszczenia nagłówka; CRC +
   idempotentny recover).
2. **Trwałość przez unmount (integracja):** zapisz plik przez FUSE, **bez** jawnego
   `fsync`, odmontuj, zamontuj ponownie → plik obecny i poprawny (unmount flushuje).
3. **Trwałość przez fsync:** zapisz plik, `sync`/`fsync`, „symuluj utratę pamięci" przez
   kill procesu FUSE bez unmount (jak istniejący test kill -9), remount → po `fsync`
   plik trwały; bez `fsync` najwyżej utracony, ale **`fsck` czysty** (spójność).
4. **Regresja:** wszystkie testy A–L `0 failed`; `make test-asan` = 0; integracja FUSE +
   urządzenie blokowe zielone (w tym kill -9 + recover → fsck czysty).
5. **Wydajność (informacyjnie):** kopiowanie wielu plików przez FUSE bez jawnego fsync jest
   wyraźnie szybsze (jeden commit na unmount zamiast na plik). Trudne do zmierzenia w CI
   (zależne od nośnika) — odnotowane.

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| `close()` (FUSE flush) | no-op, `0` (trwałość nie jest wymagana) |
| `fsync`/`fdatasync` | pełny `gh_jrnl_flush` + `fsync(fd)` (trwałe) |
| Błąd I/O wcześniejszego flushu pojemnościowego | zgłaszany przez kolejną operację (sticky, od I) |
| Awaria między commitami | utrata niezflushowanej paczki; FS spójny (recover) |

## Świadome ograniczenia M (YAGNI)

- Brak okresowego timera flush (np. co 5 s) — flush na fsync/pojemność/unmount. Timer
  (wątek w tle) ograniczyłby okno utraty danych przy długim montażu bez fsync; możliwe
  rozszerzenie. (FUSE i tak nie wymusza trwałości przy close.)
- Aplikacje niewołające `fsync` tracą więcej przy nagłym odłączeniu niż w modelu
  commit-per-close — świadomy kompromis wydajność/trwałość (jak we wszystkich poważnych
  FS; dlatego „bezpieczne usuwanie/odmontuj" istnieje).
- Pełna optymalizacja USB (większe I/O, mniej zapisów losowych, journaling logiczny zamiast
  fizycznego 2×) to osobne tematy.
