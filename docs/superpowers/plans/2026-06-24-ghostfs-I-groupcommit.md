# ghostfs pod-projekt I — group commit: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan + crash-sweep fsck==0.

**Goal:** Bieżąca (running) transakcja akumuluje wiele operacji; trwały `flush` na fsync/pojemność/unmount; per-op savepoint+undo dla atomowości operacji. Mniej fsync, mniejsza amplifikacja zapisu. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-I-groupcommit-design.md`.

**Tech Stack:** C11, OpenSSL, pthreads, libfuse3, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- KRYTYCZNE: crash-consistency zachowana (crash-sweep `fsck==0`); per-op atomowość; wszystkie testy A–H `0 failed` (z dostosowaniami flush).
- Trwałość: operacja trwała dopiero po `flush` (fsync/pojemność/unmount) — zgodne z POSIX.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/block.h` | `struct gh_undo`; pola group-commit w `gh_txn` |
| `src/block.c` | `gh_block_write` zapisuje undo przy nadpisaniu slotu < savepoint |
| `src/journal.h` | nowe API: open/op_begin/op_commit/op_rollback/flush/close (zamiast begin/commit/abort) |
| `src/journal.c` | running txn + savepoint/undo + flush + recover-on-flush-fail |
| `src/fs.c` | txn helpers (op-level); mount→open; sync→flush; unmount→flush+close |
| `tests/test_journal.c` | nowe API |
| `tests/test_fs.c` | atomowość paczki + trwałość; dostosowanie test_discard_on_free |
| `tests/test_crash.c` | flush prep przed fail_after |

---

## Task 1: Model running-transaction (journal + block)

**Files:** Modify: `src/block.h`, `src/block.c`, `src/journal.h`, `src/journal.c`, `tests/test_journal.c`

- [ ] **Step 1: `struct gh_undo` + pola w `gh_txn` (`src/block.h`)**

Zastąp `struct gh_txn` i dodaj `struct gh_undo` PRZED nią:

```c
struct gh_undo { uint32_t idx; uint8_t img[GH_BLOCK_SIZE]; };
struct gh_txn {
    int       active;
    uint64_t *blknos;
    uint8_t (*images)[GH_BLOCK_SIZE];
    uint32_t  n, cap;
    int       op_active;
    uint32_t  savepoint_n;
    uint32_t  savepoint_nd;
    struct gh_undo *undo; uint32_t nundo, undocap;
    int       dirty;
};
```

- [ ] **Step 2: `gh_block_write` zapisuje undo (`src/block.c`)**

Dodaj statyczny helper undo i zmień ścieżkę nadpisania slotu:

```c
static int gh_txn_undo_push(struct gh_txn *t, uint32_t idx) {
    if (t->nundo >= t->undocap) {
        uint32_t nc = t->undocap ? t->undocap * 2 : 16;
        struct gh_undo *u = realloc(t->undo, (size_t)nc * sizeof(struct gh_undo));
        if (!u) return -ENOMEM;
        t->undo = u; t->undocap = nc;
    }
    t->undo[t->nundo].idx = idx;
    memcpy(t->undo[t->nundo].img, t->images[idx], GH_BLOCK_SIZE);
    t->nundo++;
    return 0;
}

int gh_block_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) {
                if (t->op_active && i < t->savepoint_n) {
                    if (gh_txn_undo_push(t, i) != 0) return -ENOMEM;  /* nie modyfikuj bez undo */
                }
                memcpy(t->images[i], buf, GH_BLOCK_SIZE);
                return 0;
            }
        if (t->n >= t->cap) return -ENOSPC;
        t->blknos[t->n] = blkno; memcpy(t->images[t->n], buf, GH_BLOCK_SIZE); t->n++;
        return 0;
    }
    return gh_disk_write(dev, blkno, buf);
}
```

(`gh_block_read` bez zmian. Upewnij się, że `<stdlib.h>` jest dołączone w block.c — realloc.)

