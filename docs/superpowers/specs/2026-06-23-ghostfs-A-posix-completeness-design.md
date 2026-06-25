# ghostfs — pod-projekt A: kompletność POSIX

**Data:** 2026-06-23
**Status:** zatwierdzony projekt (przed planem implementacji)
**Część większej całości:** dekompozycja braków z `raport` na 4 pod-projekty
(A: kompletność POSIX → B: journaling → C: szyfrowanie → D: współbieżność).
Ten dokument obejmuje **wyłącznie pod-projekt A**.

## Cel

Domknięcie realnych braków systemu plików ghostfs ponad to, co celowo pominięto
w v1. Po A zamontowany przez FUSE ghostfs zachowuje się jak zwykły system plików
przy codziennych operacjach: skracanie plików, `mv`, `df`, `chmod/chown`, czasy
plików dla `make`/`rsync`/`git`, trwałość zapisu (`fsync`), dowiązania
(symboliczne i twarde) oraz atrybuty rozszerzone (xattr).

Zakres odpowiada punktom **1–6 oraz 10** z `raport`. Punkty 7–9 (journaling,
szyfrowanie, współbieżność) to osobne pod-projekty (B, C, D).

## Zasada nadrzędna (bez zmian względem v1)

Cała logika w rdzeniu `libghostfs`; FUSE i CLI to cienkie nakładki na to samo
publiczne API rdzenia. Każda nowa operacja powstaje najpierw w rdzeniu (z testem
jednostkowym bez FUSE), a dopiero potem jest podpinana w FUSE i CLI. TDD
(test-najpierw) i AddressSanitizer obowiązują tak jak w v1.

## Zmiany formatu on-disk (kompatybilne wstecz)

### I-węzeł — nowe pole `xattr_block`

`struct gh_inode` ma dziś 88 B paddingu (pola sumują się do 168 z 256 B). Dodajemy
jedno pole 8-bajtowe **kosztem paddingu**, więc rozmiar i-węzła pozostaje 256 B:

```c
struct gh_inode {
    uint16_t type;            /* 0=wolny,1=plik,2=katalog,3=symlink */
    uint16_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t atime, mtime, ctime;
    uint64_t direct[GH_NDIRECT];
    uint64_t indirect;
    uint64_t double_indirect;
    uint64_t xattr_block;     /* NOWE: 0 = brak; inaczej numer bloku z xattr */
    uint8_t  pad[...];        /* padding pomniejszony o 8 B */
};
```

**Kompatybilność:** w starych kontenerach ten obszar był wyzerowanym paddingiem,
więc czyta się jako `xattr_block == 0` (brak atrybutów). Dlatego **nie zmieniamy
magic i nie wymagamy reformatu** — istniejące kontenery montują się dalej.

### Nowy typ i-węzła

```c
enum gh_itype { GH_FREE = 0, GH_FILE = 1, GH_DIR = 2, GH_SYMLINK = 3 };
```

Stałe pomocnicze: `GH_XATTR_MAX = GH_BLOCK_SIZE` (limit łącznej wielkości xattr na
i-węzeł), `GH_SYMLINK_MAX = GH_BLOCK_SIZE` (limit długości celu dowiązania).

## Nowe publiczne API rdzenia (`src/fs.h` / `src/fs.c`)

Wszystkie funkcje zwracają `0`/`-errno` (lub liczbę bajtów dla wariantów
`ssize_t`), spójnie z istniejącym API.

