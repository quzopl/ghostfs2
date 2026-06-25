# ghostfs — pod-projekt H: wsparcie SSD/flash i urządzeń blokowych

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–G ✓ → **H (SSD/flash)**.

## Cel i motywacja

Docelowym nośnikiem jest **SSD / NVMe / pamięć flash**, nie tylko plik-kontener. Dwie
realne, ograniczone zmiany dopasowane do obecnej architektury (in-place + journaling),
bez redesignu CoW:

1. **Backend urządzeń blokowych** — ghostfs formatuje i montuje **realne urządzenia**
   (`/dev/sdX`, `/dev/nvmeXnY`, loop), nie tylko pliki. To pozycja „nie tylko sam
   kontener".
2. **TRIM/discard** — przy zwalnianiu bloków FS informuje nośnik, że bloki są nieużywane
   (`BLKDISCARD` na urządzeniu / `fallocate(PUNCH_HOLE)` na pliku). To kluczowa zmiana
   przyjazna flashowi: lepszy wear-leveling, garbage collection i mniejsza amplifikacja
   zapisu.

Świadomie POZA zakresem (z dokumentacją w sekcji końcowej): log-structured/hot-cold
(F2FS), zone-aware ZNS/SMR, RAID/multi-device — to redesign lub inny substrat.

## Część 1: Backend urządzeń blokowych

`struct gh_dev` zyskuje `int is_blkdev;`.

- **`gh_dev_open`:** `fstat`; jeśli `S_ISBLK` → rozmiar przez `ioctl(BLKGETSIZE64)`,
  `is_blkdev=1`; inaczej `lseek(SEEK_END)`, `is_blkdev=0`. `total_blocks = rozmiar /
  GH_BLOCK_SIZE`.
- **`gh_format_enc`/`gh_format`:** wykryj urządzenie blokowe (`stat` + `S_ISBLK`):
  - urządzenie → **otwórz** (`gh_dev_open`, bez `ftruncate`); `total_blocks` = pełny
    rozmiar urządzenia (argument `total_blocks` ignorowany/walidowany — `0` lub `>dev`
    → użyj rozmiaru urządzenia). Walidacja: rozmiar musi dać ≥ minimalny layout.
  - plik (istniejący lub nie) → **utwórz** rzadki plik o `total_blocks` (jak dotąd przez
    `gh_dev_create`).
  - Sektor logiczny urządzenia > 4096 B → odmowa (`-EINVAL`): blok ghostfs (4096) musi być
    wielokrotnością/≥ sektora; 512 i 4096 OK.
- **CLI `format`:** `total_blocks == 0` oznacza „autorozmiar" (wymagane dla urządzeń;
  dla nieistniejącego pliku `0` → `-EINVAL`).
- Montowanie (FUSE/CLI) urządzenia działa przez `gh_dev_open`; `flock` na urządzeniu jak
  na pliku (jeden użytkownik na raz).

## Część 2: TRIM/discard

`struct gh_dev` zyskuje listę odroczonych discardów: `uint64_t *discards; uint32_t nd,
dcap;` (init NULL/0).

### Niskopoziomowy discard (`block.c`)

```c
int gh_disk_discard(struct gh_dev *dev, uint64_t blkno, uint64_t count);
```

- urządzenie blokowe → `ioctl(BLKDISCARD, {offset, length})`,
- plik → `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, length)`,
- **best-effort:** `ENOTSUP`/`EOPNOTSUPP`/`EINVAL` → zwróć 0 (nośnik/FS nie wspiera
  discard — nie przerywaj operacji). Discard to wyłącznie podpowiedź.
- `block.c` definiuje `_GNU_SOURCE` (dla `fallocate`/`BLKDISCARD`); nagłówki
  `<sys/ioctl.h>`, `<linux/fs.h>`, `<fcntl.h>`.

### Odroczenie zintegrowane z dziennikiem (bezpieczeństwo)

Discard zwolnionego bloku MUSI nastąpić **dopiero po zatwierdzeniu** operacji zwalniającej
— inaczej wycofanie transakcji (abort) zostawiłoby blok zdiscardowany, lecz wciąż
zaalokowany (utrata danych). Dlatego:

- **`gh_free_block`:** jeśli `dev->txn` aktywny → dopisz `blkno` do `dev->discards`
  (odroczenie); inaczej (brak transakcji) → `gh_disk_discard` natychmiast.
