# ghostfs pod-projekt B — journaling: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; zadania sekwencyjne (wspólne pliki: block.h/.c, super.c, fs.c). Bramka: zielone testy + ASan.

**Goal:** Atomowość operacji wielokrokowych przez fizyczny dziennik redo, przezroczysty dla rdzenia (transakcja wpięta w warstwę bloków). Zgodnie ze spec `docs/superpowers/specs/2026-06-23-ghostfs-B-journaling-design.md`.

**Architecture:** Gdy transakcja aktywna, `gh_block_write` buforuje obrazy bloków, `gh_block_read` czyta z bufora (read-your-writes). `journal.c` zapisuje dziennik (deskryptor+obrazy), zatwierdza z barierami `fsync`, robi checkpoint na docelowe bloki, czyści dziennik. Przy montowaniu `gh_jrnl_recover` odtwarza zatwierdzone-lecz-niedokończone transakcje. `fs.c` opakowuje operacje modyfikujące w begin/commit. Format kompatybilny wstecz (`journal_blocks==0` = tryb bez dziennika).

**Tech Stack:** C11, mini-harness, ASan, libfuse3.

## Global Constraints
- Testy przez `make` (Makefile ma `-D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror`). Gołe `cc` wymagają tej flagi.
- Każde zadanie: zielone testy + ASan + commit.
- Rozmiar i-węzła i superbloku bez zmian fizycznych (nowe pola superbloku z rezerwy).

## File Structure
| Plik | Zmiana | Odpowiedzialność |
|---|---|---|
| `src/ghostfs.h` | mod | pola `journal_start`/`journal_blocks` w superbloku; `struct gh_jheader`; `GH_JMAGIC` |
| `src/block.h` | mod | `struct gh_txn`; pole `txn` w `gh_dev`; include ghostfs.h |
| `src/block.c` | mod | txn-aware `gh_block_read`/`write`; init `txn=NULL` |
| `src/super.c` | mod | `gh_format` alokuje region dziennika; ustawia pola |
| `src/journal.c/.h` | nowe | begin/commit/abort/recover |
| `src/fs.c` | mod | opakowanie operacji w transakcje; recover w mount |
| `Makefile` | mod | `src/journal.c` do CORE |
| `tests/test_super.c` | mod | layout z dziennikiem (dokładne wartości) |
| `tests/test_journal.c` | nowe | testy dziennika + odtwarzania |
| `tests/test_fs.c` | mod | symulacja awarii + recover |

---

## Task 1: Pola dziennika w superbloku + nagłówek dziennika (nagłówki)

**Files:** Modify: `src/ghostfs.h`

- [ ] **Step 1: Dodaj pola dziennika do `struct gh_superblock`** (po `root_inode`):

```c
    uint64_t root_inode;
    uint64_t journal_start;   /* pierwszy blok regionu dziennika (0 = brak) */
    uint64_t journal_blocks;  /* rozmiar dziennika w blokach (0 = brak) */
```

- [ ] **Step 2: Dodaj magic i nagłówek dziennika** (przed `#endif`, po `struct gh_dirent`):

```c
#define GH_JMAGIC "GHJRNL\0\1"   /* 8 bajtów */

struct gh_jheader {              /* pierwszy blok regionu dziennika */
    uint8_t  magic[8];
    uint64_t seq;
    uint32_t committed;          /* 1 = transakcja kompletna na dysku */
    uint32_t n_blocks;           /* liczba obrazów bloków */
    uint64_t descriptor_blocks;  /* ile bloków deskryptora */
};
```

- [ ] **Step 3: Zbuduj — istniejące testy mają przejść** (superblok nadal mieści się w 4096 B; pola w rezerwie):

Run: `make clean && make test`
Expected: część testów `test_super` może zafailować na DOKŁADNYCH wartościach layoutu — to oczekiwane, naprawimy w Task 3 (format). Pozostałe (alloc/block/inode/dir/fs/props) `0 failed`. Jeśli `test_super` failuje WYŁĄCZNIE na wartościach data_start/layout — to OK na tym etapie. Jeśli coś innego — to błąd.

- [ ] **Step 4: Commit**