| Funkcja | Zachowanie |
|---|---|
| `int gh_fs_truncate(fs, path, uint64_t new_size)` | skrócenie: zwalnia bloki logiczne ≥ `ceil(new_size/BS)`, zwija nieużywane bloki pośrednie/podwójnie pośrednie, zeruje ogon ostatniego bloku częściowego; wydłużenie: tylko `size` rośnie (dziury czytane jako zera). Aktualizuje `size`, `mtime`, `ctime`. `EISDIR` na katalogu |
| `int gh_fs_utimens(fs, path, uint64_t atime, uint64_t mtime)` | ustawia `atime`/`mtime` w i-węźle, zapisuje |
| `int gh_fs_chmod(fs, path, uint16_t mode)` | ustawia `mode & 0777`, `ctime` |
| `int gh_fs_chown(fs, path, uint32_t uid, uint32_t gid)` | ustawia `uid`/`gid` (wartość `(uint32_t)-1` = nie zmieniaj), `ctime` |
| `int gh_fs_rename(fs, oldpath, newpath)` | patrz „Semantyka rename" |
| `int gh_fs_statfs(fs, struct gh_statfs *out)` | liczy wolne bloki (skan mapy od `data_start`) i wolne i-węzły (skan tablicy) |
| `int gh_fs_sync(fs)` | `fsync(fs->dev.fd)` — wymusza trwałość |
| `int gh_fs_symlink(fs, target, linkpath)` | tworzy i-węzeł `GH_SYMLINK`, zapisuje `target` jako treść; `ENAMETOOLONG` gdy `>GH_SYMLINK_MAX` |
| `ssize_t gh_fs_readlink(fs, path, char *buf, size_t size)` | czyta treść symlinka do `buf` (do `size`); `EINVAL` gdy nie-symlink |
| `int gh_fs_link(fs, oldpath, newpath)` | twardy link: nowy wpis katalogowy na ten sam i-węzeł, `nlink++`; `EPERM` gdy cel to katalog; `EXDEV` nie dotyczy (jeden kontener) |
| `int gh_fs_setxattr(fs, path, name, value, size, int flags)` | zapis atrybutu; `flags` honoruje `XATTR_CREATE`/`XATTR_REPLACE`; `ENOSPC` gdy przekroczono `GH_XATTR_MAX` |
| `ssize_t gh_fs_getxattr(fs, path, name, void *buf, size_t size)` | `size==0` → zwraca długość; inaczej kopiuje; `ENODATA` gdy brak; `ERANGE` gdy za mały bufor |
| `ssize_t gh_fs_listxattr(fs, path, char *buf, size_t size)` | lista nazw rozdzielonych `\0`; `size==0` → długość |
| `int gh_fs_removexattr(fs, path, name)` | usuwa atrybut; zwalnia `xattr_block` gdy pusty; `ENODATA` gdy brak |

Struktura wyniku statfs:

```c
struct gh_statfs {
    uint32_t block_size;
    uint64_t total_blocks, free_blocks;
    uint64_t total_inodes, free_inodes;
    uint32_t name_max;        /* GH_NAME_MAX */
};
```

## Nowe/zmienione funkcje warstwy i-węzłów (`src/inode.c`)

- **`int gh_inode_truncate(dev, sb, ino, struct gh_inode *node, uint64_t new_size)`** —
  rdzeń skracania/wydłużania. Skracanie: dla `lbn` od `new_nblocks` do
  `old_nblocks-1` mapuje (bez alokacji) i zwalnia liść; po przejściu zwalnia bloki
  indeksowe, które stały się zbędne (`indirect`, gałęzie `double_indirect`).
  Jeśli `new_size` nie jest wielokrotnością bloku — zeruje ogon ostatniego
  zachowanego bloku (POSIX: późniejsze wydłużenie daje zera). Ustawia `size`,
  `mtime`, `ctime` i zapisuje i-węzeł.
- **`gh_inode_free`** — rozszerzony: jeśli `xattr_block != 0`, zwalnia ten blok.

## Zmiany semantyki istniejących operacji (`src/fs.c`)

### Twarde linki: zliczanie `nlink`

`remove_node` (używane przez `unlink`) **dekrementuje `nlink`** i zwalnia i-węzeł
(`gh_inode_free`) **dopiero gdy `nlink` spadnie do 0**. Dla katalogów `rmdir`
zachowuje dotychczasową logikę (`nlink` katalogu nie jest używany do współdzielenia
treści). Usunięcie wpisu katalogowego następuje zawsze; zwolnienie i-węzła
warunkowo.

### Semantyka `rename`

