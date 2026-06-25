# ghostfs — pod-projekt B: journaling (atomowość po awarii)

**Data:** 2026-06-23
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A (POSIX) ✓ → **B (journaling)** → C (szyfrowanie) → D (współbieżność).

## Cel

Atomowość operacji wielokrokowych mimo awarii zasilania/procesu. Dziś najgorszy
skutek awarii to osierocone bloki (naprawiane przez `fsck`), ale operacja
wielokrokowa (np. `rename`, `create`, `truncate`) może zostać przerwana w połowie,
zostawiając niespójny stan pośredni (wpis bez i-węzła, i-węzeł bez wpisu, połowicznie
zwolnione bloki). B wprowadza **fizyczny dziennik redo**: każda operacja modyfikująca
wiele bloków jest transakcją „wszystko albo nic". Po awarii, przy montowaniu,
zatwierdzone-lecz-niezapisane transakcje są odtwarzane; niezatwierdzone — porzucane.

Spec głównego projektu przewidział zapas w superbloku na journaling — wykorzystujemy go.

## Zasada nadrzędna: przezroczystość dla rdzenia

Kluczowa decyzja architektoniczna: **dziennik wpina się w warstwę bloków**, nie w
logikę FS. Gdy transakcja jest aktywna, `gh_block_write` nie pisze od razu na dysk,
tylko buforuje (numer_bloku → obraz 4 KB); `gh_block_read` najpierw zagląda do bufora
(read-your-writes). Dzięki temu `inode.c`, `dir.c`, `alloc.c`, `super.c` **nie wymagają
żadnych zmian** — automatycznie stają się transakcyjne, gdy transakcja jest aktywna.
Jedyne zmiany: `block.c` (świadomy transakcji odczyt/zapis), nowy `journal.c`, oraz
`fs.c` (opakowanie operacji w begin/commit) i `super.c`/format (region dziennika).

## Zmiany formatu on-disk (kompatybilne wstecz)

### Superblok — pola dziennika (z rezerwy)

```c
struct gh_superblock {
    ... (bez zmian) ...
    uint64_t root_inode;
    uint64_t journal_start;   /* NOWE: pierwszy blok regionu dziennika (0 = brak) */
    uint64_t journal_blocks;  /* NOWE: rozmiar dziennika w blokach (0 = brak) */
};
```

Pola leżą w dotychczas zerowanej rezerwie superbloku → stare kontenery czytają
`journal_blocks == 0` = **brak dziennika** (tryb bez journalingu, jak dotąd). **Bez
zmiany magic, bez wymuszonego reformatu.** Journaling działa po sformatowaniu nowym
`gh_format` (alokuje region dziennika).

### Układ kontenera po B

```
[super][mapa bitowa][i-węzły][DZIENNIK][dane]
```

`gh_format` wstawia region dziennika między i-węzły a dane. Rozmiar:
`journal_blocks = clamp(total_blocks/16, 64, 4096)`. Bloki dziennika są zaznaczone w
mapie bitowej jako metadane (nigdy nie alokowane jako dane). `data_start` rośnie o
`journal_blocks`.

### Format regionu dziennika

```
dziennik[0]         = nagłówek dziennika (gh_jheader)
dziennik[1..D]      = bloki deskryptora (lista docelowych numerów bloków)
dziennik[D+1..D+N]  = obrazy bloków (N sztuk, w kolejności jak w deskryptorze)
```

```c
struct gh_jheader {              /* leży w pierwszym bloku dziennika */
    uint8_t  magic[8];           /* "GHJRNL\0\1" */
    uint64_t seq;                /* numer sekwencyjny transakcji (rośnie) */
    uint32_t committed;          /* 1 = transakcja zatwierdzona i kompletna na dysku */
    uint32_t n_blocks;           /* liczba obrazów bloków w tej transakcji */
    uint64_t descriptor_blocks;  /* ile bloków deskryptora (D) */
    /* reszta bloku: zera */
};
```

