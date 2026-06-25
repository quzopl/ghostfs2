# ghostfs — pod-projekt K: katalogi haszowane

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–J ✓ → **K (katalogi haszowane)**.

## Cel

Usunięcie skalowania O(n) wyszukiwania/dodawania wpisu i O(n²) zapełniania katalogu.
Dziś katalog to liniowa tablica rekordów (272 B), skanowana w całości przy każdym
`lookup`/`add`. Katalog z tysiącami wpisów jest wolny; tworzenie N plików = O(n²). To
także istotna część narzutu z J (każdy `create` skanuje katalog, a każdy skan czyta bloki
z weryfikacją sumy). Wprowadzamy **haszowaną tablicę z otwartym adresowaniem** w pliku
katalogu → `lookup`/`add`/`remove` średnio O(1).

## Format katalogu (haszowany)

Plik katalogu to ciąg slotów o rozmiarze `sizeof(struct gh_dirent)` (272 B):

```
slot 0        = nagłówek (gh_dirhdr nałożony na rozmiar slotu)
sloty 1..M    = tablica haszowa (M = nslots)
```

```c
struct gh_dirhdr {            /* w slocie 0, dopełniony do rozmiaru dirent */
    uint64_t magic;          /* GH_DIRHDR_MAGIC */
    uint32_t used;           /* zajęte + tombstony (do load factor) */
    uint32_t nslots;         /* M */
};
#define GH_DIRHDR_MAGIC  0x4753484448495231ULL  /* "GSHDHIR1" */
```

- `nslots = dir.size / DENT_SZ - 1` (spójne z nagłówkiem `nslots`).
- Stany slotu `gh_dirent`:
  - **pusty:** `ino==0 && name_len==0`,
  - **tombston:** `ino==0 && name_len==GH_DIR_TOMB` (`0xFFFF`),
  - **zajęty:** `ino!=0`.
- Hash: `gh_crc32(name, name_len)`; slot startowy w tablicy = `hash % nslots`; sondowanie
  liniowe (`(h+p) % nslots`, mapowane na sloty pliku `1 + (...)`).
- Wpisy `.` i `..` to **zwykłe wpisy** tablicy haszowej (nie specjalne offsety).
- Wzrost: gdy `(used+1) > nslots*3/4` → `nslots *= 2`, **rehash** (przepisanie pliku:
  zebranie zajętych wpisów, nowy nagłówek+puste sloty, ponowne wstawienie haszowe;
  tombstony znikają). Wzrost amortyzuje O(1). Początkowo `GH_DIR_INIT_SLOTS = 16`.

Stałe: `#define GH_DIR_INIT_SLOTS 16`, `#define GH_DIR_TOMB 0xFFFFu`.

## Publiczne API (stabilne sygnatury + jedna nowa funkcja)

`gh_dir_add`/`gh_dir_lookup`/`gh_dir_remove`/`gh_dir_is_empty`/`gh_dir_iterate` —
**te same sygnatury**, implementacja haszowa. Dodajemy:

```c
int gh_dir_set_ino(struct gh_dev*, const struct gh_superblock*,
                   uint64_t dir_ino, const char *name, uint64_t new_ino);
```

— haszowo znajduje wpis i podmienia jego `ino` (dla `rename`: aktualizacja `..` oraz
`RENAME_EXCHANGE`). Zastępuje statyczne `update_dotdot`/`set_entry_ino` w `fs.c`.

- `gh_dir_lookup`: hash → sonduj; zajęty+dopasowanie → `out_ino`; pusty → `-ENOENT`;
  tombston/niedopasowanie → dalej. Propaguj `-EIO` z odczytu (korupcja, jak w J).
- `gh_dir_add`: `-ENAMETOOLONG` gdy `>255`; inicjalizuj tablicę gdy `size==0`; `-EEXIST`
  gdy istnieje; wzrost przy load factor; wstaw w pierwszy pusty/tombston (pusty → `used++`).
- `gh_dir_remove`: hash → sonduj → tombston; `-ENOENT` gdy brak.
- `gh_dir_is_empty`/`gh_dir_iterate`: iteruj sloty `1..nslots`, pomiń pusty/tombston;
  `is_empty` pomija `.`/`..`.

