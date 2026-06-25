# ghostfs — pod-projekt E: utwardzenie i wiarygodność

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–D ✓ (raport 1–10) → **E (utwardzenie)** → F (wydajność) → G (funkcje odłożone).

## Cel

Podnieść zaufanie do istniejącego systemu plików, zanim dołożymy nowe funkcje. Trzy
filary: (1) `fsck` weryfikuje i naprawia całe drzewo (nie tylko mapę bloków), (2)
deterministyczny test atomowości po awarii (wstrzykiwany licznik zapisów — domyka punkt
ze strategii testów spec A, odłożony do B), (3) ciągła integracja (CI) i odporność na
uszkodzone kontenery.

## Część 1: `fsck` — pełna weryfikacja i naprawa drzewa

Dziś `gh_fsck` sprawdza wyłącznie spójność mapy bitowej bloków. Recenzje B/A wskazały, że
nie wykrywa złych `nlink` ani osieroconych i-węzłów. Rozszerzamy `gh_fsck` (ta sama
sygnatura `int gh_fsck(struct gh_fs*, int repair, int *issues)`):

Po dotychczasowym sprawdzeniu mapy bloków, dodatkowo:
1. **Osiągalność (DFS od roota):** przejdź drzewo katalogów od `root_inode`, podążając za
   wpisami (z pominięciem `.`/`..`), ze zbiorem odwiedzonych (rozmiar `inode_count`) i
   ochroną przed cyklami. Zbierz:
   - zbiór osiągalnych i-węzłów,
   - liczbę referencji dla i-węzłów plików/symlinków = liczba wpisów katalogowych (poza
     `.`/`..`) wskazujących dany i-węzeł.
2. **Niespójności (zliczane do `issues`):**
   - **Osierocony i-węzeł:** `type != GH_FREE`, nie jest rootem, nieosiągalny z drzewa.
   - **Zły `nlink` pliku/symlinka:** `n.nlink != liczba_referencji`.
   - (Katalogi: ten FS trzyma `nlink` katalogu jako stałą 2 z założenia — `nlink` katalogu
     NIE jest weryfikowany przeciw liczbie podkatalogów; weryfikujemy tylko osiągalność.)
3. **Naprawa (`repair != 0`):**
   - osierocony i-węzeł → zwolnij (`gh_inode_free`), potem przelicz mapę bloków jeszcze raz
     (osierocone i-węzły mogły trzymać bloki — kolejność: napraw i-węzły, potem mapę),
   - zły `nlink` → ustaw `n.nlink = liczba_referencji`, zapisz i-węzeł.
4. **Brak fałszywych alarmów na zdrowym FS:** po normalnych operacjach `nlink` == liczba
   referencji i brak sierot — istniejące testy z `fsck==0` muszą dalej przechodzić.

CLI `fsck <plik> [--repair]` już istnieje — po rozszerzeniu raportuje pełniejszy `issues`.

## Część 2: Wstrzykiwanie awarii — deterministyczna atomowość

Dodajemy do `struct gh_dev` pole licznika awarii:

```c
struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn;
                struct gh_cipher *cipher; long fail_after; };
```

- `fail_after == 0` → wyłączone (zachowanie bez zmian).
- `fail_after > 0` → w `gh_disk_write`: dekrementuj; gdy osiągnie 0 → zwróć `-EIO`
  (symulacja przerwanego zapisu/awarii) i pozostaw `fail_after == 0` (kolejne zapisy już
  przechodzą — symuluje pojedynczy punkt awarii). Inicjalizowane na 0 w
  `gh_dev_create`/`gh_dev_open`.

**Test (sweep „awaria po N zapisach"):** dla N = 1..K (pokrywając liczbę zapisów operacji):
- świeży kontener, zamontuj, ustaw `fs.dev.fail_after = N`, wykonaj operację modyfikującą
  (np. mkdir+create+write); operacja może zwrócić błąd (symulowana awaria w trakcie),
- zamknij, otwórz świeżo (`fail_after = 0`), zamontuj (uruchamia `gh_jrnl_recover`),
- **niezmienniki:** `gh_fsck` (z pełną weryfikacją) zwraca `issues == 0`; stan jest
  atomowy — operacja albo w pełni zaszła (plik z treścią), albo wcale (brak pliku); nigdy
  połowicznie.

To realizuje punkt „symulacja awarii: przerwanie po N zapisach + remount + fsck" ze
strategii testów spec A, którego pełna wersja była odłożona.

## Część 3: Odporność na uszkodzone kontenery + CI

- **Test odporności (bez clang/fuzzera):** podawaj zniekształcone wejścia do `gh_fs_mount`:
  plik za krótki, zerowy, ze złym `magic`, z `data_start`/`journal_start` poza plikiem,
  ze spreparowanym (uszkodzonym) nagłówkiem dziennika. Oczekiwanie: montowanie/recover
  **odmawia z błędem, nie crashuje** (zgodne z istniejącą walidacją; test to formalizuje).
  Plus pętla „losowy bajt-flip w skopiowanym zdrowym kontenerze → mount → nie crashuje
  (montuje albo odmawia)" pod ASan — tani fuzzing strukturalny.
- **CI (GitHub Actions):** `.github/workflows/ci.yml` — na push/PR: instalacja
  `libfuse3-dev`, `libssl-dev`, `attr`; `make test`; `make test-asan`; budowa `cli`/`fuse`.
  Integracja FUSE warunkowo (CI może nie mieć `/dev/fuse`) — jeśli niedostępna, pomiń krok
  montowania, ale uruchom testy jednostkowe + ASan (bramka).

## Strategia testowania (TDD + ASan)

1. `tests/test_fs.c` (rozszerzenie): fsck wykrywa zły `nlink` (ręcznie zepsuty i-węzeł →
   `issues>0`; `repair` naprawia → ponowny `fsck==0`); fsck wykrywa osierocony i-węzeł
   (zaalokowany bez wpisu → wykryty; repair zwalnia). Zdrowy FS → `issues==0` (regresja).
2. `tests/test_crash.c` (nowy): sweep awarii N=1..40 dla operacji; po recover `fsck==0` i
   atomowość. Plus test odporności na uszkodzone kontenery (truncated/zły magic/zły
   journal → mount odmawia, brak crasha).
3. Wszystkie istniejące testy A–D dalej `0 failed`; `make test-asan` = 0 failed.
4. CI uruchamia powyższe automatycznie.

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| fsck: osierocony i-węzeł | policz do `issues`; `repair` → zwolnij |
| fsck: zły `nlink` | policz do `issues`; `repair` → ustaw na liczbę referencji |
| fsck: cykl w drzewie (uszkodzenie) | ochrona zbiorem odwiedzonych; nie zapętlaj |
| mount: uszkodzony kontener/dziennik | odmowa z `-errno`, bez crasha |
| `fail_after` wyzwolony | `gh_disk_write` → `-EIO`, operacja wycofana przez warstwę wyżej |

## Świadome ograniczenia E (YAGNI)

- fsck nie weryfikuje `nlink` katalogów (z założenia stałe 2) — tylko osiągalność.
- Wstrzykiwanie awarii dotyczy zapisów (`gh_disk_write`); nie modeluje rozdarcia
  pojedynczego bloku (zakładamy atomowość zapisu bloku przez OS).
- Fuzzing strukturalny (bit-flip) bez pełnego silnika (clang/libFuzzer niedostępny) —
  tani, pętlowy wariant pod ASan; pełny fuzzing to przyszłe rozszerzenie.
- CI: integracja FUSE warunkowa (zależna od `/dev/fuse` w runnerze).
