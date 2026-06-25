# ghostfs — pod-projekt L: węzły specjalne (mknod)

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–K ✓ → **L (węzły specjalne / mknod)**.

## Cel

Dodanie **węzłów specjalnych** POSIX: FIFO (potoki nazwane), gniazda (AF_UNIX), urządzenia
**znakowe** i **blokowe**. Domyka lukę POSIX i jest warunkiem koniecznym, by ghostfs mógł
kiedykolwiek pełnić rolę roota (init/udev tworzą węzły specjalne). Bez zmiany formatu
on-disk (kompatybilne wstecz).

## Model on-disk (bez zmiany layoutu)

Nowe typy i-węzłów (wartości mieszczą się w istniejącym `uint16_t type`):

```c
enum gh_itype { GH_FREE=0, GH_FILE=1, GH_DIR=2, GH_SYMLINK=3,
                GH_FIFO=4, GH_SOCK=5, GH_CHR=6, GH_BLK=7 };
```

- FIFO/gniazdo: brak treści i `rdev` — i-węzeł z samymi metadanymi (`direct[]` zerowe).
- Urządzenie znakowe/blokowe: numer urządzenia `rdev` (`dev_t`) trzymany w **`direct[0]`**
  (technika ext2 — węzeł urządzenia nie ma bloków danych). Brak nowych pól w i-węźle.

Stare kontenery nie zawierają typów 4–7 → pełna kompatybilność.

## Krytyczne: węzły specjalne NIE mają bloków danych

`direct[0]` urządzenia to `rdev`, **nie** numer bloku. Każda ścieżka, która zwalnia/znaczy
bloki i-węzła, MUSI pomijać węzły specjalne, inaczej `rdev` zostanie potraktowany jak blok
(zwolnienie/podwójne użycie → korupcja mapy bitowej). Dotyczy:

- **`gh_inode_free`** (`inode.c`): zwalniaj `direct[]`/`indirect`/`double_indirect` **tylko**
  dla typów z treścią (`GH_FILE`/`GH_DIR`/`GH_SYMLINK`); dla węzłów specjalnych pomiń
  zwalnianie bloków (i-węzeł i tak zwalniany; `xattr_block` zwalniany zawsze).
- **`gh_fsck`** (`fs.c`): znacz `direct[]`/indirect/double jako osiągalne **tylko** dla
  typów z treścią; `xattr_block` znaczony zawsze. (Inaczej `rdev` w `direct[0]` =
  fałszywie „osiągalny blok".)
- **`gh_fs_truncate`** (`fs.c`): dozwolone **wyłącznie** dla `GH_FILE`; `GH_DIR` → `-EISDIR`;
  pozostałe (symlink/specjalne) → `-EINVAL`. (Chroni `rdev` przed zwolnieniem.)

## Nowe/zmienione API

### Rdzeń (`fs.c` / `fs.h`)

```c
int gh_fs_mknod(struct gh_fs*, const char *path, uint16_t mode, uint64_t rdev);
```

- Typ węzła wyprowadzony z `mode & S_IFMT`: `S_IFIFO→GH_FIFO`, `S_IFSOCK→GH_SOCK`,
  `S_IFCHR→GH_CHR`, `S_IFBLK→GH_BLK`, `S_IFREG→GH_FILE` (fallback), inne → `-EINVAL`
  (katalog tworzy `mkdir`, nie `mknod`).
- Tworzy i-węzeł danego typu, `mode = mode & 07777`; dla `GH_CHR`/`GH_BLK` zapisuje
  `direct[0]=rdev`. Dodaje wpis w katalogu rodzica. Opakowane w transakcję (group commit).
- Wewnętrznie: `make_node` rozszerzony o parametr `rdev` (create/mkdir przekazują `0`).

### Warstwa FUSE (`fuse_main.c`)

- `getattr`: mapuj typy na `S_IFIFO`/`S_IFSOCK`/`S_IFCHR`/`S_IFBLK` (oraz dotychczasowe
  `S_IFDIR`/`S_IFLNK`/`S_IFREG`); dla `GH_CHR`/`GH_BLK` ustaw `st->st_rdev = (dev_t)direct[0]`.
- Nowy handler `mknod` → `gh_fs_mknod` (pod `wrlock`). Dodany do tablicy `ops`.

### CLI (`cli.c`)

- Komenda `mknod <plik> <ścieżka> <fifo|sock|chr|blk> [major minor]` — buduje `mode` i
  `rdev` (`makedev`), woła `gh_fs_mknod`. Do testów/kompletności.

## Zachowanie operacji na węzłach specjalnych

- **hardlink** (`gh_fs_link`): dozwolony dla węzłów specjalnych (nie-katalog) → `nlink++`
  (POSIX pozwala dowiązywać węzły urządzeń). Katalog dalej `-EPERM`.
- **unlink** (`remove_node`): jak plik — dekrement `nlink`, zwolnienie i-węzła przy 0
  (bez zwalniania bloków — `gh_inode_free` pomija).
- **read/write** (`gh_fs_read/write`): tylko `GH_FILE` (dla węzłów specjalnych jądro samo
  obsługuje I/O; przez ghostfs nie są wołane). Bez zmian.
- **xattr/chmod/chown/utimens/rename**: działają na węzłach specjalnych jak na plikach
  (operują na metadanych i-węzła) — bez zmian.

## Strategia testowania (TDD + ASan)

1. `tests/test_fs.c`: utwórz przez `gh_fs_mknod` FIFO, gniazdo, urządzenie znakowe
   (major/minor), blokowe; `gh_fs_getattr` → poprawny `type`; dla CHR/BLK `direct[0]==rdev`.
   `fsck==0` po utworzeniu (kluczowe: `rdev` w `direct[0]` NIE jest fałszywie „osiągalny").
   `unlink` węzła → `fsck==0`, i-węzeł wolny, brak fałszywego zwolnienia bloku.
2. `tests/test_fs.c`: hardlink urządzenia (`nlink` rośnie, oba wpisy działają); `truncate`
   węzła specjalnego → `-EINVAL`; `mknod` z błędnym `mode` (np. katalog) → `-EINVAL`.
3. Integracja FUSE (`tests/integration.sh`): `mkfifo $MNT/p` → `ls -l` pokazuje `p` jako
   FIFO (`prw-`); `mknod $MNT/cdev c 1 3` (jeśli root/sudo) → `ls -l` pokazuje `crw-` z
   `1, 3`; `stat` poprawny typ/rdev. (mknod urządzenia wymaga uprawnień — pomiń gdy brak.)
4. **Regresja:** wszystkie testy A–K `0 failed`; `make test-asan` = 0; integracja FUSE +
   urządzenie blokowe zielone; `fsck` czysty (węzły specjalne nie psują mapy bitowej).

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| `mknod` z `mode` katalogu/nieznanym typem | `-EINVAL` |
| `truncate` węzła specjalnego/symlinka | `-EINVAL` (katalog `-EISDIR`) |
| `link` na katalog | `-EPERM` (węzły specjalne dozwolone) |
| istniejąca ścieżka | `-EEXIST` |

## Świadome ograniczenia L (YAGNI)

- ghostfs **przechowuje** węzły specjalne (typ + `rdev`); semantyka IPC (FIFO/gniazda) i
  I/O urządzeń to domena jądra — ghostfs jedynie utrwala węzeł, jak każdy filesystem.
- Brak `mode` z bitami setuid/setgid/sticky ponad standardowe 12 bitów (`mode & 07777`).
- To jeden z kilku warunków roota; pełna zdolność bootowania (in-kernel lub initramfs +
  pivot_root + mmap binarek) pozostaje większym tematem.