```bash
git add src/ghostfs.h
git commit -m "feat: pola journal_start/journal_blocks + gh_jheader (superblok)"
```

---

## Task 2: Transakcyjna warstwa bloków

**Files:** Modify: `src/block.h`, `src/block.c`; Test: `tests/test_block.c`

- [ ] **Step 1: Rozszerz `src/block.h`** — include, `struct gh_txn`, pole `txn`:

```c
#ifndef GH_BLOCK_H
#define GH_BLOCK_H
#include <stdint.h>
#include "ghostfs.h"

struct gh_txn {
    int       active;
    uint64_t *blknos;                  /* docelowe numery bloków (deduplikowane) */
    uint8_t (*images)[GH_BLOCK_SIZE];  /* obrazy bloków */
    uint32_t  n, cap;
};

struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn; };

int  gh_dev_create(const char *path, uint64_t total_blocks, struct gh_dev *dev);
int  gh_dev_open(const char *path, struct gh_dev *dev);
void gh_dev_close(struct gh_dev *dev);
int  gh_block_read(struct gh_dev *dev, uint64_t blkno, void *buf);
int  gh_block_write(struct gh_dev *dev, uint64_t blkno, const void *buf);
#endif
```

- [ ] **Step 2: Napisz failujący test w `tests/test_block.c`** (dodaj funkcję + `RUN_TEST`):

```c
static void test_txn_buffer(void) {
    char tmp[] = "/tmp/ghost_txnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 16, &dev), 0);

    /* recznie zbuduj transakcje (bufor na 4 bloki) */
    struct gh_txn t; memset(&t, 0, sizeof(t));
    t.cap = 4; t.blknos = malloc(4 * sizeof(uint64_t));
    t.images = malloc(4 * GH_BLOCK_SIZE); t.active = 1;
    dev.txn = &t;

    char w[GH_BLOCK_SIZE]; memset(w, 0xCD, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, 5, w), 0);     /* trafia do bufora, nie na dysk */

    /* read-your-writes: widac w transakcji */
    char r[GH_BLOCK_SIZE]; memset(r, 0, sizeof(r));
    CHECK_EQ(gh_block_read(&dev, 5, r), 0);
    CHECK_EQ(memcmp(r, w, GH_BLOCK_SIZE), 0);

    /* na dysku jeszcze nic (czytamy bez transakcji) */
    dev.txn = NULL;
    char d[GH_BLOCK_SIZE]; memset(d, 0xFF, sizeof(d));
    CHECK_EQ(gh_block_read(&dev, 5, d), 0);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* kontener wyzerowany */

    /* dedup: drugi zapis tego samego bloku nie zwieksza n */
    dev.txn = &t;
    CHECK_EQ(gh_block_write(&dev, 5, w), 0);
    CHECK_EQ(t.n, 1u);

    /* przepelnienie: zapelnij do cap, kolejny -> ENOSPC */
    CHECK_EQ(gh_block_write(&dev, 6, w), 0);
    CHECK_EQ(gh_block_write(&dev, 7, w), 0);
    CHECK_EQ(gh_block_write(&dev, 8, w), 0);
    CHECK_EQ(t.n, 4u);
    CHECK_EQ(gh_block_write(&dev, 9, w), -ENOSPC);

    free(t.blknos); free(t.images);
    dev.txn = NULL; gh_dev_close(&dev); unlink(tmp);
}
```

Dodaj `#include <stdlib.h>`, `#include <string.h>`, `#include <errno.h>` jeśli brakuje, i `RUN_TEST(test_txn_buffer);` w main.

- [ ] **Step 2b: Uruchom — ma failować** (na razie `gh_block_write` ignoruje `txn`):

Run: `make clean && make test 2>&1 | grep -A3 test_block | head`
Expected: FAIL w `test_txn_buffer` (zapis idzie na dysk / brak logiki txn).

- [ ] **Step 3: Zaimplementuj logikę txn w `src/block.c`**

Dodaj `#include <string.h>` i `#include <errno.h>` jeśli brak. W `gh_dev_create` i `gh_dev_open` ustaw `dev->txn = NULL;`. Zastąp `gh_block_read`/`gh_block_write`:

```c
int gh_block_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) { memcpy(buf, t->images[i], GH_BLOCK_SIZE); return 0; }
    }
    return io_at(dev, blkno, buf, 0);
}
int gh_block_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) { memcpy(t->images[i], buf, GH_BLOCK_SIZE); return 0; }
        if (t->n >= t->cap) return -ENOSPC;
        t->blknos[t->n] = blkno; memcpy(t->images[t->n], buf, GH_BLOCK_SIZE); t->n++;
        return 0;
    }
    return io_at(dev, blkno, (void *)buf, 1);
}
```

- [ ] **Step 4: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_block|failed' | head`
Expected: `test_block` `0 failed`; cała reszta poza ewentualnym `test_super` (layout) też.

- [ ] **Step 5: ASan dla test_block**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_block.c src/block.c -o build/test_block_asan && ./build/test_block_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 6: Commit**

```bash
git add src/block.h src/block.c tests/test_block.c
git commit -m "feat: transakcyjna warstwa bloków (bufor txn + read-your-writes)"
```

---

## Task 3: Format z regionem dziennika

**Files:** Modify: `src/super.c`, `tests/test_super.c`

- [ ] **Step 1: Zaktualizuj test layoutu `tests/test_super.c`**

W `test_format_then_mount` (po `gh_mount_sb`) dodaj asercje dziennika i popraw oczekiwania layoutu:

```c
    /* dziennik istnieje i jest miedzy i-wezlami a danymi */
    CHECK(sb.journal_blocks > 0);
    CHECK_EQ(sb.journal_start, sb.inode_start + ((sb.inode_count / GH_INODES_PER_BLK)));
    CHECK_EQ(sb.data_start, sb.journal_start + sb.journal_blocks);
```

(Jeśli istniejące asercje sprawdzają DOKŁADNĄ wartość `data_start` bez dziennika — zaktualizuj je tak, by `data_start == journal_start + journal_blocks`.)

- [ ] **Step 2: Uruchom — ma failować** (format nie tworzy jeszcze dziennika):

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -g -Itests tests/test_super.c src/block.c src/super.c -o build/test_super && ./build/test_super`
Expected: FAIL (journal_blocks==0).

- [ ] **Step 3: Zmodyfikuj `gh_format` w `src/super.c`**

Zmień wyliczenie layoutu: po `inode_blocks`, przed `data_start`:

```c
    uint64_t inode_blocks  = ceil_div(inode_count, GH_INODES_PER_BLK);
    uint64_t bitmap_start = 1;
    uint64_t inode_start  = bitmap_start + bitmap_blocks;
    uint64_t journal_start = inode_start + inode_blocks;
    uint64_t journal_blocks = total_blocks / 16;
    if (journal_blocks < 8) journal_blocks = 8;
    if (journal_blocks > 4096) journal_blocks = 4096;
    uint64_t data_start   = journal_start + journal_blocks;
    if (data_start >= total_blocks) return -EINVAL;
```

Ustaw pola superbloku (po pozostałych `sb.*`):

```c
    sb.journal_start = journal_start;
    sb.journal_blocks = journal_blocks;
```

(Pętla zaznaczająca bity metadanych `[0, data_start)` automatycznie obejmie region dziennika — bez zmian. `ceil_div` już istnieje.)

- [ ] **Step 4: Uruchom test — ma przejść**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -Itests tests/test_super.c src/block.c src/super.c -o build/test_super && ./build/test_super`
Expected: `0 failed`. Następnie pełny `make clean && make test` → wszystkie `0 failed` (format z dziennikiem nie psuje inode/dir/fs, bo bloki danych dalej alokowane od `data_start`).

- [ ] **Step 5: ASan + pełny make test**

Run: `make test-asan`
Expected: wszystkie `0 failed`, brak raportów.

- [ ] **Step 6: Commit**

```bash
git add src/super.c tests/test_super.c
git commit -m "feat: gh_format alokuje region dziennika (journal_start/blocks)"
```

---

## Task 4: Silnik dziennika (begin/commit/abort/recover)

**Files:** Create: `src/journal.h`, `src/journal.c`, `tests/test_journal.c`; Modify: `Makefile`

- [ ] **Step 1: Utwórz `src/journal.h`**

