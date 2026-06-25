# ghostfs pod-projekt F — wydajność: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan + brak regresji semantyki.

**Goal:** Hinty alokacji (koniec skanów O(n²)) + cache bloków write-through (mniej I/O i deszyfrowania) + benchmark. Zero zmian semantyki. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-F-performance-design.md`.

**Tech Stack:** C11, pthreads, OpenSSL, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- KRYTYCZNE: wszystkie testy A–E + integracja + stres współbieżności (D) muszą dalej
  przechodzić — cache/hinty są przezroczyste.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/block.h` | `hint_block`/`hint_inode`/`cache` w `gh_dev`; `struct gh_bcache`/`gh_bentry`; `#include <pthread.h>`; deklaracje `gh_bcache_create/destroy` |
| `src/block.c` | hinty init; cache w `gh_disk_read/write`; `gh_bcache_create/destroy` |
| `src/alloc.c` | `gh_alloc_block`/`gh_free_block` z hintem |
| `src/inode.c` | `gh_inode_alloc`/`gh_inode_free` z hintem |
| `src/fs.c` | mount tworzy cache; unmount niszczy |
| `Makefile` | `-lpthread` do `LDLIBS` |
| `tests/test_fs.c` | masowa alokacja + cache transparentność |
| `tests/bench.c` | benchmark |

---

## Task 1: Hinty alokacji

**Files:** Modify: `src/block.h`, `src/block.c`, `src/alloc.c`, `src/inode.c`; Test: `tests/test_fs.c`

- [ ] **Step 1: Dodaj hinty do `struct gh_dev` (`src/block.h`)**

```c
struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn;
                struct gh_cipher *cipher; long fail_after;
                uint64_t hint_block; uint64_t hint_inode; };
```

- [ ] **Step 2: Inicjalizuj hinty w `src/block.c`** — w `gh_dev_create` i `gh_dev_open`
dodaj `dev->hint_block = 0; dev->hint_inode = 0;` (obok `dev->fail_after = 0;`).

- [ ] **Step 3: Napisz failujący/strażniczy test w `tests/test_fs.c`** (+ RUN_TEST):

```c
static void test_alloc_hint_mass(void) {
    char tmp[] = "/tmp/ghost_ahXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 8192, 1024), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    char name[64];
    /* utworz 400 plikow */
    for (int i = 0; i < 400; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_create(&fs, name, 0644), 0);
        char d[50]; memset(d, 'a', sizeof(d));
        CHECK_EQ(gh_fs_write(&fs, name, d, sizeof(d), 0), (ssize_t)sizeof(d));
    }
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* usun co drugi */
    for (int i = 0; i < 400; i += 2) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_unlink(&fs, name), 0);
    }
    /* utworz 200 nowych — musza trafic w zwolnione zasoby (hint nie pomija) */
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "/g%d", i);
        CHECK_EQ(gh_fs_create(&fs, name, 0644), 0);
    }
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* odczyt zachowanych plikow spojny */
    char buf[64];
    snprintf(name, sizeof(name), "/f1");   /* nieparzysty -> zachowany */
    CHECK_EQ(gh_fs_read(&fs, name, buf, 50, 0), 50);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 4: Zaimplementuj hinty w `src/alloc.c`**

Zastąp `gh_alloc_block` i `gh_free_block`:

```c
int gh_free_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t blkno) {
    if (blkno < sb->data_start || blkno >= sb->total_blocks) return -EINVAL;
    if (blkno < dev->hint_block) dev->hint_block = blkno;
    return bitmap_rw(dev, sb, blkno, 0, 1, NULL);
}