- [ ] **Step 3: Nowe API w `src/journal.h`**

```c
#ifndef GH_JOURNAL_H
#define GH_JOURNAL_H
#include "ghostfs.h"
#include "block.h"
int  gh_jrnl_open(struct gh_dev*, const struct gh_superblock*);
void gh_jrnl_op_begin(struct gh_dev*);
void gh_jrnl_op_commit(struct gh_dev*);
void gh_jrnl_op_rollback(struct gh_dev*);
int  gh_jrnl_flush(struct gh_dev*, const struct gh_superblock*);
void gh_jrnl_close(struct gh_dev*);
int  gh_jrnl_recover(struct gh_dev*, const struct gh_superblock*);
#endif
```

- [ ] **Step 4: Przepisz `src/journal.c`** — zastąp `gh_jrnl_begin`/`gh_jrnl_abort`/`gh_jrnl_commit` (zachowaj `raw_write`/`raw_read`/`GH_JPTRS`/`gh_jrnl_recover` bez zmian):

```c
int gh_jrnl_open(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (sb->journal_blocks == 0) { dev->txn = NULL; return 0; }
    struct gh_txn *t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    uint64_t avail = sb->journal_blocks - 1;
    uint64_t dmax = avail / GH_JPTRS + 1;
    if (avail <= dmax) { free(t); return -ENOSPC; }
    t->cap = (uint32_t)(avail - dmax);
    t->blknos = malloc((size_t)t->cap * sizeof(uint64_t));
    t->images = malloc((size_t)t->cap * GH_BLOCK_SIZE);
    if (!t->blknos || !t->images) { free(t->blknos); free(t->images); free(t); return -ENOMEM; }
    t->n = 0; t->active = 1;
    t->op_active = 0; t->savepoint_n = 0; t->savepoint_nd = 0;
    t->undo = NULL; t->nundo = 0; t->undocap = 0; t->dirty = 0;
    dev->txn = t;
    return 0;
}

void gh_jrnl_op_begin(struct gh_dev *dev) {
    struct gh_txn *t = dev->txn;
    if (!t) return;
    t->savepoint_n = t->n; t->savepoint_nd = dev->nd; t->nundo = 0; t->op_active = 1;
}

void gh_jrnl_op_commit(struct gh_dev *dev) {
    struct gh_txn *t = dev->txn;
    if (!t) return;
    t->nundo = 0; t->op_active = 0; t->dirty = 1;
}

void gh_jrnl_op_rollback(struct gh_dev *dev) {
    struct gh_txn *t = dev->txn;
    if (!t) return;
    for (uint32_t k = t->nundo; k > 0; k--)             /* odtworz w odwrotnej kolejnosci */
        memcpy(t->images[t->undo[k-1].idx], t->undo[k-1].img, GH_BLOCK_SIZE);
    t->n = t->savepoint_n;
    dev->nd = t->savepoint_nd;                          /* porzuc discardy operacji */
    t->nundo = 0; t->op_active = 0;
}

void gh_jrnl_close(struct gh_dev *dev) {
    gh_discard_clear(dev);
    if (!dev->txn) return;
    free(dev->txn->undo);
    free(dev->txn->blknos); free(dev->txn->images); free(dev->txn);
    dev->txn = NULL;
}

int gh_jrnl_flush(struct gh_dev *dev, const struct gh_superblock *sb) {
    struct gh_txn *t = dev->txn;
    if (!t) return 0;                                   /* tryb bez dziennika */
    if (!t->dirty || t->n == 0) { t->dirty = 0; return 0; }
    int rc = -EIO;
    uint64_t js = sb->journal_start;
    uint64_t dblocks = (t->n + GH_JPTRS - 1) / GH_JPTRS;
    uint8_t blk[GH_BLOCK_SIZE];

    for (uint64_t d = 0; d < dblocks; d++) {
        memset(blk, 0, sizeof(blk));
        for (uint32_t k = 0; k < GH_JPTRS; k++) {
            uint32_t idx = (uint32_t)(d * GH_JPTRS + k);
            if (idx >= t->n) break;
            memcpy(blk + k * 8, &t->blknos[idx], 8);
        }
        { int r = raw_write(dev, js + 1 + d, blk); if (r) { rc = r; goto fail; } }
    }
    for (uint32_t i = 0; i < t->n; i++)
        { int r = raw_write(dev, js + 1 + dblocks + i, t->images[i]); if (r) { rc = r; goto fail; } }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    struct gh_jheader h; memset(&h, 0, sizeof(h));
    memcpy(h.magic, GH_JMAGIC, 8);
    h.seq = 1; h.committed = 1; h.n_blocks = t->n; h.descriptor_blocks = dblocks;
    memset(blk, 0, sizeof(blk)); memcpy(blk, &h, sizeof(h));
    if (raw_write(dev, js, blk)) { rc = -EIO; goto fail; }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    for (uint32_t i = 0; i < t->n; i++)
        { int r = raw_write(dev, t->blknos[i], t->images[i]); if (r) { rc = r; goto fail; } }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    memset(blk, 0, sizeof(blk));
    if (raw_write(dev, js, blk)) { rc = -EIO; goto fail; }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    gh_discard_flush(dev, sb);
    t->n = 0; t->dirty = 0;          /* reset bufora, zostaw otwarty na kolejna paczke */
    return 0;
fail:
    gh_jrnl_recover(dev, sb);        /* dokoncz ewentualny committed txn na dysku (idempotentne) */
    gh_discard_clear(dev);
    t->n = 0; t->dirty = 0;
    return rc;
}
```