1. Rozwiąż `oldpath` → `src_ino` (musi istnieć, inaczej `ENOENT`). Root → `EBUSY`.
2. Podziel obie ścieżki na (rodzic, nazwa); rozwiąż oba katalogi-rodziców.
3. Jeśli `newpath` istnieje (cel):
   - `src` to katalog → cel musi być katalogiem i pustym (`ENOTDIR`/`ENOTEMPTY`).
   - `src` to plik/symlink → cel nie może być katalogiem (`EISDIR`).
   - Usuń cel: wpis katalogowy + zwolnienie i-węzła wg reguły `nlink`.
4. Zabezpieczenie pętli: jeśli `src` to katalog, `newpath` nie może leżeć w
   poddrzewie `src` → `EINVAL`. Sprawdzenie idzie **po i-węzłach** (od nowego
   rodzica w górę po `..` do roota; trafienie `src_ino` = pętla), a nie po
   prefiksie ścieżki — odporne na końcowe/zwielokrotnione `/`.
5. Dodaj wpis `newname → src_ino` w nowym rodzicu; usuń stary wpis. Gdy usunięcie
   starego wpisu zawiedzie po udanym dodaniu — wycofaj nowy wpis (stan sprzed).
6. Jeśli `src` to katalog i zmienił się rodzic: zaktualizuj wpis `..` w `src` na
   nowy `pino`. **`nlink` rodziców NIE jest korygowany** — ten system plików
   nigdzie nie utrzymuje `nlink` katalogu jako licznika podkatalogów (`mkdir`
   ustawia stałe `nlink=2`, `rmdir`/usuwanie nie ruszają `nlink` rodzica,
   pustość liczona przez skan wpisów, nie `nlink`). Korekta rodziców w rename
   byłaby więc niespójna z resztą systemu, dlatego jej nie ma.

### `fsck` — osiągalność `xattr_block`

`gh_fsck` dolicza `n.xattr_block` (gdy ≠ 0) do mapy bloków oczekiwanych
(`mark`). Bloki danych symlinków są już pokryte przez `direct[]`/`indirect`,
bo symlink przechowuje cel jak zwykły plik.

## Format bloku xattr (1 blok na i-węzeł)

Sekwencja rekordów upakowanych od początku bloku:

```
[uint16 name_len][uint16 value_len][name bytes][value bytes] ...
```

Koniec listy = rekord z `name_len == 0` lub osiągnięcie końca bloku. Operacje:
- `set`: jeśli `xattr_block==0` i trzeba zapisać — alokuje blok (zerowany),
  zapisuje numer do i-węzła. Aktualizacja istniejącej nazwy = usuń+dopisz.
  Przepełnienie `GH_XATTR_MAX` → `ENOSPC`.
- `get`/`list`: liniowe przejście rekordów.
- `remove`: kompaktuje pozostałe rekordy; gdy lista pusta → zwalnia blok,
  zeruje `xattr_block`.

## Warstwa FUSE (`src/fuse_main.c`)

Realne handlery zamiast atrap (kolejność zwracania `-errno` bez zmian):

| Handler FUSE | Wywołuje |
|---|---|
| `truncate` | `gh_fs_truncate` (zamiast `return 0`) |
| `utimens` | tłumaczy `UTIME_NOW`/`UTIME_OMIT`, woła `gh_fs_utimens` |
| `rename` (flagi=0; `RENAME_EXCHANGE`/`RENAME_NOREPLACE` → `EINVAL`) | `gh_fs_rename` |
| `statfs` | `gh_fs_statfs` → wypełnia `struct statvfs` |
| `chmod` | `gh_fs_chmod` |
| `chown` | `gh_fs_chown` |
| `flush` / `fsync` | `gh_fs_sync` |
| `symlink` / `readlink` | `gh_fs_symlink` / `gh_fs_readlink` |
| `link` | `gh_fs_link` |
| `setxattr` / `getxattr` / `listxattr` / `removexattr` | odpowiednie `gh_fs_*xattr` |

`getattr` rozszerzony: dla `GH_SYMLINK` zwraca `S_IFLNK | mode`.

## Narzędzie CLI (`src/cli.c`)