int gh_alloc_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out) {
    if (!out) return -EINVAL;
    uint64_t start = dev->hint_block;
    if (start < sb->data_start || start >= sb->total_blocks) start = sb->data_start;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t lo = (pass == 0) ? start : sb->data_start;
        uint64_t hi = (pass == 0) ? sb->total_blocks : start;
        for (uint64_t b = lo; b < hi; b++) {
            int set = 0; int r = gh_bitmap_test(dev, sb, b, &set); if (r) return r;
            if (!set) {
                r = bitmap_rw(dev, sb, b, 1, 1, NULL); if (r) return r;
                uint8_t zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
                r = gh_block_write(dev, b, zero); if (r) return r;
                dev->hint_block = b + 1;
                *out = b; return 0;
            }
        }
    }
    return -ENOSPC;
}
```

- [ ] **Step 5: Zaimplementuj hint i-węzłów w `src/inode.c`**

Zastąp `gh_inode_alloc`:

```c
int gh_inode_alloc(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint16_t type, uint64_t *out_ino) {
    uint64_t start = dev->hint_inode;
    if (start < GH_ROOT_INO + 1 || start >= sb->inode_count) start = GH_ROOT_INO + 1;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t lo = (pass == 0) ? start : (uint64_t)(GH_ROOT_INO + 1);
        uint64_t hi = (pass == 0) ? sb->inode_count : start;
        for (uint64_t ino = lo; ino < hi; ino++) {
            struct gh_inode n; int r = gh_inode_read(dev, sb, ino, &n);
            if (r) return r;
            if (n.type == GH_FREE) {
                memset(&n, 0, sizeof(n));
                n.type = type; n.mode = (type == GH_DIR) ? 0755 : 0644;
                n.nlink = 1; n.atime = n.mtime = n.ctime = (uint64_t)time(NULL);
                r = gh_inode_write(dev, sb, ino, &n); if (r) return r;
                dev->hint_inode = ino + 1;
                *out_ino = ino; return 0;
            }
        }
    }
    return -ENOSPC;
}
```

W `gh_inode_free`, tuż przed finalnym `return` (po zapisaniu i-węzła jako `GH_FREE`),
dodaj aktualizację hintu:

```c
    if (ino < dev->hint_inode) dev->hint_inode = ino;
```

(Umieść tę linię tam, gdzie masz dostęp do `ino` i `dev` — w `gh_inode_free` przed
`return r ? r : first_err;`.)

- [ ] **Step 6: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_fs|test_alloc|test_inode|failed'`
Expected: wszystkie `0 failed` (poprawność alokacji zachowana, hinty nie pomijają wolnych
zasobów — test masowy z usuwaniem to potwierdza).

- [ ] **Step 7: ASan + pełny make test**

Run: `make test-asan`
Expected: wszystkie `0 failed`.

- [ ] **Step 8: Commit**

```bash
git add src/block.h src/block.c src/alloc.c src/inode.c tests/test_fs.c
git commit -m "perf: hinty alokacji blokow/i-wezlow (koniec skanow O(n^2))"
```

---

## Task 2: Cache bloków (write-through, bezpieczny wątkowo)

**Files:** Modify: `src/block.h`, `src/block.c`, `src/fs.c`, `Makefile`; Test: `tests/test_fs.c`

- [ ] **Step 1: Rozszerz `src/block.h`** — include, struktury, pole, deklaracje:

```c
#include <pthread.h>
/* ... istniejace ... */
struct gh_bentry { uint64_t blkno; int valid; uint8_t data[GH_BLOCK_SIZE]; };
struct gh_bcache { pthread_mutex_t lock; uint32_t nslots; struct gh_bentry *slots; };
#define GH_BCACHE_SLOTS 1024u

struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn;
                struct gh_cipher *cipher; long fail_after;
                uint64_t hint_block; uint64_t hint_inode; struct gh_bcache *cache; };

int  gh_bcache_create(struct gh_dev *dev);
void gh_bcache_destroy(struct gh_dev *dev);
```

(Pole `cache` dodane do `gh_dev`; pamiętaj o `dev->cache = NULL;` w create/open — Step 2.)

- [ ] **Step 2: Implementacja cache w `src/block.c`**

Dodaj `#include <pthread.h>` i `#include <stdlib.h>` (jeśli brak). W `gh_dev_create` i
`gh_dev_open` dodaj `dev->cache = NULL;`. Dodaj funkcje:

```c
int gh_bcache_create(struct gh_dev *dev) {
    struct gh_bcache *c = calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    c->nslots = GH_BCACHE_SLOTS;
    c->slots = calloc(c->nslots, sizeof(struct gh_bentry));
    if (!c->slots) { free(c); return -ENOMEM; }
    if (pthread_mutex_init(&c->lock, NULL) != 0) { free(c->slots); free(c); return -EIO; }
    dev->cache = c;
    return 0;
}
void gh_bcache_destroy(struct gh_dev *dev) {
    if (!dev->cache) return;
    pthread_mutex_destroy(&dev->cache->lock);
    free(dev->cache->slots); free(dev->cache); dev->cache = NULL;
}
```

- [ ] **Step 3: Zintegruj cache w `gh_disk_read`/`gh_disk_write` (`src/block.c`)**

Zastąp obie funkcje pełnymi wersjami (hook `fail_after` + szyfr + cache):

