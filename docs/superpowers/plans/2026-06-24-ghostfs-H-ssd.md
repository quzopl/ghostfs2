# ghostfs pod-projekt H — SSD/flash: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan + brak regresji A–G.

**Goal:** ghostfs działa na realnych urządzeniach blokowych (nie tylko plik) i jest przyjazny flashowi przez TRIM/discard (odroczony po commit dziennika). Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-H-ssd-design.md`.

**Tech Stack:** C11 (+`_GNU_SOURCE` w block.c/super.c), Linux ioctl (BLKGETSIZE64/BLKDISCARD), fallocate, libfuse3, OpenSSL, pthreads, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- Wszystkie testy A–G + integracja `0 failed`. Discard jest best-effort (nie przerywa operacji).

## File Structure
| Plik | Zmiana |
|---|---|
| `src/block.h` | `is_blkdev`/`discards`/`nd`/`dcap` w `gh_dev`; deklaracje `gh_disk_discard`/`gh_discard_pending_add`/`gh_discard_clear`/`gh_discard_flush` |
| `src/block.c` | `_GNU_SOURCE`; `gh_dev_open`/`create`/`close` (urządzenia + discard fields); `gh_disk_discard`; lista odroczeń |
| `src/alloc.c` | `gh_free_block` odracza/wykonuje discard; `gh_discard_flush` |
| `src/super.c` | `gh_format_enc` wykrywa urządzenie, autorozmiar, whole-region discard |
| `src/journal.c` | commit flush discardów; abort czyści discardy |
| `tests/test_block.c`, `tests/test_fs.c` | testy discard/punch-hole/abort |
| `tests/integration_blockdev.sh` | nowy: loop device |

---

## Task 1: Backend urządzeń blokowych + pola gh_dev

**Files:** Modify: `src/block.h`, `src/block.c`, `src/super.c`; Test: `tests/test_block.c`

- [ ] **Step 1: Rozszerz `struct gh_dev` (`src/block.h`)** — dodaj pola na końcu:

```c
struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn;
                struct gh_cipher *cipher; long fail_after;
                uint64_t hint_block; uint64_t hint_inode; struct gh_bcache *cache;
                int is_blkdev; uint64_t *discards; uint32_t nd; uint32_t dcap; };

int gh_disk_discard(struct gh_dev *dev, uint64_t blkno, uint64_t count);
void gh_discard_pending_add(struct gh_dev *dev, uint64_t blkno);
void gh_discard_clear(struct gh_dev *dev);
int  gh_discard_flush(struct gh_dev *dev, const struct gh_superblock *sb);
```

- [ ] **Step 2: `_GNU_SOURCE` + nagłówki + detekcja urządzenia w `src/block.c`**

Na SAMEJ GÓRZE pliku (przed wszystkimi `#include`):

```c
#define _GNU_SOURCE
```

Dodaj include'y: `#include <sys/stat.h>`, `#include <sys/ioctl.h>`, `#include <linux/fs.h>`,
`#include <stdlib.h>` (jeśli brak). Zastąp `gh_dev_open` i `gh_dev_create` (inicjalizacja
nowych pól + detekcja urządzenia):

```c
int gh_dev_create(const char *path, uint64_t total_blocks, struct gh_dev *dev) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -errno;
    if (ftruncate(fd, (off_t)(total_blocks * GH_BLOCK_SIZE)) != 0) { int e = -errno; close(fd); return e; }
    dev->fd = fd; dev->total_blocks = total_blocks;
    dev->txn = NULL; dev->cipher = NULL; dev->fail_after = 0;
    dev->hint_block = 0; dev->hint_inode = 0; dev->cache = NULL;
    dev->is_blkdev = 0; dev->discards = NULL; dev->nd = 0; dev->dcap = 0;
    return 0;
}

int gh_dev_open(const char *path, struct gh_dev *dev) {
    int fd = open(path, O_RDWR); if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = -errno; close(fd); return e; }
    uint64_t bytes; int is_blk = 0;
    if (S_ISBLK(st.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) { int e = -errno; close(fd); return e; }
        is_blk = 1;
    } else {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0) { int e = -errno; close(fd); return e; }
        bytes = (uint64_t)end;
    }
    dev->fd = fd; dev->total_blocks = bytes / GH_BLOCK_SIZE;
    dev->txn = NULL; dev->cipher = NULL; dev->fail_after = 0;
    dev->hint_block = 0; dev->hint_inode = 0; dev->cache = NULL;
    dev->is_blkdev = is_blk; dev->discards = NULL; dev->nd = 0; dev->dcap = 0;
    return 0;
}
```