Deskryptor: ciąg `uint64_t` numerów bloków docelowych (po `GH_PTRS_PER_BLK`=512 na
blok). Obraz i-ty trafi na blok o numerze `descriptor[i]`.

## Transakcja w pamięci

```c
struct gh_txn {
    int       active;
    uint64_t *blknos;            /* docelowe numery bloków (deduplikowane) */
    uint8_t (*images)[GH_BLOCK_SIZE]; /* obrazy bloków */
    uint32_t  n, cap;            /* liczba i pojemność */
};
```

`struct gh_dev` zyskuje `struct gh_txn *txn;` (NULL = brak aktywnej transakcji).
`cap` = `journal_blocks - 1 - descriptor_blocks_max` (ile obrazów mieści dziennik).

## API dziennika (`src/journal.h` / `src/journal.c`)

| Funkcja | Zachowanie |
|---|---|
| `int gh_jrnl_begin(struct gh_dev*, const struct gh_superblock*)` | alokuje/zeruje `txn`, ustawia `dev->txn`; jeśli `journal_blocks==0` → no-op (tryb bez dziennika), `dev->txn` zostaje NULL |
| `int gh_jrnl_commit(struct gh_dev*, const struct gh_superblock*)` | zapis dziennika (deskryptor + obrazy), `fsync`; zapis nagłówka z `committed=1`, `fsync` (punkt zatwierdzenia); checkpoint: obrazy na docelowe bloki, `fsync`; wyczyść nagłówek (`committed=0`), `fsync`; zwolnij `txn`, `dev->txn=NULL`. Bez dziennika → no-op |
| `void gh_jrnl_abort(struct gh_dev*)` | porzuca bufor `txn` (nic na dysk), `dev->txn=NULL` |
| `int gh_jrnl_recover(struct gh_dev*, const struct gh_superblock*)` | przy montowaniu: czyta nagłówek; jeśli `magic` OK i `committed==1` → odtwarza obrazy na docelowe bloki, `fsync`, czyści nagłówek. Idempotentne (można powtórzyć po kolejnej awarii w trakcie odtwarzania) |

### Świadomy transakcji `block.c`

- `gh_block_write(dev, blkno, buf)`: jeśli `dev->txn` aktywny → wstaw/zastąp wpis
  `(blkno, buf)` w buforze (`upsert`, deduplikacja po `blkno`); jeśli bufor pełny
  (`n==cap`) → zwróć `-ENOSPC`. Inaczej bezpośredni `pwrite` (jak dotąd).
- `gh_block_read(dev, blkno, buf)`: jeśli `dev->txn` aktywny i `blkno` w buforze →
  `memcpy` z obrazu; inaczej `pread` (jak dotąd).
- Zapisy do samego regionu dziennika (w `journal.c`) omijają transakcję — używają
  niskopoziomowego `pwrite`/wewnętrznej ścieżki bez `dev->txn` (by uniknąć rekurencji).

## Integracja w `fs.c`

Każda **modyfikująca** operacja publiczna opakowana w transakcję:

```
gh_jrnl_begin(&fs->dev, &fs->sb);
r = <dotychczasowa logika>;
if (r < 0) gh_jrnl_abort(&fs->dev); else { int c = gh_jrnl_commit(&fs->dev, &fs->sb); if (c) r = c; }
return r;
```

Operacje modyfikujące: `create`, `mkdir`, `unlink`, `rmdir`, `link`, `symlink`,
`rename`, `truncate`, `write`, `chmod`, `chown`, `utimens`, `setxattr`, `removexattr`.
Operacje tylko-do-odczytu (`read`, `getattr`, `readdir`, `statfs`, `getxattr`,
`listxattr`, `readlink`) — bez transakcji.

`gh_fs_mount` po wczytaniu superbloku woła `gh_jrnl_recover` (odtworzenie po awarii).

## Protokół zatwierdzania (porządek barier)

1. Zapisz bloki deskryptora + obrazy do regionu dziennika. `fsync`.
2. Zapisz nagłówek z `committed=1`, świeżym `seq`, `n_blocks`. `fsync`. **← punkt
   zatwierdzenia** (po nim transakcja przetrwa awarię).