- [ ] **Step 5: Zaktualizuj `tests/test_journal.c` na nowe API**

Zamień użycia: `gh_jrnl_begin`→`gh_jrnl_open`; sekwencja zapisu w transakcji →
`gh_jrnl_op_begin`+zapisy+`gh_jrnl_op_commit`; trwały zapis → `gh_jrnl_flush`; sprzątanie →
`gh_jrnl_close`; abort → `gh_jrnl_op_rollback`. Przykład dla `test_commit_persists`:

```c
    CHECK_EQ(gh_jrnl_open(&dev, &sb), 0);
    CHECK(dev.txn != NULL);
    gh_jrnl_op_begin(&dev);
    char w[GH_BLOCK_SIZE]; memset(w, 0x5A, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, sb.data_start + 5, w), 0);
    gh_jrnl_op_commit(&dev);
    /* przed flush: na dysku zera (raw) */
    char d[GH_BLOCK_SIZE]; raw_read(&dev, sb.data_start + 5, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);
    CHECK_EQ(gh_jrnl_flush(&dev, &sb), 0);
    raw_read(&dev, sb.data_start + 5, d);
    CHECK_EQ(memcmp(d, w, GH_BLOCK_SIZE), 0);    /* po flush trwale */
    char hb[GH_BLOCK_SIZE]; raw_read(&dev, sb.journal_start, hb);
    struct gh_jheader jh; memcpy(&jh, hb, sizeof(jh));
    CHECK_EQ(jh.committed, 0u);
    gh_jrnl_close(&dev);
```

Dla `test_abort_discards` (była): `gh_jrnl_op_begin`→`gh_block_write`→`gh_jrnl_op_rollback`;
sprawdź, że bufor wrócił (kolejny `gh_jrnl_flush` nic nie pisze — `dirty==0`). DODAJ nowy
`test_op_rollback_in_batch`: op A (write blok X, op_commit) potem op B (write blok X innym
wzorcem + write nowy blok Y, op_rollback) → po flush na dysku X ma wzorzec A (nie B), Y
nieobecny (zera). Testy recover (`test_recover_redo`/`test_recover_uncommitted_noop`) — bez
zmian (recover niezmieniony), tylko zamień ewentualne `gh_jrnl_begin`/`abort` na
`open`/`close` jeśli występują.

- [ ] **Step 6: Build + testy**