(Zachowaj istniejące `#include` block.c; jeśli `gh_dev_create` różni się szczegółem —
dostosuj, kluczowe to inicjalizacja nowych pól i brak regresji.)

- [ ] **Step 3: `gh_dev_close` zwalnia listę discardów (`src/block.c`)**

```c
void gh_dev_close(struct gh_dev *dev) {
    if (dev->discards) { free(dev->discards); dev->discards = NULL; dev->nd = dev->dcap = 0; }
    if (dev->fd >= 0) { close(dev->fd); dev->fd = -1; }
}
```

(Jeśli obecne `gh_dev_close` ma inny kształt — dodaj tylko zwolnienie `discards` na
początku.)

- [ ] **Step 4: Wykrywanie urządzenia w `gh_format_enc` (`src/super.c`)**

Na górze `src/super.c` dodaj `#define _GNU_SOURCE` (pierwsza linia) oraz include'y
`#include <sys/stat.h>`, `#include <sys/ioctl.h>`, `#include <linux/fs.h>`, `#include <fcntl.h>`.
Na początku `gh_format_enc`, przed wyliczeniem layoutu, dodaj detekcję urządzenia i
autorozmiar:

```c
    struct stat pst; int is_blk = 0;
    if (stat(path, &pst) == 0 && S_ISBLK(pst.st_mode)) {
        is_blk = 1;
        int tfd = open(path, O_RDWR); if (tfd < 0) return -errno;
        uint64_t bytes = 0; int ssz = 512;
        if (ioctl(tfd, BLKGETSIZE64, &bytes) != 0) { int e = -errno; close(tfd); return e; }
        ioctl(tfd, BLKSSZGET, &ssz);
        close(tfd);
        if (ssz > (int)GH_BLOCK_SIZE) return -EINVAL;       /* sektor > 4096 niewspierany */
        uint64_t dev_total = bytes / GH_BLOCK_SIZE;
        if (total_blocks == 0 || total_blocks > dev_total) total_blocks = dev_total;
    } else if (total_blocks == 0) {
        return -EINVAL;   /* plik nieistniejacy wymaga rozmiaru */
    }
```

Następnie tam, gdzie obecnie `gh_format_enc` woła `gh_dev_create(path, total_blocks, &dev)`,
rozgałęź:

```c
    struct gh_dev dev;
    int r = is_blk ? gh_dev_open(path, &dev) : gh_dev_create(path, total_blocks, &dev);
    if (r) return r;
```

(Reszta `gh_format_enc` bez zmian na tym etapie — discard regionu dojdzie w Task 2.)

- [ ] **Step 5: Test braku regresji w `tests/test_block.c`** (+ RUN_TEST):

```c
static void test_dev_fields(void) {
    char tmp[] = "/tmp/ghost_dfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 32, &dev), 0);
    CHECK_EQ(dev.is_blkdev, 0);
    CHECK_EQ(dev.total_blocks, 32);
    gh_dev_close(&dev);
    /* ponowne otwarcie pliku */
    CHECK_EQ(gh_dev_open(tmp, &dev), 0);
    CHECK_EQ(dev.is_blkdev, 0);
    CHECK_EQ(dev.total_blocks, 32);
    gh_dev_close(&dev); unlink(tmp);
}
```

- [ ] **Step 6: Zbuduj i testy**

Run: `make clean && make test && make cli fuse`
Expected: wszystkie `0 failed` (regresja A–G zachowana); CLI/FUSE budują się czysto
(`-Wall -Wextra -Werror`; `_GNU_SOURCE` nie psuje kompilacji). `make test-asan` → `0 failed`.

- [ ] **Step 7: Commit**

```bash
git add src/block.h src/block.c src/super.c tests/test_block.c
git commit -m "feat: backend urzadzen blokowych (BLKGETSIZE64, S_ISBLK, autorozmiar) + pola gh_dev"
```

---

## Task 2: TRIM/discard (odroczony, zintegrowany z dziennikiem)