3. Checkpoint: zapisz każdy obraz na docelowy blok (`pwrite`). `fsync`.
4. Wyczyść nagłówek (`committed=0`). `fsync`.

Awaria między 1–2: po montowaniu nagłówek `committed==0` (lub zły magic) → porzuć,
stan = sprzed operacji. Awaria między 2–4: `committed==1` → odtwórz obrazy na cele
(idempotentnie) → stan = po operacji. Nigdy stan pośredni.

## Strategia testowania (TDD + ASan)

1. **`tests/test_journal.c`** (nowy):
   - begin/commit: zapisy w transakcji widoczne po commit; przed commit na dysku
     (przez świeży `gh_dev_open` symulujący inny widok) NIE widać zmian.
   - abort: po `gh_jrnl_abort` żadna zmiana nie trafia na dysk.
   - **read-your-writes**: w trakcie transakcji `gh_block_read` widzi to, co
     `gh_block_write` zapisał.
   - **odtwarzanie po awarii**: ręcznie spreparuj dziennik z `committed=1` i obrazami,
     wywołaj `gh_jrnl_recover`, sprawdź że cele mają obrazy; nagłówek wyczyszczony.
   - **przerwanie przed commit**: spreparuj dziennik `committed=0` → recover nic nie
     zmienia.
   - przepełnienie transakcji (`n==cap`) → `-ENOSPC` przy `gh_block_write`.
2. **`tests/test_fs.c`** (rozszerzenie): symulacja awarii operacji — wstrzykiwany
   licznik zapisów bloków, który po N zapisach „udaje" awarię (przerywa checkpoint);
   ponowne montowanie + `gh_jrnl_recover` → operacja albo w pełni zaszła, albo wcale;
   `fsck`==0; brak utraty/wycieku.
3. **Format**: `tests/test_super.c` — nowy `gh_format` tworzy region dziennika,
   `journal_blocks>0`, `data_start` przesunięty; bloki dziennika zaznaczone w mapie.
4. **Property/integracja**: dotychczasowe testy (A) muszą dalej przechodzić (operacje
   przez transakcje dają ten sam wynik). `make test-asan` = 0 failed.
5. **Symulacja awarii w skrypcie**: kontener, sekwencja operacji CLI, „kill -9"
   procesu FUSE w trakcie zapisu (best-effort), remount → `fsck` czysty.

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| Transakcja > pojemność dziennika | `-ENOSPC` przy `gh_block_write`; operacja wycofana (abort) |
| Błąd I/O w fazie 1 (dziennik) | abort, stan sprzed operacji |
| Błąd I/O w fazie 3 (checkpoint) | nagłówek `committed==1` → następny mount dokończy odtwarzanie |
| Zły magic nagłówka dziennika | traktuj jak `committed==0` (brak transakcji do odtworzenia) |
| `journal_blocks==0` (stary kontener) | tryb bez dziennika — zachowanie jak w A |

## Świadome ograniczenia B (YAGNI)

- Jedna transakcja naraz (v1 = jeden proces; współbieżność = D).
- Dziennik fizyczny (pełne obrazy bloków) — prostszy i odporny; nie logiczny.
  Większy narzut I/O, akceptowalny dla v1.
- Operacja, która dotyka więcej bloków niż pojemność dziennika (skrajnie duży
  `truncate`/`write`), zwraca `-ENOSPC`. Rozmiar dziennika dobrany tak, by typowe
  operacje (w tym zapisy FUSE ≤128 KB i CLI ≤64 KB na wywołanie) mieściły się z
  zapasem.
- Brak checksumów obrazów (poza magic+committed+seq) — wykrywanie rozdarcia zapisu
  nagłówka opiera się na atomowości zapisu pojedynczego bloku przez OS. Pełne
  sumy kontrolne = możliwe rozszerzenie.
- `gh_format` z dziennikiem zmienia układ → stare kontenery działają bez dziennika
  (kompatybilność), nowe mają journaling po sformatowaniu.