```c
#ifndef GH_JOURNAL_H
#define GH_JOURNAL_H
#include "ghostfs.h"
#include "block.h"
int  gh_jrnl_begin(struct gh_dev*, const struct gh_superblock*);
int  gh_jrnl_commit(struct gh_dev*, const struct gh_superblock*);
void gh_jrnl_abort(struct gh_dev*);
int  gh_jrnl_recover(struct gh_dev*, const struct gh_superblock*);
#endif
```

- [ ] **Step 2: Napisz failujący test `tests/test_journal.c`**

```c
#include "test.h"
#include "../src/block.h"
#include "../src/super.h"
#include "../src/journal.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

/* niskopoziomowy odczyt bloku z pominieciem transakcji */
static void raw_read(struct gh_dev *dev, uint64_t b, void *buf) {
    pread(dev->fd, buf, GH_BLOCK_SIZE, (off_t)b * GH_BLOCK_SIZE);
}

static void test_commit_persists(void) {
    char tmp[] = "/tmp/ghost_jcXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    CHECK_EQ(gh_dev_open(tmp, &dev), 0); CHECK_EQ(gh_mount_sb(&dev, &sb), 0);

    CHECK_EQ(gh_jrnl_begin(&dev, &sb), 0);
    CHECK(dev.txn != NULL);
    char w[GH_BLOCK_SIZE]; memset(w, 0x5A, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, sb.data_start, w), 0);

    /* przed commit: na dysku zera (raw) */
    char d[GH_BLOCK_SIZE]; raw_read(&dev, sb.data_start, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);

    CHECK_EQ(gh_jrnl_commit(&dev, &sb), 0);
    CHECK(dev.txn == NULL);
    /* po commit: blok na dysku ma dane */
    raw_read(&dev, sb.data_start, d);
    CHECK_EQ(memcmp(d, w, GH_BLOCK_SIZE), 0);
    /* naglowek dziennika wyczyszczony (committed=0) */
    char h[GH_BLOCK_SIZE]; raw_read(&dev, sb.journal_start, h);
    struct gh_jheader jh; memcpy(&jh, h, sizeof(jh));
    CHECK_EQ(jh.committed, 0u);

    gh_dev_close(&dev); unlink(tmp);
}

static void test_abort_discards(void) {
    char tmp[] = "/tmp/ghost_jaXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    CHECK_EQ(gh_jrnl_begin(&dev, &sb), 0);
    char w[GH_BLOCK_SIZE]; memset(w, 0x11, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, sb.data_start, w), 0);
    gh_jrnl_abort(&dev);
    CHECK(dev.txn == NULL);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, sb.data_start, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* nic nie zapisane */
    gh_dev_close(&dev); unlink(tmp);
}

static void test_recover_redo(void) {
    char tmp[] = "/tmp/ghost_jrXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    /* spreparuj zatwierdzona transakcje recznie: 1 blok docelowy = data_start */
    uint64_t js = sb.journal_start;
    uint64_t target = sb.data_start + 3;
    char img[GH_BLOCK_SIZE]; memset(img, 0xA7, sizeof(img));
    /* deskryptor: blok js+1, pierwszy uint64 = target */
    char db[GH_BLOCK_SIZE]; memset(db, 0, sizeof(db)); memcpy(db, &target, 8);
    pwrite(dev.fd, db, GH_BLOCK_SIZE, (off_t)(js + 1) * GH_BLOCK_SIZE);
    /* obraz: blok js+2 */
    pwrite(dev.fd, img, GH_BLOCK_SIZE, (off_t)(js + 2) * GH_BLOCK_SIZE);
    /* naglowek committed=1 */
    struct gh_jheader jh; memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.seq = 1; jh.committed = 1;
    jh.n_blocks = 1; jh.descriptor_blocks = 1;
    char hb[GH_BLOCK_SIZE]; memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    /* recover -> target ma obraz, naglowek wyczyszczony */
    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, target, d);
    CHECK_EQ(memcmp(d, img, GH_BLOCK_SIZE), 0);
    raw_read(&dev, js, hb); memcpy(&jh, hb, sizeof(jh));
    CHECK_EQ(jh.committed, 0u);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_recover_uncommitted_noop(void) {
    char tmp[] = "/tmp/ghost_juXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);
    uint64_t js = sb.journal_start, target = sb.data_start + 1;
    char img[GH_BLOCK_SIZE]; memset(img, 0x44, sizeof(img));
    char db[GH_BLOCK_SIZE]; memset(db, 0, sizeof(db)); memcpy(db, &target, 8);
    pwrite(dev.fd, db, GH_BLOCK_SIZE, (off_t)(js + 1) * GH_BLOCK_SIZE);
    pwrite(dev.fd, img, GH_BLOCK_SIZE, (off_t)(js + 2) * GH_BLOCK_SIZE);
    struct gh_jheader jh; memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.committed = 0;   /* NIE zatwierdzone */
    jh.n_blocks = 1; jh.descriptor_blocks = 1;
    char hb[GH_BLOCK_SIZE]; memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, target, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* nic nie odtworzone */
    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_commit_persists);
    RUN_TEST(test_abort_discards);
    RUN_TEST(test_recover_redo);
    RUN_TEST(test_recover_uncommitted_noop);
    return TEST_SUMMARY();
}
```