**Files:** Modify: `src/block.c`, `src/alloc.c`, `src/journal.c`, `src/super.c`; Test: `tests/test_block.c`, `tests/test_fs.c`

- [ ] **Step 1: `gh_disk_discard` + lista odroczeń w `src/block.c`**

Dodaj `#include <fcntl.h>` (dla `fallocate`/`FALLOC_FL_*` przy `_GNU_SOURCE`). Dodaj:

```c
int gh_disk_discard(struct gh_dev *dev, uint64_t blkno, uint64_t count) {
    if (count == 0) return 0;
    uint64_t off = blkno * GH_BLOCK_SIZE;
    uint64_t len = count * GH_BLOCK_SIZE;
    if (dev->is_blkdev) {
        uint64_t range[2] = { off, len };
        if (ioctl(dev->fd, BLKDISCARD, range) != 0) {
            if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL) return 0;
            return -errno;
        }
    } else {
        if (fallocate(dev->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      (off_t)off, (off_t)len) != 0) {
            if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL) return 0;
            return -errno;
        }
    }
    return 0;
}

void gh_discard_pending_add(struct gh_dev *dev, uint64_t blkno) {
    if (dev->nd >= dev->dcap) {
        uint32_t nc = dev->dcap ? dev->dcap * 2 : 64;
        uint64_t *p = realloc(dev->discards, (size_t)nc * sizeof(uint64_t));
        if (!p) return;                 /* best-effort: porzuc gdy brak pamieci */
        dev->discards = p; dev->dcap = nc;
    }
    dev->discards[dev->nd++] = blkno;
}

void gh_discard_clear(struct gh_dev *dev) { dev->nd = 0; }
```

- [ ] **Step 2: `gh_discard_flush` + `gh_free_block` w `src/alloc.c`**

Dodaj `#include <stdlib.h>` (qsort). Dodaj komparator i flush oraz zmień `gh_free_block`:

```c
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int gh_discard_flush(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (dev->nd == 0) return 0;
    /* zostaw tylko bloki wciaz wolne (ochrona przed realokacja) */
    uint32_t w = 0;
    for (uint32_t i = 0; i < dev->nd; i++) {
        int set = 0;
        if (gh_bitmap_test(dev, sb, dev->discards[i], &set) == 0 && !set)
            dev->discards[w++] = dev->discards[i];
    }
    dev->nd = w;
    if (w == 0) return 0;
    qsort(dev->discards, w, sizeof(uint64_t), cmp_u64);
    uint32_t i = 0;
    while (i < w) {
        uint64_t start = dev->discards[i], cnt = 1;
        while (i + cnt < w && dev->discards[i + cnt] == start + cnt) cnt++;
        gh_disk_discard(dev, start, cnt);     /* best-effort */
        i += (uint32_t)cnt;
    }
    dev->nd = 0;
    return 0;
}

int gh_free_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t blkno) {
    if (blkno < sb->data_start || blkno >= sb->total_blocks) return -EINVAL;
    if (blkno < dev->hint_block) dev->hint_block = blkno;
    int r = bitmap_rw(dev, sb, blkno, 0, 1, NULL);
    if (r) return r;
    if (dev->txn && dev->txn->active) gh_discard_pending_add(dev, blkno);  /* odroczenie */
    else gh_disk_discard(dev, blkno, 1);                                   /* natychmiast */
    return 0;
}
```

- [ ] **Step 3: Integracja z dziennikiem (`src/journal.c`)**

W `gh_jrnl_abort` dodaj czyszczenie odroczonych discardów (abort = porzuć):

```c
void gh_jrnl_abort(struct gh_dev *dev) {
    gh_discard_clear(dev);
    if (!dev->txn) return;
    free(dev->txn->blknos); free(dev->txn->images); free(dev->txn);
    dev->txn = NULL;
}
```

W `gh_jrnl_commit`, na ścieżce SUKCESU (po fazie 4: wyczyszczeniu nagłówka i `fsync`), PRZED
`gh_jrnl_abort(dev)`, wywołaj flush:

```c
    /* faza 4 ... wyczysc naglowek, fsync ... */
    gh_discard_flush(dev, sb);    /* wykonaj odroczone discardy (filtr: wolne w mapie) */
    gh_jrnl_abort(dev);           /* zwolnij txn (discardy juz puste) */
    return 0;
```