- **`gh_jrnl_commit`:** po pełnym zatwierdzeniu (wyczyszczeniu nagłówka dziennika) →
  **przefiltruj** odroczone bloki (discard tylko gdy bit w mapie = wolny, na wypadek
  realokacji w tej samej transakcji), **scal** ciągłe zakresy i wywołaj `gh_disk_discard`
  per zakres; wyczyść listę.
- **`gh_jrnl_abort`:** wyczyść listę odroczeń **bez** discardu.
- Tryb bez dziennika (`journal_blocks==0`): brak transakcji → discard natychmiastowy w
  `gh_free_block`.

**Poprawność z zerowaniem przy alokacji:** `gh_alloc_block` zeruje nowy blok, więc nawet
gdyby nośnik po discardzie zwracał śmieci, blok jest nadpisany zerami przed użyciem —
discard-po-commit jest bezpieczny.

### Whole-device discard przy formacie

`gh_format_enc` na początku (po otwarciu urządzenia/utworzeniu pliku, przed zapisem
metadanych) discarduje **cały region danych** (`gh_disk_discard(data_start..total)` lub
całe urządzenie) — standard `mkfs` na SSD: czysty start. Best-effort. Zapisy metadanych i
tak nadpisują swoje bloki.

## Część 3: Cykl życia i narzędzia

- `gh_dev_open`/`gh_dev_create` inicjalizują `is_blkdev`, `discards=NULL`, `nd=dcap=0`.
- `gh_dev_close` zwalnia `dev->discards` (jeśli zaalokowane).
- CLI: komunikat przy formacie urządzenia (autorozmiar). FUSE bez zmian (mount przez
  `gh_dev_open`).

## Strategia testowania (TDD + ASan)

1. `tests/test_block.c` (rozszerzenie): `gh_disk_discard` na pliku-kontenerze punch'uje
   dziurę (po discardzie `stat.st_blocks` maleje); best-effort gdy niewspierane.
2. `tests/test_fs.c` (rozszerzenie): po utworzeniu+zapisie+usunięciu pliku, odroczone
   discardy są wykonane po commit (plik-kontener staje się rzadszy: `st_blocks` po `unlink`
   < przed); `fsck==0`; brak discardu na blokach realokowanych (free+realloc w jednej
   sekwencji nie discarduje aktywnego bloku).
3. `tests/test_fs.c`: abort (np. operacja kończąca się błędem) nie discarduje — niezmiennik
   integralności (blok zwolniony-lecz-wycofany nie jest discardowany).
4. **Urządzenie blokowe (integracja, wymaga uprawnień):** `tests/integration_blockdev.sh`
   — `losetup` tworzy loop nad plikiem zapasowym, `format` ghostfs na `/dev/loopX`, mount
   FUSE, operacje round-trip, `fsck` czysty, unmount, `losetup -d`. Pomijany gdy brak
   roota/loop (komunikat `POMINIETO`).
5. **Regresja:** wszystkie testy A–G `0 failed`; `make test-asan` = 0; integracja FUSE
   (plik-kontener) zielona.

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| Discard niewspierany przez nośnik/FS | best-effort: zwróć 0, kontynuuj |
| Format urządzenia, sektor logiczny > 4096 | `-EINVAL` |
| Format urządzenia, `total_blocks` > rozmiar | użyj rozmiaru urządzenia |
| `format` pliku nieistniejącego z `total_blocks==0` | `-EINVAL` |
| Abort transakcji z odroczonymi discardami | wyczyść listę, bez discardu |
| Blok realokowany przed flush discardu | pominięty (filtr „wolny w mapie") |

## Świadome ograniczenia H (YAGNI)

- Discard odroczony per-transakcja, scalany w zakresy; brak globalnego batcha/cooldownu
  (np. `fstrim`-style okresowy) — możliwe rozszerzenie.
- Brak log-structured/hot-cold (F2FS), zone-aware (ZNS/SMR), multi-device/RAID — inny
  substrat lub redesign CoW (v2).
- Zerowanie bloku przy alokacji pozostaje (bezpieczeństwo > minimalna amplifikacja
  zapisu); świadomy kompromis.
- Amplifikacja zapisu dziennika (2× dla bloków objętych transakcją) nieadresowana —
  inherentna dla fizycznego journalingu; redukcja = journaling logiczny (przyszłość).
- Alignment: zakładamy sektor ≤ 4096 i rozmiar = wielokrotność 4096 (typowe 512/4096).