- [ ] **Step 3: Dodaj `src/journal.c` do `CORE` w `Makefile`** (przed `src/fs.c`):

```make
CORE    := src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/fs.c
```

- [ ] **Step 4: Uruchom — ma failować** (brak `journal.c`):

Run: `make clean && make test 2>&1 | grep -i journal | head`
Expected: błąd linkera / brak implementacji.

- [ ] **Step 5: Zaimplementuj `src/journal.c`**

```c
#include "journal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define GH_JPTRS (GH_BLOCK_SIZE / 8)   /* 512 numerów bloków na blok deskryptora */

static int raw_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    ssize_t n = pwrite(dev->fd, buf, GH_BLOCK_SIZE, (off_t)blkno * GH_BLOCK_SIZE);
    return (n == (ssize_t)GH_BLOCK_SIZE) ? 0 : -EIO;
}
static int raw_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    ssize_t n = pread(dev->fd, buf, GH_BLOCK_SIZE, (off_t)blkno * GH_BLOCK_SIZE);
    return (n == (ssize_t)GH_BLOCK_SIZE) ? 0 : -EIO;
}

int gh_jrnl_begin(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (sb->journal_blocks == 0) { dev->txn = NULL; return 0; }  /* tryb bez dziennika */
    struct gh_txn *t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    uint64_t avail = sb->journal_blocks - 1;          /* minus naglowek */
    uint64_t dmax = avail / GH_JPTRS + 1;             /* rezerwa na bloki deskryptora */
    if (avail <= dmax) { free(t); return -ENOSPC; }
    t->cap = (uint32_t)(avail - dmax);
    t->blknos = malloc((size_t)t->cap * sizeof(uint64_t));
    t->images = malloc((size_t)t->cap * GH_BLOCK_SIZE);
    if (!t->blknos || !t->images) { free(t->blknos); free(t->images); free(t); return -ENOMEM; }
    t->n = 0; t->active = 1;
    dev->txn = t;
    return 0;
}

void gh_jrnl_abort(struct gh_dev *dev) {
    if (!dev->txn) return;
    free(dev->txn->blknos); free(dev->txn->images); free(dev->txn);
    dev->txn = NULL;
}

int gh_jrnl_commit(struct gh_dev *dev, const struct gh_superblock *sb) {
    struct gh_txn *t = dev->txn;
    if (!t) return 0;                       /* tryb bez dziennika */
    if (t->n == 0) { gh_jrnl_abort(dev); return 0; }
    uint64_t js = sb->journal_start;
    uint64_t dblocks = (t->n + GH_JPTRS - 1) / GH_JPTRS;
    uint8_t blk[GH_BLOCK_SIZE];

    /* faza 1: deskryptor + obrazy do dziennika */
    for (uint64_t d = 0; d < dblocks; d++) {
        memset(blk, 0, sizeof(blk));
        for (uint32_t k = 0; k < GH_JPTRS; k++) {
            uint32_t idx = (uint32_t)(d * GH_JPTRS + k);
            if (idx >= t->n) break;
            memcpy(blk + k * 8, &t->blknos[idx], 8);
        }
        int r = raw_write(dev, js + 1 + d, blk); if (r) return r;
    }
    for (uint32_t i = 0; i < t->n; i++) {
        int r = raw_write(dev, js + 1 + dblocks + i, t->images[i]); if (r) return r;
    }
    if (fsync(dev->fd)) return -EIO;

    /* faza 2: naglowek committed=1 (punkt zatwierdzenia) */
    struct gh_jheader h; memset(&h, 0, sizeof(h));
    memcpy(h.magic, GH_JMAGIC, 8);
    h.seq = 1; h.committed = 1; h.n_blocks = t->n; h.descriptor_blocks = dblocks;
    memset(blk, 0, sizeof(blk)); memcpy(blk, &h, sizeof(h));
    if (raw_write(dev, js, blk)) return -EIO;
    if (fsync(dev->fd)) return -EIO;

    /* faza 3: checkpoint na docelowe bloki */
    for (uint32_t i = 0; i < t->n; i++) {
        int r = raw_write(dev, t->blknos[i], t->images[i]); if (r) return r;
    }
    if (fsync(dev->fd)) return -EIO;

    /* faza 4: wyczysc naglowek */
    memset(blk, 0, sizeof(blk));
    if (raw_write(dev, js, blk)) return -EIO;
    if (fsync(dev->fd)) return -EIO;

    gh_jrnl_abort(dev);
    return 0;
}

int gh_jrnl_recover(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (sb->journal_blocks == 0) return 0;
    uint64_t js = sb->journal_start;
    uint8_t blk[GH_BLOCK_SIZE];
    if (raw_read(dev, js, blk)) return 0;        /* nie da sie czytac -> pomin */
    struct gh_jheader h; memcpy(&h, blk, sizeof(h));
    if (memcmp(h.magic, GH_JMAGIC, 8) != 0) return 0;
    if (!h.committed) return 0;                  /* nie zatwierdzone -> nic */
    if (h.n_blocks == 0 || h.n_blocks > sb->journal_blocks) return 0;

    uint64_t *targets = malloc((size_t)h.n_blocks * sizeof(uint64_t));
    if (!targets) return -ENOMEM;
    for (uint64_t d = 0; d < h.descriptor_blocks; d++) {
        uint8_t db[GH_BLOCK_SIZE];
        if (raw_read(dev, js + 1 + d, db)) { free(targets); return -EIO; }
        for (uint32_t k = 0; k < GH_JPTRS; k++) {
            uint32_t idx = (uint32_t)(d * GH_JPTRS + k);
            if (idx >= h.n_blocks) break;
            memcpy(&targets[idx], db + k * 8, 8);
        }
    }
    for (uint32_t i = 0; i < h.n_blocks; i++) {
        uint8_t img[GH_BLOCK_SIZE];
        if (raw_read(dev, js + 1 + h.descriptor_blocks + i, img)) { free(targets); return -EIO; }
        if (targets[i] < sb->total_blocks) {
            if (raw_write(dev, targets[i], img)) { free(targets); return -EIO; }
        }
    }
    free(targets);
    if (fsync(dev->fd)) return -EIO;
    memset(blk, 0, sizeof(blk));               /* wyczysc naglowek */
    if (raw_write(dev, js, blk)) return -EIO;
    if (fsync(dev->fd)) return -EIO;
    return 0;
}
```

