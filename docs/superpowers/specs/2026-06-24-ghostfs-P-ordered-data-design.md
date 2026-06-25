# ghostfs — pod-projekt P: ordered-data journaling (ścieżka zapisu)

**Data:** 2026-06-24
**Status:** zatwierdzony (autonomia użytkownika); diagnoza z `raport.md`.
**Część całości:** A–O ✓ → **P (ordered data)**.

## Problem
Zapis ~9 MB/s przy odczycie ~204 MB/s na pendrivie. Przyczyna: **journaling fizyczny pisze
każdy blok dwukrotnie** (dziennik + checkpoint), więc bloki danych (główna objętość) mają 2×
amplifikację. Dodatkowo bufor transakcji wypełnia się danymi → częste flushe pojemnościowe →
dużo `fsync` (drogich na USB).

## Rozwiązanie: ordered-data dla nowo-alokowanych bloków zwykłych plików

**Nowo-alokowane bloki danych zwykłego pliku zapisywane są BEZPOŚREDNIO do miejsca
docelowego (z pominięciem dziennika); journalowane są tylko metadane** (mapa bitowa,
i-węzeł, suma kontrolna bloku). Dane zapisane raz (1×) zamiast dwukrotnie; bufor txn
wypełnia się tylko metadanymi (~1% objętości) → wielokrotnie mniej flushy → mniej `fsync`.

### Zakres (świadomie wąski dla bezpieczeństwa)
- **Tylko zwykłe pliki** (`node->type == GH_FILE`). Katalogi (krytyczne metadane) i wszystko
  inne pozostają **w pełni journalowane** (bez zmian) — ogranicza obszar ryzyka.
- **Tylko NOWO-alokowane bloki liści**. Nadpisania istniejących bloków pozostają
  journalowane (atomowość danych+sumy przy edycji in-place — bez tego rozdarte nadpisanie
  desynchronizowałoby dane i sumę → fałszywe EIO).
- Bloki wskaźnikowe (indirect/double/l1) — zawsze journalowane i zerowane (metadane).

## Dlaczego to crash-consistent

Nowo-alokowany blok staje się **referencyjny** (osiągalny przez i-węzeł) dopiero gdy jego
metadane (mapa+i-węzeł) zostaną zatwierdzone. Zatem:
- **Awaria PRZED commitem metadanych:** mapa bitowa niezatwierdzona → blok **wolny**,
  niereferencyjny. Bezpośrednio zapisana treść leży w wolnym bloku → nieczytana, nieszkodliwa
  (zostanie nadpisana przy realokacji). Spójne (utrata niezflushowanego zapisu = POSIX).
- **Awaria PO commicie:** dane trwałe (fsync fazy-1 dziennika utrwala bezpośredni zapis
  PRZED znacznikiem commit) + metadane trwałe → blok referencyjny z poprawną treścią i sumą.
  Spójne.
- **Ordering:** bezpośredni zapis (`gh_disk_write` = `pwrite`, cache write-through) trafia do
  cache OS w trakcie operacji; `fsync(dev->fd)` w fazie 1 `gh_jrnl_flush` utrwala go PRZED
  zapisem znacznika commit (faza 2). Dane są więc trwałe nie później niż metadane.
- **Recover:** odtwarza obrazy dziennika (metadane, w tym blok sumy) na cele; blok danych nie
  jest w dzienniku, ale został zapisany bezpośrednio (trwały przed commitem) → już na miejscu.
  Suma (z dziennika) + dane (bezpośrednie) spójne.
- **Rollback operacji (I):** metadane (mapa/i-węzeł/suma) cofane przez undo; bezpośredni
  zapis danych zostaje na dysku, ale blok zwolniony (mapa cofnięta) → niereferencyjny,
  nieszkodliwy.

## Zmiany API

### `src/alloc.c` / `alloc.h`
```c
int gh_alloc_block_nz(struct gh_dev*, const struct gh_superblock*, uint64_t *out);
```
Jak `gh_alloc_block`, ale **bez zerowania** bloku (zaznacza tylko bit mapy + hint). Refaktor:
wspólny `alloc_impl(..., int zero)`; `gh_alloc_block` = `zero=1`, `gh_alloc_block_nz` = `zero=0`.
Blok nz ma śmieci do czasu pełnego zapisu przez `gh_inode_pwrite` (zawsze pełny blok zera+dane).