```c
int gh_disk_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    if (blkno >= dev->total_blocks) return -EINVAL;
    struct gh_bcache *c = dev->cache;
    if (c && blkno != 0) {
        struct gh_bentry *e = &c->slots[blkno % c->nslots];
        pthread_mutex_lock(&c->lock);
        if (e->valid && e->blkno == blkno) {
            memcpy(buf, e->data, GH_BLOCK_SIZE);
            pthread_mutex_unlock(&c->lock);
            return 0;
        }
        pthread_mutex_unlock(&c->lock);
    }
    off_t off = (off_t)blkno * GH_BLOCK_SIZE;
    if (dev->cipher && blkno != 0) {
        uint8_t tmp[GH_BLOCK_SIZE];
        ssize_t n = pread(dev->fd, tmp, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
        int r = gh_crypto_decrypt_block(dev->cipher, blkno, tmp, buf);
        if (r) return r;
    } else {
        ssize_t n = pread(dev->fd, buf, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
    }
    if (c && blkno != 0) {
        struct gh_bentry *e = &c->slots[blkno % c->nslots];
        pthread_mutex_lock(&c->lock);
        e->blkno = blkno; e->valid = 1; memcpy(e->data, buf, GH_BLOCK_SIZE);
        pthread_mutex_unlock(&c->lock);
    }
    return 0;
}

int gh_disk_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (dev->fail_after > 0) {
        if (--dev->fail_after == 0) return -EIO;
    }
    if (blkno >= dev->total_blocks) return -EINVAL;
    off_t off = (off_t)blkno * GH_BLOCK_SIZE;
    if (dev->cipher && blkno != 0) {
        uint8_t tmp[GH_BLOCK_SIZE];
        int r = gh_crypto_encrypt_block(dev->cipher, blkno, buf, tmp); if (r) return r;
        ssize_t n = pwrite(dev->fd, tmp, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
    } else {
        ssize_t n = pwrite(dev->fd, buf, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
    }
    struct gh_bcache *c = dev->cache;
    if (c && blkno != 0) {
        struct gh_bentry *e = &c->slots[blkno % c->nslots];
        pthread_mutex_lock(&c->lock);
        e->blkno = blkno; e->valid = 1; memcpy(e->data, buf, GH_BLOCK_SIZE);
        pthread_mutex_unlock(&c->lock);
    }
    return 0;
}
```

(Uwaga: zachowaj istniejące `#include <errno.h>`/`<string.h>` w block.c.)

- [ ] **Step 4: Cykl życia cache w `src/fs.c`**

W `gh_fs_mount_key`, po pomyślnym ustawieniu `dev` i ewentualnego szyfru, a przed
`gh_jrnl_recover`, włącz cache:

```c
    if (gh_bcache_create(&fs->dev) != 0) {
        free(fs->dev.cipher); fs->dev.cipher = NULL;
        gh_dev_close(&fs->dev); return -ENOMEM;
    }
```

W `gh_fs_unmount`, przed `gh_dev_close`, zniszcz cache:

```c
void gh_fs_unmount(struct gh_fs *fs) {
    gh_bcache_destroy(&fs->dev);
    free(fs->dev.cipher); fs->dev.cipher = NULL;
    gh_dev_close(&fs->dev);
}
```

(Recover po `gh_bcache_create` korzysta z cache — w porządku, świeży cache pusty.)

- [ ] **Step 5: `-lpthread` do `LDLIBS` w `Makefile`**

```make
LDLIBS  := -lcrypto -lpthread
```

(Cel `fuse` może zachować dodatkowe `-lpthread` — duplikat nieszkodliwy — lub usuń je,
skoro `LDLIBS` już je zawiera.)

- [ ] **Step 6: Napisz test transparentności cache w `tests/test_fs.c`** (+ RUN_TEST):

```c
static void test_cache_transparent(void) {
    char tmp[] = "/tmp/ghost_caXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK(fs.dev.cache != NULL);              /* cache wlaczony przez mount */

    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f", 0644), 0);
    const char *m = "tresc-przez-cache-1234567890";
    CHECK_EQ(gh_fs_write(&fs, "/d/f", m, strlen(m), 0), (ssize_t)strlen(m));
    /* wielokrotny odczyt (trafienia cache) spojny */
    for (int i = 0; i < 5; i++) {
        char b[64] = {0};
        CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m));
        CHECK_EQ(memcmp(b, m, strlen(m)), 0);
    }
    /* nadpisanie -> odczyt widzi nowa tresc (write-through) */
    const char *m2 = "NOWA";
    CHECK_EQ(gh_fs_write(&fs, "/d/f", m2, strlen(m2), 0), (ssize_t)strlen(m2));
    char b[64] = {0}; CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m2));
    CHECK_EQ(memcmp(b, m2, strlen(m2)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);

    /* remount (swiezy cache) -> dane trwale i spojne */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    memset(b, 0, sizeof(b));
    CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m2));
    CHECK_EQ(memcmp(b, m2, strlen(m2)), 0);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 7: Uruchom testy — mają przejść**

Run: `make clean && make test`
Expected: wszystkie `0 failed` (w tym `test_cache_transparent`; cała reszta A–E bez zmian).

- [ ] **Step 8: Pełny ASan**

Run: `make test-asan`
Expected: wszystkie `0 failed`, brak raportów (zwłaszcza zero wycieków cache/mutex —
unmount niszczy).

- [ ] **Step 9: Commit**

```bash
git add src/block.h src/block.c src/fs.c Makefile tests/test_fs.c
git commit -m "perf: cache blokow write-through (direct-mapped, bezpieczny watkowo)"
```

---

## Task 3: Benchmark + regresja współbieżności

**Files:** Create: `tests/bench.c`; Modify: `tests/integration.sh`

- [ ] **Step 1: Utwórz `tests/bench.c`**

```c
#include "test.h"
#include "../src/fs.h"
#include "../src/super.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