Run: `make clean && make test 2>&1 | grep -E 'test_journal|failed' ; echo "--- pelny ---"; make test 2>&1 | grep -c '0 failed'`
Expected: na tym etapie `fs.c` jeszcze woła stare API → **błąd kompilacji `fs.c`** (brak
`gh_jrnl_begin`/`commit`/`abort`). To OK — `fs.c` przepinamy w Task 2. Najpierw zbuduj sam
`test_journal` ręcznie:
`cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -Itests tests/test_journal.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c -o build/test_journal_only 2>&1 | head`
— ten link **też** zawiedzie, bo `test_journal.c` linkuje `fs.c`? Nie linkuje `fs.c`. Jeśli
`super.c`/inne nie wołają starego API, `test_journal` zbuduje się i przejdzie. Zweryfikuj i
napraw ewentualne wywołania starego API poza fs.c. Cel: `test_journal` zielony; `fs.c`
naprawiamy w Task 2 (NIE commituj rozjazdu — Task 1 i 2 mogą być jednym commitem jeśli
łatwiej; ale preferowane: Task 1 zostawia drzewo kompilowalne tylko dla test_journal, Task
2 przywraca pełny build). **Aby uniknąć złamanego builda między zadaniami — wykonaj Task 1 i
Task 2 i dopiero wtedy commituj oba (jeden commit „group commit core + fs integration") LUB
w Task 1 od razu zaślepkowo przepnij fs.c (patrz Task 2).**

> **UWAGA wykonawcza:** Najbezpieczniej połączyć Task 1 i Task 2 w jeden ciągły krok (rdzeń
> + integracja fs.c) i dopiero potem build/test/commit, bo API dziennika zmienia się
> niekompatybilnie. Poniższy Task 2 zawiera komplet zmian fs.c.

- [ ] **Step 7: (po Task 2) Commit** — patrz koniec Task 2.

---

## Task 2: Integracja `fs.c` + dostosowanie testów

**Files:** Modify: `src/fs.c`, `tests/test_fs.c`, `tests/test_crash.c`

- [ ] **Step 1: Przepnij helpery transakcji + mount/sync/unmount (`src/fs.c`)**

Zamień helpery `txn_begin`/`txn_end_i`/`txn_end_s`:

```c
static int txn_begin(struct gh_fs *fs) {
    struct gh_txn *t = fs->dev.txn;
    if (t && t->n > t->cap / 2) gh_jrnl_flush(&fs->dev, &fs->sb);   /* pojemnosc */
    gh_jrnl_op_begin(&fs->dev);
    return 0;
}
static int txn_end_i(struct gh_fs *fs, int rc) {
    if (rc < 0) gh_jrnl_op_rollback(&fs->dev);
    else        gh_jrnl_op_commit(&fs->dev);
    return rc;
}
static ssize_t txn_end_s(struct gh_fs *fs, ssize_t rc) {
    if (rc < 0) gh_jrnl_op_rollback(&fs->dev);
    else        gh_jrnl_op_commit(&fs->dev);
    return rc;
}
```

W `gh_fs_mount_key`, po `gh_jrnl_recover(...)` (i przed `return 0`), otwórz running txn:

```c
    r = gh_jrnl_recover(&fs->dev, &fs->sb);
    if (r) { /* dotychczasowe sprzatanie */ ... return r; }
    r = gh_jrnl_open(&fs->dev, &fs->sb);
    if (r) { /* sprzatanie: bcache_destroy, wipe+free cipher, dev_close */ ... return r; }
    return 0;
```

(Dostosuj do faktycznej obsługi błędu w `gh_fs_mount_key` — przy błędzie `gh_jrnl_open`
zwolnij cache, klucz, zamknij dev.)

`gh_fs_sync`:

```c
int gh_fs_sync(struct gh_fs *fs) {
    int r = gh_jrnl_flush(&fs->dev, &fs->sb);
    if (r) return r;
    return fsync(fs->dev.fd) == 0 ? 0 : -errno;
}
```

`gh_fs_unmount`:

```c
void gh_fs_unmount(struct gh_fs *fs) {
    gh_jrnl_flush(&fs->dev, &fs->sb);    /* utrwal ostatnia paczke */
    gh_jrnl_close(&fs->dev);
    gh_bcache_destroy(&fs->dev);
    if (fs->dev.cipher) { gh_crypto_wipe(fs->dev.cipher); free(fs->dev.cipher); fs->dev.cipher = NULL; }
    gh_dev_close(&fs->dev);
}
```

(Usuń wszelkie pozostałe wywołania `gh_jrnl_begin`/`gh_jrnl_commit`/`gh_jrnl_abort` w fs.c —
helpery powyżej są jedynymi punktami styku.)

- [ ] **Step 2: Dostosuj `tests/test_crash.c`** — utrwal prep przed `fail_after`:

W `test_crash_atomic`, po `gh_fs_mkdir(&fs, "/d", 0755)` (prep), dodaj `gh_fs_sync(&fs);`
PRZED `fs.dev.fail_after = N;`. (Prep staje się trwały; awaria dotyczy flushu operacji przy
unmount; atomowość paczki zachowana, `fsck==0` dla każdego N.)

- [ ] **Step 3: Dostosuj `tests/test_fs.c` `test_discard_on_free`** — flush przed sprawdzeniem dysku:

Po `gh_fs_unlink(&fs, "/big")` dodaj `CHECK_EQ(gh_fs_sync(&fs), 0);` PRZED `stat(tmp, &sb2)`
(discardy wykonywane są przy flush). Analogicznie, jeśli `test_discard_punch` (test_block)
działa na surowym `gh_dev` bez fs — bez zmian (tam nie ma running txn fs-poziomu).

- [ ] **Step 4: Dodaj testy group commit w `tests/test_fs.c`** (+ RUN_TEST):

```c
static void test_batch_atomicity(void) {
    char tmp[] = "/tmp/ghost_baXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 256, 64), 0);   /* maly dziennik -> latwy ENOSPC w paczce */
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* operacja A: udana */
    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    const char *ma = "tresc-A";
    CHECK_EQ(gh_fs_write(&fs, "/a", ma, strlen(ma), 0), (ssize_t)strlen(ma));
    /* operacja B: zapis wiekszy niz pojemnosc dziennika -> ENOSPC, pelne wycofanie */
    CHECK_EQ(gh_fs_create(&fs, "/b", 0644), 0);
    char *big = malloc(2 * 1024 * 1024); memset(big, 'b', 2*1024*1024);
    ssize_t w = gh_fs_write(&fs, "/b", big, 2*1024*1024, 0);
    CHECK(w < 0);   /* nie zmiescilo sie -> blad, operacja wycofana */
    free(big);
    /* A nadal spojna (read-your-writes), fsck czysty */
    char buf[16] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/a", buf, sizeof(buf), 0), (ssize_t)strlen(ma));
    CHECK_EQ(memcmp(buf, ma, strlen(ma)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_durability_flush(void) {
    char tmp[] = "/tmp/ghost_duXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f", 0644), 0);
    const char *m = "trwale-po-flush";
    CHECK_EQ(gh_fs_write(&fs, "/d/f", m, strlen(m), 0), (ssize_t)strlen(m));
    /* bez jawnego sync: unmount flushuje */
    gh_fs_unmount(&fs);
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char b[32] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m));
    CHECK_EQ(memcmp(b, m, strlen(m)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj `RUN_TEST(test_batch_atomicity); RUN_TEST(test_durability_flush);`.

- [ ] **Step 5: Build + pełne testy + ASan**

Run: `make clean && make test && make test-asan`
Expected: wszystkie zestawy `0 failed` (w tym test_journal z nowym API, test_crash z flush
prep, test_discard_on_free z sync, nowe test_batch_atomicity/test_durability_flush); brak
raportów ASan (zero wycieków undo/txn — `gh_jrnl_close` zwalnia `undo`).

- [ ] **Step 6: Integracja FUSE + stres współbieżności + urządzenie blokowe**

Run: `make cli fuse && ./tests/integration.sh && ./tests/integration_blockdev.sh`
Expected: `WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY` (w tym 8 pisarzy + recover po kill -9 +
fsck czysty); urządzenie blokowe `WSZYSTKIE TESTY URZADZENIA BLOKOWEGO PRZESZLY`. Posprzątaj
mounty. (kill -9 w trakcie pracy = utrata niezflushowanej paczki, ale remount+recover daje
FS spójny; fsck czysty.)

- [ ] **Step 7: Commit (Task 1 + 2 razem)**

```bash
git add src/block.h src/block.c src/journal.h src/journal.c src/fs.c tests/test_journal.c tests/test_fs.c tests/test_crash.c
git commit -m "feat: group commit — running txn z savepoint/undo, flush na fsync/pojemnosc/unmount"
```

---

## Task 3: Benchmark (mniej fsync) + pełna regresja

**Files:** Modify: `tests/test_bench.c`

- [ ] **Step 1: Rozszerz benchmark o jeden flush na koniec (`tests/test_bench.c`)**

W `bench_one`, po pętli create+write, dodaj jeden `gh_fs_sync(&fs)` i zmierz całość; wypisz
że to teraz JEDEN flush zamiast M commitów. Asercje poprawności i `fsck==0` bez zmian.
Przykład (dodaj po pętli zapisu, przed pomiarem odczytu):

```c
    gh_fs_sync(&fs);   /* jeden trwaly flush calej paczki (group commit) */
```

(Wartości informacyjne; oczekiwany wyraźnie krótszy czas create+write niż przed I, bo ~1
flush zamiast M×4 fsync. W raporcie podaj zmierzone czasy.)

- [ ] **Step 2: Uruchom benchmark + pełna regresja**

Run: `make clean && make test 2>&1 | grep -A6 'TEST test_bench'`
Expected: `test_bench` `0 failed`, czasy wypisane (create+write istotnie szybsze). Następnie
`make test-asan` → wszystkie `0 failed`.

- [ ] **Step 3: Crash-sweep potwierdzenie**

Run: `make test 2>&1 | grep -A2 'TEST test_crash'`
Expected: `test_crash` `0 failed` — atomowość paczki po flush dla każdego N=1..40 (`fsck==0`).

- [ ] **Step 4: Commit**

```bash
git add tests/test_bench.c
git commit -m "test: benchmark group commit (jeden flush zamiast commit-per-op)"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec I:** running txn + savepoint/undo (Task 1) ✓; flush na fsync/pojemność/
unmount + integracja fs.c (Task 2) ✓; dostosowanie testów (discard/crash) + atomowość paczki
+ trwałość (Task 2) ✓; benchmark mniej-fsync (Task 3) ✓.
**Placeholdery:** brak; pełny kod journal.c/block.c/fs.c.
**Spójność:** API `gh_jrnl_open/op_begin/op_commit/op_rollback/flush/close` spójne między
journal.h/journal.c/fs.c; `gh_txn` pola savepoint/undo/dirty używane w block.c (undo push) i
journal.c (op_*); flush = dawny 4-fazowy commit z resetem bufora zamiast free.
**Ryzyka:** (1) niekompatybilna zmiana API dziennika → Task 1+2 jako jeden commit (build
spójny); (2) trwałość po flush → testy sprawdzające dysk dostają `gh_fs_sync` (discard/crash/
enc-at-rest przez unmount); (3) per-op atomowość przez undo „nie modyfikuj bez undo" (write
zwraca -ENOMEM zamiast zostawić slot bez undo); (4) flush-fail → `gh_jrnl_recover` dokańcza
committed txn (on-disk spójny); (5) współbieżność: flush/op pod wrlock (D) — bez wyścigu.
