# ghostfs — pod-projekt I: group commit (batching dziennika)

**Data:** 2026-06-24
**Status:** zatwierdzony projekt (autonomia użytkownika)
**Część całości:** A–H ✓ → **I (group commit)** → J (checksumy) → K (katalogi haszowane).

## Cel i motywacja

Dziś **każda operacja FS = osobna transakcja** = 4× `fsync` + każdy blok zapisany
dwukrotnie (dziennik + miejsce docelowe). Dla SSD/flash to podwójny problem: amplifikacja
zapisu (zużycie komórek) i niska przepustowość (benchmark: 300 plików ≈ 2,5 s).

**Group commit:** bieżąca (running) transakcja **akumuluje wiele operacji** i jest
zatwierdzana trwale (`flush`) dopiero okresowo — na `fsync`/`flush` z VFS, przy zapełnieniu
bufora, lub przy odmontowaniu. Jeden trwały commit amortyzuje `fsync`-e i amplifikację na
wiele operacji. To **najwyższa dźwignia wydajnościowa/żywotnościowa** pod flash.

## Zmiana semantyki trwałości (świadoma, zgodna z POSIX)

Operacja zwraca sukces, ale jest **trwała dopiero po `flush`** (`fsync`/`fdatasync` z
aplikacji, zapełnienie bufora, lub `unmount`). To **dokładnie model POSIX** — bez `fsync`
nie ma gwarancji trwałości. Spójność po awarii zachowana: awaria między flushami →
utrata **niezflushowanej paczki**, FS spójny w stanie ostatniego flush (nigdy połowiczny).

## Atomowość pojedynczej operacji (savepoint + undo)

Skoro wiele operacji dzieli jedną running-transakcję, operacja, która **zawiedzie w
połowie** (np. `-ENOSPC` w trakcie `write`/`truncate`/`setxattr`), musi się wycofać **bez
naruszania wcześniejszych operacji z paczki**. Mechanizm: per-operacyjny **savepoint +
undo log** w buforze transakcji.

`struct gh_txn` rozszerzony:

```c
struct gh_undo { uint32_t idx; uint8_t img[GH_BLOCK_SIZE]; };
struct gh_txn {
    int       active;
    uint64_t *blknos; uint8_t (*images)[GH_BLOCK_SIZE];
    uint32_t  n, cap;
    /* group commit: */
    int       op_active;        /* czy trwa pojedyncza operacja */
    uint32_t  savepoint_n;      /* t->n na początku operacji */
    uint32_t  savepoint_nd;     /* dev->nd (discardy) na początku operacji */
    struct gh_undo *undo; uint32_t nundo, undocap;   /* obrazy nadpisanych slotów < savepoint_n */
    int       dirty;            /* bufor ma niezatwierdzone zmiany (do flush) */
};
```

- **`gh_jrnl_op_begin(dev)`**: `savepoint_n = t->n`; `savepoint_nd = dev->nd`; `nundo = 0`;
  `op_active = 1`.
- **`gh_block_write`** (gdy `op_active` i nadpisywany slot `idx < savepoint_n`): zanim
  nadpisze obraz, **dopisz `(idx, stary_obraz)` do undo logu** (jednokrotnie na slot w danej
  operacji — wystarczy zapisać pierwsze nadpisanie). Nowe sloty (`idx >= savepoint_n`) nie
  wymagają undo (wycofa je obcięcie `n`).
- **`gh_jrnl_op_commit(dev)`**: operacja udana — `op_active = 0`; `nundo = 0` (porzuć undo);
  `dirty = 1`.
- **`gh_jrnl_op_rollback(dev)`**: operacja nieudana — przywróć obrazy z undo logu (do
  `images[idx]`); `t->n = savepoint_n`; `dev->nd = savepoint_nd` (porzuć discardy operacji);
  `nundo = 0`; `op_active = 0`. Bufor wraca do stanu sprzed operacji; wcześniejsze operacje
  z paczki nietknięte.