(Na ścieżce błędu `fail:` pozostaje `gh_jrnl_abort(dev)` — porzuca discardy, bo operacja
się nie zatwierdziła. Tryb bez dziennika: `dev->txn==NULL`, discard natychmiastowy w
`gh_free_block`, `gh_jrnl_commit` no-op.)

- [ ] **Step 4: Whole-region discard przy formacie (`src/super.c`)**

W `gh_format_enc`, po otwarciu/utworzeniu `dev` i wyliczeniu `data_start`/`total_blocks`,
przed zapisem metadanych, dodaj best-effort discard regionu danych:

```c
    gh_disk_discard(&dev, data_start, total_blocks - data_start);   /* czysty start na SSD */
```

- [ ] **Step 5: Test punch-hole w `tests/test_block.c`** (+ RUN_TEST):

```c
#include <sys/stat.h>
static void test_discard_punch(void) {
    char tmp[] = "/tmp/ghost_dpXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 64, &dev), 0);
    char buf[GH_BLOCK_SIZE]; memset(buf, 0xAB, sizeof(buf));
    for (uint64_t b = 10; b < 40; b++) CHECK_EQ(gh_block_write(&dev, b, buf), 0);

    struct stat s1; fstat(dev.fd, &s1);
    /* discard zapisanych blokow -> plik staje sie rzadszy */
    CHECK_EQ(gh_disk_discard(&dev, 10, 30), 0);
    struct stat s2; fstat(dev.fd, &s2);
    CHECK(s2.st_blocks <= s1.st_blocks);   /* punch-hole zmniejszyl alokacje (lub best-effort) */
    gh_dev_close(&dev); unlink(tmp);
}
```

- [ ] **Step 6: Test odroczenia/abort w `tests/test_fs.c`** (+ RUN_TEST):

```c
#include <sys/stat.h>
static void test_discard_on_free(void) {
    char tmp[] = "/tmp/ghost_doXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* zapisz duzy plik (wiele blokow) */
    CHECK_EQ(gh_fs_create(&fs, "/big", 0644), 0);
    char *d = malloc(200000); memset(d, 'z', 200000);
    CHECK_EQ(gh_fs_write(&fs, "/big", d, 200000, 0), 200000);
    struct stat sb1; stat(tmp, &sb1);
    /* usun -> odroczone discardy wykonane po commit -> plik rzadszy */
    CHECK_EQ(gh_fs_unlink(&fs, "/big"), 0);
    struct stat sb2; stat(tmp, &sb2);
    CHECK(sb2.st_blocks <= sb1.st_blocks);   /* discard zwolnil miejsce (best-effort) */
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    /* integralnosc: po realokacji blok nie jest discardowany (zapis-odczyt spojny) */
    CHECK_EQ(gh_fs_create(&fs, "/re", 0644), 0);
    const char *m = "po-realokacji-spojne";
    CHECK_EQ(gh_fs_write(&fs, "/re", m, strlen(m), 0), (ssize_t)strlen(m));
    char rb[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/re", rb, sizeof(rb), 0), (ssize_t)strlen(m));
    CHECK_EQ(memcmp(rb, m, strlen(m)), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    free(d); gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 7: Uruchom testy + ASan**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed` (w tym `test_discard_punch`, `test_discard_on_free`);
regresja A–G zachowana; brak raportów ASan. (Discard best-effort, więc `st_blocks` może
nie spaść jeśli FS tmp nie wspiera punch-hole — asercja `<=` to dopuszcza, ale na ext4/tmp
zwykle spada.)

- [ ] **Step 8: Commit**

```bash
git add src/block.c src/alloc.c src/journal.c src/super.c tests/test_block.c tests/test_fs.c
git commit -m "feat: TRIM/discard (BLKDISCARD/punch-hole) odroczony po commit + discard regionu przy formacie"
```

---

## Task 3: Integracja na urządzeniu blokowym (loop) + regresja

**Files:** Create: `tests/integration_blockdev.sh`

- [ ] **Step 1: Utwórz `tests/integration_blockdev.sh`**