Nowe komendy (każda otwiera kontener, woła rdzeń, zamyka):
`mv <plik> <stara> <nowa>`, `truncate <plik> <ścieżka> <rozmiar>`,
`ln <plik> <cel> <link>` (twardy), `lns <plik> <cel> <link>` (symbol.),
`chmod <plik> <ścieżka> <ósemkowo>`, `stat <plik> <ścieżka>`, `df <plik>`.

## Strategia testowania (TDD + ASan, jak w v1)

1. **`tests/test_inode.c`** — truncate: skracanie zwalnia właściwe bloki (po
   operacji `fsck` czyste), wydłużanie daje zera, zerowanie ogona bloku
   częściowego; truncate na pliku z indirect.
2. **`tests/test_fs.c`** — rozszerzenie: chmod/chown (pola+`ctime`), utimens,
   statfs (dokładne liczby wolnych bloków/i-węzłów po znanej sekwencji), rename
   (plik; katalog ze zmianą rodzica i aktualizacją `..`; nadpisanie pliku;
   nadpisanie niepustego katalogu → `ENOTEMPTY`; przeniesienie katalogu w jego
   poddrzewo → `EINVAL`). Po każdej grupie `fsck`=0.
3. **`tests/test_links_xattr.c`** (nowy) — symlink/readlink (round-trip,
   `ENAMETOOLONG`); twardy link (`nlink` rośnie, treść współdzielona, `unlink`
   jednego zostawia drugi, zwolnienie i-węzła dopiero przy `nlink==0`); xattr
   (set/get/list/remove, `XATTR_CREATE`/`XATTR_REPLACE`, `ERANGE`, `ENODATA`,
   przepełnienie → `ENOSPC`, zwolnienie bloku po usunięciu ostatniego). Po
   wszystkim `fsck`=0.
4. **`tests/integration.sh`** — rozszerzenie o realne narzędzia: skracanie przez
   `truncate`/`>`, `mv` (plik i katalog), `df -h` pokazuje rozmiar, `chmod`+`ls -l`,
   `ln -s`+odczyt celu, `setfattr`/`getfattr` (jeśli dostępne; inaczej pominięcie
   z komunikatem), trwałość po `fsync`.
5. **ASan** — wszystkie testy jednostkowe pod `-fsanitize=address,undefined`,
   zero raportów (nienegocjowalne).

## Obsługa błędów (errno) — uzupełnienia

| Sytuacja | Reakcja |
|---|---|
| `truncate`/`utimens`/`chmod`/`chown` na nieistniejącej ścieżce | `ENOENT` |
| `rename` katalogu w jego poddrzewo | `EINVAL` |
| `rename` nadpisanie niepustego katalogu | `ENOTEMPTY` |
| `rename` pliku na katalog (lub odwrotnie) | `EISDIR` / `ENOTDIR` |
| `link` na katalog | `EPERM` |
| `readlink` na nie-symlink | `EINVAL` |
| `getxattr`/`removexattr` brak nazwy | `ENODATA` |
| `getxattr` za mały bufor (`size>0`) | `ERANGE` |
| `setxattr` przekroczenie `GH_XATTR_MAX` | `ENOSPC` |
| `symlink` cel `>GH_SYMLINK_MAX` | `ENAMETOOLONG` |

## Świadome ograniczenia A (YAGNI)

- xattr i cel symlinka ograniczone do 1 bloku (4096 B) — pokrywa `PATH_MAX` i
  typowe atrybuty; wielo-blokowe xattr poza zakresem.
- `rename` obsługuje flagi `0`; `RENAME_EXCHANGE`/`RENAME_NOREPLACE` → `EINVAL`.
- Atomowość operacji wielokrokowych (rename, set/remove xattr) opiera się na
  dotychczasowej kolejności zapisów; pełna atomowość = pod-projekt B (journaling).
- Trwałość: `fsync` wymusza zapis na nośnik; brak bariery transakcyjnej do czasu B.
- `fsck` weryfikuje wyłącznie spójność mapy bitowej bloków (w tym osiągalność
  `xattr_block`); **nie** sprawdza `nlink` ani osieroconych i-węzłów (i-węzeł
  zaalokowany bez wpisu katalogowego). Pełniejsza weryfikacja drzewa/`nlink` to
  naturalny zakres pod-projektu B.