**Undo „pierwsze nadpisanie":** aby uniknąć zapisu wielu obrazów tego samego slotu, undo
zapisuje stan slotu z momentu savepointu. Implementacja: przy nadpisaniu slotu `idx <
savepoint_n` sprawdź, czy `idx` jest już w undo logu tej operacji; jeśli nie — dodaj. (Lub
prościej, choć drożej pamięciowo: zapisuj każde nadpisanie i odtwarzaj w odwrotnej
kolejności — wtedy ostatni odtworzony to najstarszy = poprawny stan sprzed operacji.
Wybór: **odwrotna kolejność**, prostsze i poprawne.)

## Trwały commit (`flush`) i cykl życia

- **`gh_jrnl_open(dev, sb)`** (zastępuje `gh_jrnl_begin`): przy montowaniu otwiera running
  txn (alokuje bufor, `n=0`, `dirty=0`). `journal_blocks==0` → `dev->txn=NULL` (tryb bez
  dziennika, jak dotąd).
- **`gh_jrnl_flush(dev, sb)`**: jeśli `txn` i `dirty` i `n>0` → wykonaj **dotychczasowy
  4-fazowy commit** (deskryptor+obrazy→fsync; nagłówek committed=1→fsync; checkpoint→fsync;
  wyczyść nagłówek→fsync), potem `gh_discard_flush`, potem **zresetuj bufor** (`n=0`,
  `nd=0`, `dirty=0`). Bufor zostaje otwarty na kolejną paczkę. Bez dziennika → no-op.
- **`gh_jrnl_close(dev)`**: zwalnia bufor txn + undo + ustawia `dev->txn=NULL`. (Wołane przy
  unmount PO `gh_jrnl_flush`.)
- **`gh_jrnl_recover`** — bez zmian (odtwarza ostatni zflushowany, lecz niedokończony
  commit).

### Wyzwalacze flush
1. **`gh_fs_sync`** (FUSE `flush`/`fsync`): `gh_jrnl_flush` + `fsync(fd)` — bariera
   trwałości na żądanie aplikacji.
2. **Pojemność:** przy rozpoczęciu operacji (`op_begin`), jeśli `t->n > cap/2` →
   `gh_jrnl_flush` przed operacją (gwarancja miejsca; running txn nigdy nie przepełnia się
   w trakcie typowej operacji). Operacja większa niż pojemność → `-ENOSPC` (jak dotąd).
3. **`gh_fs_unmount`:** `gh_jrnl_flush` przed `gh_jrnl_close`.

## Integracja w `fs.c`

Helpery transakcyjne zmieniają znaczenie (mniej `fsync`):

```c
static int txn_begin(struct gh_fs *fs) {
    if (fs->dev.txn && fs->dev.txn->n > fs->dev.txn->cap / 2)
        gh_jrnl_flush(&fs->dev, &fs->sb);     /* pojemność */
    gh_jrnl_op_begin(&fs->dev);
    return 0;
}
static int txn_end_i(struct gh_fs *fs, int rc) {
    if (rc < 0) gh_jrnl_op_rollback(&fs->dev);
    else        gh_jrnl_op_commit(&fs->dev);
    return rc;                                /* BRAK trwałego commitu tutaj */
}
/* analogicznie txn_end_s dla ssize_t */
```

- `gh_fs_mount_key`: `gh_jrnl_open` zamiast `gh_jrnl_begin`; `gh_jrnl_recover` jak dotąd.
- `gh_fs_sync`: `gh_jrnl_flush(&fs->dev,&fs->sb)`; zwróć `fsync(fd)` wynik.
- `gh_fs_unmount`: `gh_jrnl_flush` → `gh_jrnl_close` → (wymaż klucz, zwolnij cache, zamknij).

## Współbieżność i odczyty (bez regresji D/B)

- Running txn to współdzielony stan mutowany wyłącznie pod `wrlock` (D). `flush` zachodzi
  pod `wrlock` (gf_fsync/gf_flush) albo jednowątkowo (unmount) albo wewnątrz operacji
  zapisu (`op_begin` capacity). Nigdy nie wyścig z odczytami.