## Tworzenie katalogów (`super.c`, `fs.c`)

`.`/`..` dodawane przez `gh_dir_add` (nie bezpośredni `pwrite`):
- **`super.c` (root):** po utworzeniu i-węzła root (typ DIR, size 0) →
  `gh_dir_add(root, ".", root)` + `gh_dir_add(root, "..", root)`.
- **`fs.c` `make_node` (mkdir):** po `gh_inode_alloc` → `gh_dir_add(ino, ".", ino)` +
  `gh_dir_add(ino, "..", pino)`; potem ustaw `mode`/`nlink=2` na świeżo wczytanym i-węźle,
  zapisz; dodaj wpis w rodzicu. (`gh_dir_add` inicjalizuje tablicę przy pierwszym wpisie.)

`fs.c` `rename`: `update_dotdot`→`gh_dir_set_ino(sino,"..",npino)`;
`set_entry_ino`→`gh_dir_set_ino`. Usuń stare helpery liniowe.

## Kompatybilność

Nowa flaga `#define GH_SB_HASHDIR 0x4u` ustawiana przy formacie (informacyjna). Kod używa
haszowania zawsze. **Kontenery sprzed K (katalogi liniowe) wymagają ponownego formatu** —
świadoma niekompatybilność formatu katalogów (projekt przedprodukcyjny; testy formatują od
nowa). Mount nie wymusza flagi (root i tak istnieje).

## Strategia testowania (TDD + ASan)

1. `tests/test_dir.c`: add/lookup/remove haszowe (w tym kolizje — wiele nazw o tym samym
   `hash % nslots`); `-EEXIST`/`-ENOENT`; tombston + ponowne użycie slotu; **wzrost**
   (dodaj > INIT_SLOTS*3/4 wpisów → rehash → wszystkie nadal znajdowane); `is_empty`/
   `iterate` poprawne (pomijają `.`/`..`/tombstony); `gh_dir_set_ino`.
2. `tests/test_dir.c`: **skala** — dodaj 1000 wpisów, każdy `lookup` znajduje; usuń połowę,
   reszta znajdowana; iterate liczy poprawnie. (Sprawdza brak O(n²) — szybkie.)
3. `tests/test_fs.c`/integracja: tworzenie wielu plików w katalogu (np. 500) szybkie i
   spójne; `fsck==0`; `mkdir`/`rmdir`/`rename`/`readdir` poprawne; zagnieżdżone ścieżki.
4. **Regresja:** wszystkie testy A–J `0 failed` (`.`/`..`, path resolve, rename z `..`,
   EXCHANGE); `make test-asan` = 0; integracja FUSE (`ls`, `mkdir -p`, `mv`) + urządzenie
   blokowe zielone.
5. **Benchmark:** `test_bench` — tworzenie M plików w jednym katalogu powinno być wyraźnie
   szybsze niż przy katalogu liniowym (usunięcie O(n²) skanów; częściowo łagodzi narzut J).

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| `add` istniejącej nazwy | `-EEXIST` |
| `lookup`/`remove`/`set_ino` braku | `-ENOENT` |
| nazwa `>255` | `-ENAMETOOLONG` |
| korupcja bloku katalogu (suma J) | `-EIO` (propagowane) |
| tablica „pełna" mimo load factor | wzrost; w skrajności `-ENOSPC` (brak bloków) |

## Świadome ograniczenia K (YAGNI)

- Otwarte adresowanie z sondowaniem liniowym + wzrost ×2 (nie htree/B-tree). Proste,
  O(1) średnio; pełny htree (wielopoziomowy) = możliwe rozszerzenie dla ekstremalnych
  katalogów.
- Rehash przy wzroście wczytuje zajęte wpisy do pamięci (`malloc`) — O(zajętych) pamięci,
  rzadkie. Wariant blokowy = przyszłość.
- Brak zmniejszania tablicy przy masowym usuwaniu (tombstony reklamowane dopiero przy
  następnym wzroście). Akceptowalne.
- Format katalogów niekompatybilny z kontenerami sprzed K (reformat).