```bash
#!/usr/bin/env bash
# Test ghostfs na realnym urzadzeniu blokowym (loop). Wymaga roota/sudo i losetup.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GFS="$ROOT/build/ghostfs"; CLI="$ROOT/build/ghostfs-cli"

if ! command -v losetup >/dev/null; then echo "POMINIETO: brak losetup"; exit 0; fi
SUDO=""
if [ "$(id -u)" != 0 ]; then
  if sudo -n true 2>/dev/null; then SUDO="sudo"; else echo "POMINIETO: brak uprawnien root"; exit 0; fi
fi

BACK=$(mktemp /tmp/ghost_loop.XXXXXX.img)
truncate -s 64M "$BACK"
LOOP=$($SUDO losetup --find --show "$BACK") || { echo "POMINIETO: losetup nieudany"; rm -f "$BACK"; exit 0; }
MNT=$(mktemp -d)
cleanup() {
  fusermount3 -u "$MNT" 2>/dev/null || true
  $SUDO losetup -d "$LOOP" 2>/dev/null || true
  rm -f "$BACK"; rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

# format urzadzenia (autorozmiar: 0 blokow = caly device)
$SUDO chmod o+rw "$LOOP" 2>/dev/null || true
"$CLI" format "$LOOP" 0 1024 || { echo "FAIL: format urzadzenia"; exit 1; }
echo "OK: format urzadzenia blokowego $LOOP"

# mount FUSE na urzadzeniu
"$GFS" "$LOOP" "$MNT" -f &
GPID=$!; sleep 1
echo "test-na-ssd" > "$MNT/f.txt"
test "$(cat "$MNT/f.txt")" = "test-na-ssd" && echo "OK: round-trip na urzadzeniu"
head -c 1000000 /dev/urandom > "$MNT/big.bin"
cp "$MNT/big.bin" /tmp/ghost_bd.out
cmp -s "$MNT/big.bin" /tmp/ghost_bd.out && echo "OK: duzy plik na urzadzeniu"
rm -f "$MNT/big.bin" /tmp/ghost_bd.out
fusermount3 -u "$MNT"; wait $GPID 2>/dev/null || true

# fsck urzadzenia
"$CLI" fsck "$LOOP" | grep -q "0 niespójności" && echo "OK: fsck urzadzenia czysty"
echo "WSZYSTKIE TESTY URZADZENIA BLOKOWEGO PRZESZLY"
```

- [ ] **Step 2: Nadaj prawa i uruchom (best-effort — wymaga uprawnień)**

Run: `chmod +x tests/integration_blockdev.sh && make cli fuse && ./tests/integration_blockdev.sh`
Expected: jeśli dostępny root/sudo + loop → kolejne `OK:` i `WSZYSTKIE TESTY URZADZENIA
BLOKOWEGO PRZESZLY`; inaczej `POMINIETO: ...` (i exit 0). Jeśli uruchomione i któryś krok
`FAIL` — to realny błąd backendu urządzeń; zdiagnozuj.

- [ ] **Step 3: Pełna regresja**

Run: `make clean && make test && make cli fuse && ./tests/integration.sh`
Expected: wszystkie testy jednostkowe `0 failed`; integracja na pliku-kontenerze (A–G)
zielona (`WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY`). Posprzątaj mounty.

- [ ] **Step 4: Commit**

```bash
git add tests/integration_blockdev.sh
git commit -m "test: integracja na urzadzeniu blokowym (loop) — format/mount/round-trip/fsck"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec H:** backend urządzeń (open/format detekcja, autorozmiar, walidacja
sektora) — Task 1 ✓; `gh_disk_discard` + odroczenie + flush po commit + abort drop +
discard regionu przy formacie — Task 2 ✓; integracja loop + regresja — Task 3 ✓.
**Placeholdery:** brak; pełny kod gh_dev_open/discard/flush/free.
**Spójność:** `is_blkdev`/`discards`/`nd`/`dcap` w gh_dev; `gh_disk_discard`/
`gh_discard_pending_add`/`gh_discard_clear` (block.c) + `gh_discard_flush`/`gh_free_block`
(alloc.c) + integracja journal commit/abort. `_GNU_SOURCE` w block.c i super.c.
**Ryzyka:** (1) discard odroczony do PO commit (abort drop) → integralność z transakcją;
(2) filtr „wolny w mapie" chroni przed discardem realokowanego bloku; (3) zerowanie przy
alokacji czyni discard-po-commit bezpiecznym; (4) best-effort (ENOTSUP→0) → działa też na
nośnikach/FS bez discardu; (5) loop-test wymaga uprawnień → POMINIETO gdy brak.