### `src/block.c` / `block.h`
```c
int gh_block_write_direct(struct gh_dev*, uint64_t blkno, const void *buf);
```
Aktualizuje **sumę kontrolną** bloku jak `gh_block_write` (journalowana, do txn), po czym
zapisuje **dane bezpośrednio** przez `gh_disk_write` (szyfrowanie + `pwrite` + cache),
**z pominięciem bufora txn**. (Suma w dzienniku, dane bezpośrednio — spójne po commicie.)

### `src/inode.c`
- `bmap(..., int leaf_direct, int *out_newleaf)`: gdy alokuje **blok liścia** (`direct[lbn]`/
  `ptrs[lbn]`/`l2[i2]`) i `leaf_direct` → użyj `gh_alloc_block_nz` i ustaw `*out_newleaf=1`;
  inaczej `gh_alloc_block` (zerowany). Bloki wskaźnikowe zawsze `gh_alloc_block` (zerowane).
  Istniejące wywołania (pread/truncate, alloc=0) → `leaf_direct=0, out_newleaf=NULL`.
- `gh_inode_pwrite`: `int direct = (node->type == GH_FILE)`. Dla każdego bloku:
  - `bmap(..., direct, &newleaf)`.
  - jeśli `direct && newleaf`: zbuduj pełny blok (`memset 0` gdy zapis częściowy, potem
    `memcpy` danych) i `gh_block_write_direct(dev, phys, blk)` — **bez RMW** (blok nowy).
  - inaczej (nadpisanie / katalog / inne): jak dotąd — RMW gdy częściowy + `gh_block_write`.

## Strategia testowania (TDD + ASan)

1. **Poprawność/read-your-writes** (`tests/test_inode.c` lub `test_fs.c`): zapisz pliki
   różnej wielkości (pełne bloki, częściowe, wielo-blokowe, częściowy ostatni blok) i odczytaj
   — bajt w bajt zgodne. Plik z dziurą (zapis na offsecie) → dziura czyta się jako zera.
   Nadpisanie istniejącego bloku → poprawne. `fsck==0`.
2. **Szyfrowanie + sumy** (`tests/test_csum.c`/`test_enc.c`): na kontenerze szyfrowanym i
   sumowanym — zapis bezpośredni szyfruje i ustawia sumę; odczyt OK; `fsck==0`; surowe bajty
   na dysku nie zawierają plaintextu; weryfikacja sumy przechodzi (brak fałszywego EIO).
3. **Crash-consistency (BRAMKA)** (`tests/test_crash.c`): istniejący sweep awarii musi dalej
   dawać `fsck==0`. NOWY scenariusz: zapis danych (bezpośredni) + awaria przed commitem →
   `fsck==0`, brak referencyjnego bloku ze śmieciami/EIO (plik albo ma dane, albo nie).
4. **Trwałość**: zapis + `fsync` → trwałe po remount; zapis bez fsync + symulacja utraty
   pamięci → spójne (`fsck==0`), brak EIO na blokach referencyjnych.
5. **Regresja A–O** `0 failed`; `make test-asan` = 0; integracja FUSE + urządzenie blokowe
   zielone (round-trip, edycja in-place, kill -9 + recover → fsck czysty, at-rest enc).
6. **Wydajność (informacyjnie)**: liczba wpisów w buforze txn po zapisie dużego pliku
   drastycznie niższa (tylko metadane); mniej flushy pojemnościowych. (Pomiar throughput
   zależny od nośnika — odnotowany.)

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| `gh_disk_write` (bezpośredni) błąd I/O | `gh_inode_pwrite` zwraca błąd → op rollback (mapa/suma cofnięte) → spójne |
| ENOSPC w trakcie wielo-blokowego zapisu | rollback całej operacji; bloki bezpośrednie zwolnione → niereferencyjne |
| Nadpisanie istniejącego bloku | ścieżka journalowana (bez zmian) — atomowość danych+sumy |
| Katalog / symlink / xattr | w pełni journalowane (bez zmian) |

## Świadome ograniczenia P (YAGNI)

- Nadpisania in-place wciąż 2× (journalowane) — konieczne dla atomowości danych+sumy przy
  edycji bloku referencyjnego. Główny zysk dotyczy zapisu nowych danych (`cp`, append) —
  dokładnie workload z raportu.
- Katalogi journalowane (świadomy wybór bezpieczeństwa) — ich objętość jest znikoma.
- Pełna eliminacja amplifikacji dla wszystkich zapisów wymagałaby CoW (log-structured) —
  osobny, większy temat („v2").