- **Read-your-writes:** odczyty (`rdlock`) widzą bufor running txn (cała paczka:
  zatwierdzone operacje), więc `getattr`/`read`/`fsck` w obrębie sesji widzą spójny stan
  — bez potrzeby flush. (`op_rollback` usuwa zmiany nieudanej operacji, więc niewidoczne.)

## Wpływ na testy (świadomy)

Operacja nie jest trwała na dysku do `flush`. Skutki dla testów:
- Testy używające `fsck` w tej samej sesji **działają bez zmian** (read-your-writes na
  buforze running txn → `fsck==0`).
- Testy **remount** (unmount → mount) działają — `unmount` flushuje.
- Testy sprawdzające **surowy stan dysku** (`st_blocks`, surowy `pread`) **w trakcie sesji
  bez flush** muszą dodać `gh_fs_sync` (by wywołać flush). Dotyczy m.in.
  `test_discard_on_free` (H) — dodać `gh_fs_sync` po `unlink` przed sprawdzeniem
  `st_blocks`.
- **Crash-sweep (`test_crash`, E):** prep (`mkdir`) musi być utrwalony osobno — dodać
  `gh_fs_sync` po prep, przed ustawieniem `fail_after`. Wtedy `fail_after` wyzwala się w
  trakcie `flush` operacji (przy unmount); atomowość paczki zachowana; remount + recover →
  `fsck==0`; plik pełny-albo-nieobecny.

## Strategia testowania (TDD + ASan)

1. `tests/test_fs.c`: **atomowość operacji w paczce** — w jednej sesji wykonaj operację
   udaną A, potem operację B kończącą się błędem (np. `setxattr` przekraczający pojemność
   albo `write` ENOSPC), bez flush; sprawdź, że A jest spójna a B w pełni wycofana
   (read-your-writes); `fsck==0`.
2. `tests/test_fs.c`: **trwałość na flush** — operacje, `gh_fs_sync`, „symuluj utratę
   pamięci" przez `gh_fs_unmount`+`mount`; dane obecne. Oraz: operacje BEZ flush, potem
   `unmount` (flushuje) + remount → dane obecne.
3. `tests/test_crash.c`: zaktualizowany sweep (flush przy unmount jako jednostka atomowa) —
   `fsck==0` dla każdego N; atomowość paczki.
4. **Mniej `fsync`:** benchmark (`test_bench`) — utworzenie M plików + jeden `gh_fs_sync`
   na końcu jest istotnie szybsze niż commit-per-op (pomiar informacyjny; oczekiwany
   wyraźny spadek czasu).
5. **Regresja:** wszystkie testy A–H `0 failed` (z dostosowaniami flush wg „Wpływ na
   testy"); `make test-asan` = 0; integracja FUSE + stres współbieżności (D) zielone;
   integracja urządzenia blokowego (H) zielona.

## Obsługa błędów

| Sytuacja | Reakcja |
|---|---|
| Operacja nieudana w paczce | `op_rollback` — wycofanie tylko tej operacji |
| Bufor pełny przy `op_begin` | `flush` przed operacją |
| Operacja > pojemność dziennika | `-ENOSPC` (jak dotąd) |
| Błąd I/O w `flush` | abort running txn (porzuć paczkę), zwróć błąd; FS spójny w ostatnim flush |
| Awaria między flushami | utrata niezflushowanej paczki; FS spójny (recover) |

## Świadome ograniczenia I (YAGNI)

- Brak okresowego timera flush (np. co 5 s jak ext4) — flush na `fsync`/pojemność/unmount.
  Timer wymagałby wątku w tle; możliwe rozszerzenie. (FUSE i tak woła `flush` przy
  `close`.)
- Trwałość zależna od `fsync` aplikacji — zgodne z POSIX, ale aplikacje niewołające
  `fsync` tracą więcej przy awarii niż w modelu commit-per-op. Świadomy kompromis
  wydajność/trwałość (jak we wszystkich poważnych FS).
- Amplifikacja zapisu pojedynczej paczki nadal 2× (dziennik+checkpoint) — inherentne dla
  fizycznego journalingu; redukcja = journaling logiczny (przyszłość).