static double secs(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static void bench_one(const char *label, const char *pass) {
    char tmp[] = "/tmp/ghost_bnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 16384, 2048, pass), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount_key(&fs, tmp, pass), 0);

    const int M = 300; const size_t SZ = 4000;
    char *data = malloc(SZ); memset(data, 'x', SZ);
    char name[64];
    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < M; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        gh_fs_create(&fs, name, 0644);
        CHECK_EQ(gh_fs_write(&fs, name, data, SZ, 0), (ssize_t)SZ);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* dwukrotny odczyt wszystkich (druga runda = cieply cache) */
    char *rb = malloc(SZ);
    for (int i = 0; i < M; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_read(&fs, name, rb, SZ, 0), (ssize_t)SZ);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    for (int i = 0; i < M; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_read(&fs, name, rb, SZ, 0), (ssize_t)SZ);
        CHECK_EQ(memcmp(rb, data, SZ), 0);   /* poprawnosc */
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);
    printf("  [%s] create+write %d plikow: %.3fs | odczyt zimny: %.3fs | odczyt cieply: %.3fs\n",
           label, M, secs(t0, t1), secs(t1, t2), secs(t2, t3));
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    free(data); free(rb);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_bench(void) {
    bench_one("jawny", NULL);
    bench_one("szyfrowany", "haslo-benchmark");
}

int main(void) {
    RUN_TEST(test_bench);
    return TEST_SUMMARY();
}
```

- [ ] **Step 2: Uruchom benchmark**

Run: `make clean && make test 2>&1 | grep -A4 'TEST test_bench'`
Expected: `test_bench` `0 failed`, wypisane czasy (odczyt ciepły powinien być ≤ zimnemu —
cache działa). Wartości informacyjne.

- [ ] **Step 3: Stres współbieżności z cache (regresja D)**

Run: `make clean && make test && make cli fuse && ./tests/integration.sh`
Expected: wszystkie `OK:` (w tym `OK: 8 rownoleglych pisarzy...`, `OK: fsck czysty po
stresie wspolbieznosci`) i `WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY` — dowód, że cache pod
współbieżnością nie psuje danych. (Sekcja 15 z D testuje współbieżność na żywym mount z
włączonym cache.) Posprzątaj mounty.

- [ ] **Step 4: Pełny ASan**

Run: `make test-asan`
Expected: wszystkie `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add tests/bench.c
git commit -m "test: benchmark wydajnosci (create/write/read, jawny i szyfrowany; cieply vs zimny cache)"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec F:** hinty alokacji bloków/i-węzłów (Task 1) ✓; cache bloków write-through
+ cykl życia + thread-safety (Task 2) ✓; benchmark + regresja współbieżności (Task 3) ✓.
**Placeholdery:** brak; pełny kod alloc/inode/cache.
**Spójność:** `hint_block`/`hint_inode`/`cache` w `gh_dev`; `struct gh_bcache`/`gh_bentry`/
`GH_BCACHE_SLOTS`; `gh_bcache_create/destroy`; `gh_disk_read/write` integrują hook+szyfr+
cache; `-lpthread` w LDLIBS. Spójne między zadaniami.
**Ryzyka:** (1) cache pod współbieżnością — mutex chroni wyścig odczyt-vs-odczyt, D-rwlock
serializuje odczyt-vs-zapis (test integracyjny + TSan to bramka); (2) hinty z zawijaniem
nie pomijają wolnych zasobów (test masowy); (3) cache tylko przez `gh_fs_mount` — surowy
`gh_dev` w testach niskiego poziomu ma `cache=NULL` (bez zmian zachowania); (4) hook
`fail_after` przed zapisem → cache nieaktualizowany przy nieudanym zapisie (poprawne).