- [ ] **Step 6: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_journal|failed'`
Expected: `test_journal` `0 failed`; cała reszta `0 failed`.

- [ ] **Step 7: ASan dla test_journal**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_journal.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/fs.c -o build/test_journal_asan && ./build/test_journal_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 8: Commit**

```bash
git add src/journal.h src/journal.c tests/test_journal.c Makefile
git commit -m "feat: silnik dziennika (begin/commit/abort/recover) + testy"
```

---

## Task 5: Integracja transakcji w `fs.c` + recover przy montowaniu

**Files:** Modify: `src/fs.c`; Test: `tests/test_fs.c`

- [ ] **Step 1: Dołącz dziennik i wywołaj recover w `gh_fs_mount` (`src/fs.c`)**

Na górze dodaj `#include "journal.h"`. W `gh_fs_mount`, po udanym `gh_mount_sb`, przed `return 0`:

```c
    r = gh_jrnl_recover(&fs->dev, &fs->sb);
    if (r) { gh_dev_close(&fs->dev); return r; }
    return 0;
```

- [ ] **Step 2: Opakuj operacje modyfikujące w transakcje**

Dla KAŻDEJ z funkcji: `gh_fs_create`, `gh_fs_mkdir`, `gh_fs_unlink`, `gh_fs_rmdir`,
`gh_fs_link`, `gh_fs_symlink`, `gh_fs_rename`, `gh_fs_truncate`, `gh_fs_write`,
`gh_fs_chmod`, `gh_fs_chown`, `gh_fs_utimens`, `gh_fs_setxattr`, `gh_fs_removexattr` —
zastosuj wzorzec transakcyjny. Aby uniknąć powtórzeń i błędów, dodaj dwa prywatne
helpery na górze `fs.c` (po include'ach) i użyj ich w każdej operacji:

```c
/* zacznij transakcje; zwraca 0 lub -errno */
static int txn_begin(struct gh_fs *fs) { return gh_jrnl_begin(&fs->dev, &fs->sb); }
/* zakoncz: przy bledzie (rc<0) abort, inaczej commit; zwraca finalny kod (int) */
static int txn_end_i(struct gh_fs *fs, int rc) {
    if (rc < 0) { gh_jrnl_abort(&fs->dev); return rc; }
    int c = gh_jrnl_commit(&fs->dev, &fs->sb);
    return c ? c : rc;
}
/* wariant dla operacji zwracajacych ssize_t (write) */
static ssize_t txn_end_s(struct gh_fs *fs, ssize_t rc) {
    if (rc < 0) { gh_jrnl_abort(&fs->dev); return rc; }
    int c = gh_jrnl_commit(&fs->dev, &fs->sb);
    return c ? (ssize_t)c : rc;
}
```

Wzorzec dla operacji `int` (przykład `gh_fs_create`):

```c
int gh_fs_create(struct gh_fs *fs, const char *path, uint16_t mode) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, GH_FILE, mode));
}
```

Zastosuj analogicznie do `gh_fs_mkdir`, `gh_fs_unlink`, `gh_fs_rmdir`, `gh_fs_link`,
`gh_fs_symlink`, `gh_fs_rename`, `gh_fs_truncate`, `gh_fs_chmod`, `gh_fs_chown`,
`gh_fs_utimens`, `gh_fs_setxattr`, `gh_fs_removexattr` — owijając ich dotychczasowe
ciało: `int b = txn_begin(fs); if (b) return b; return txn_end_i(fs, <stare-cialo-jako-wyrazenie-lub-funkcja>);`.

Jeśli ciało jest wieloliniowe, wydziel je do statycznego helpera `..._locked(fs, ...)`
zwracającego `int`/`ssize_t` i wołaj go między begin a end. Dla `gh_fs_write` użyj
`txn_end_s` i `ssize_t`:

```c
ssize_t gh_fs_write(struct gh_fs *fs, const char *path, const void *buf, size_t n, uint64_t off) {
    int b = txn_begin(fs); if (b) return b;
    ssize_t rc;
    {
        uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
        if (r) { rc = r; }
        else {
            struct gh_inode node; r = gh_inode_read(&fs->dev, &fs->sb, ino, &node);
            if (r) rc = r;
            else if (node.type != GH_FILE) rc = -EISDIR;
            else rc = gh_inode_pwrite(&fs->dev, &fs->sb, ino, &node, buf, n, off);
        }
    }
    return txn_end_s(fs, rc);
}
```

(Operacje tylko-do-odczytu `gh_fs_read/getattr/readdir/statfs/getxattr/listxattr/readlink`
oraz `gh_fs_sync` NIE są opakowywane.)

- [ ] **Step 3: Napisz test symulacji awarii w `tests/test_fs.c`**

Test sprawdza, że po „zatwierdzeniu" transakcji ręczne odtworzenie z dziennika jest
spójne oraz że operacja jest atomowa (albo cała, albo wcale). Dodaj funkcję +
`RUN_TEST`:

```c
static void test_journaled_atomic(void) {
    char tmp[] = "/tmp/ghost_jatXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* operacja przez transakcje dziala normalnie i jest trwala po remount */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f.txt", 0644), 0);
    const char *m = "dane przez dziennik";
    CHECK_EQ(gh_fs_write(&fs, "/d/f.txt", m, strlen(m), 0), (ssize_t)strlen(m));
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);

    /* remount (uruchamia recover; brak zaleglej transakcji) i sprawdz trwalosc */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0), (ssize_t)strlen(m));
    CHECK_EQ(memcmp(buf, m, strlen(m)), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj `RUN_TEST(test_journaled_atomic);`.

- [ ] **Step 4: Uruchom testy — mają przejść**

Run: `make clean && make test`
Expected: wszystkie zestawy `0 failed` (w tym `test_fs` z `test_journaled_atomic`).
Wszystkie istniejące testy A muszą dalej przechodzić (operacje przez transakcje dają
ten sam wynik logiczny).

- [ ] **Step 5: Pełny ASan**

Run: `make test-asan`
Expected: wszystkie `0 failed`, brak raportów (zwłaszcza zero wycieków `txn` — abort/commit zwalniają bufor).

- [ ] **Step 6: Commit**

```bash
git add src/fs.c tests/test_fs.c
git commit -m "feat: operacje fs.c w transakcjach dziennika + recover przy mount"
```

---

## Task 6: Integracja FUSE/CLI + symulacja awarii w skrypcie

**Files:** Modify: `tests/integration.sh`

- [ ] **Step 1: Dodaj test odtwarzania po awarii do `tests/integration.sh`**

Przed sekcją „12) fsck po wszystkim" dodaj krok wymuszonej awarii:

```bash
# 13) symulacja awarii: kill -9 procesu FUSE w trakcie pracy, remount, fsck czysty
echo zawartosc > "$MNT/crash.txt"
mkdir -p "$MNT/cd"
sync
kill -9 "$FPID" 2>/dev/null || true
fusermount3 -u "$MNT" 2>/dev/null || true
wait "$FPID" 2>/dev/null || true
# remount (uruchamia recover)
"$GFS" "$CONT" "$MNT" -f &
FPID=$!; sleep 1
ok 'test -f "$MNT/crash.txt"' 'remount po kill -9 (recover)'
"$CLI" fsck "$CONT" >/dev/null 2>&1 || true
```

(Skrypt już ma funkcję `ok` z Task A11; jeśli używasz `$FPID` po restarcie — upewnij się że zmienna jest nadpisana, jak wyżej.)

- [ ] **Step 2: Uruchom pełną integrację**

Run: `make clean && make test && make cli fuse && ./tests/integration.sh`
Expected: wszystkie `OK:` (w tym `OK: remount po kill -9 (recover)`) i `WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY`. Posprzątaj wiszące mounty.

- [ ] **Step 3: Commit**

```bash
git add tests/integration.sh
git commit -m "test: integracyjna symulacja awarii (kill -9 + remount/recover)"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec B:** pola superbloku + nagłówek (Task 1) ✓; txn-aware block (Task 2) ✓; format z dziennikiem (Task 3) ✓; silnik begin/commit/abort/recover (Task 4) ✓; integracja fs.c + recover-on-mount (Task 5) ✓; symulacja awarii (Task 6) ✓. Kompatybilność wstecz (`journal_blocks==0`) obsłużona w begin/commit/recover ✓.

**Placeholdery:** brak; kod kompletny dla warstwy bloków i journal.c.

**Spójność typów:** `struct gh_txn`/`gh_dev.txn` (block.h), `struct gh_jheader`/`GH_JMAGIC` (ghostfs.h), `gh_jrnl_begin/commit/abort/recover` (journal.h), `txn_begin/txn_end_i/txn_end_s` (fs.c) — spójne między zadaniami. `GH_JPTRS=512` zgodne z deskryptorem (512 numerów na blok).

**Ryzyka odnotowane:** (1) format zmienia layout → test_super zaktualizowany (Task 3); (2) maleńkie kontenery — floor dziennika 8 bloków, by format się powiódł; (3) operacja przekraczająca pojemność dziennika → `-ENOSPC` z `gh_block_write` propagowane jako błąd operacji (abort).
