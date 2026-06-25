# ghostfs pod-projekt K — katalogi haszowane: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. dir.c jest load-bearing (każda ścieżka) — bramka: zielone testy + ASan + integracja.

**Goal:** Haszowana tablica katalogu (open addressing) → `lookup`/`add`/`remove` O(1) średnio; koniec O(n²) zapełniania. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-K-hashdir-design.md`.

**Tech Stack:** C11, OpenSSL, pthreads, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- API dir stabilne (add/lookup/remove/is_empty/iterate) + nowa `gh_dir_set_ino`. `.`/`..` przez `gh_dir_add`. Wszystkie testy A–J `0 failed`.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/ghostfs.h` | `struct gh_dirhdr`, `GH_DIRHDR_MAGIC`, `GH_DIR_INIT_SLOTS`, `GH_DIR_TOMB`, `GH_SB_HASHDIR` |
| `src/dir.h` | `gh_dir_set_ino` |
| `src/dir.c` | przepisany na haszowanie |
| `src/super.c` | root `.`/`..` przez `gh_dir_add`; flaga `GH_SB_HASHDIR` |
| `src/fs.c` | `make_node` `.`/`..` przez `gh_dir_add`; rename przez `gh_dir_set_ino` (usuń update_dotdot/set_entry_ino) |
| `tests/test_dir.c` | testy haszowe (kolizje, wzrost, skala) |
| `tests/test_bench.c` | benchmark wielu plików w katalogu |

---

## Task 1: Katalogi haszowane (dir.c + integracja)

**Files:** Modify: `src/ghostfs.h`, `src/dir.h`, `src/dir.c`, `src/super.c`, `src/fs.c`, `tests/test_dir.c`

- [ ] **Step 1: `src/ghostfs.h` — nagłówek katalogu + stałe**

Przy `#define`: `#define GH_DIR_INIT_SLOTS 16`, `#define GH_DIR_TOMB 0xFFFFu`,
`#define GH_SB_HASHDIR 0x4u`. Przed `#endif`:

```c
#define GH_DIRHDR_MAGIC 0x4753484448495231ULL   /* "GSHDHIR1" */
struct gh_dirhdr {            /* w slocie 0 pliku katalogu, dopelniony do rozmiaru dirent */
    uint64_t magic;
    uint32_t used;            /* zajete + tombstony */
    uint32_t nslots;
};
```

- [ ] **Step 2: `src/dir.h` — `gh_dir_set_ino`**

```c
int gh_dir_set_ino(struct gh_dev*, const struct gh_superblock*,
                   uint64_t dir_ino, const char *name, uint64_t new_ino);
```

- [ ] **Step 3: Przepisz `src/dir.c`** — zastąp całą zawartość (zachowaj `gh_path_split`/
`gh_path_resolve` BEZ ZMIAN — przeklej istniejące):

```c
#include "dir.h"
#include "csum.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#define DENT_SZ ((uint64_t)sizeof(struct gh_dirent))

static int rd_hdr(struct gh_dev *dev, const struct gh_superblock *sb,
                  struct gh_inode *dir, struct gh_dirhdr *h) {
    uint8_t buf[sizeof(struct gh_dirent)];
    ssize_t r = gh_inode_pread(dev, sb, dir, buf, DENT_SZ, 0);
    if (r < 0) return (int)r;
    if (r != (ssize_t)DENT_SZ) return -EIO;
    memcpy(h, buf, sizeof(*h));
    return 0;
}
static int wr_hdr(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t dir_ino, struct gh_inode *dir, const struct gh_dirhdr *h) {
    uint8_t buf[sizeof(struct gh_dirent)]; memset(buf, 0, sizeof(buf));
    memcpy(buf, h, sizeof(*h));
    return gh_inode_pwrite(dev, sb, dir_ino, dir, buf, DENT_SZ, 0) == (ssize_t)DENT_SZ ? 0 : -EIO;
}
static int rd_slot(struct gh_dev *dev, const struct gh_superblock *sb,
                   struct gh_inode *dir, uint64_t slot, struct gh_dirent *de) {
    ssize_t r = gh_inode_pread(dev, sb, dir, de, DENT_SZ, slot * DENT_SZ);
    if (r < 0) return (int)r;
    if (r != (ssize_t)DENT_SZ) return -EIO;
    return 0;
}
static int wr_slot(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t dir_ino, struct gh_inode *dir, uint64_t slot, const struct gh_dirent *de) {
    return gh_inode_pwrite(dev, sb, dir_ino, dir, de, DENT_SZ, slot * DENT_SZ) == (ssize_t)DENT_SZ ? 0 : -EIO;
}
static int slot_empty(const struct gh_dirent *de) { return de->ino == 0 && de->name_len != GH_DIR_TOMB; }
static int slot_tomb(const struct gh_dirent *de)  { return de->ino == 0 && de->name_len == GH_DIR_TOMB; }

static int dir_init(struct gh_dev *dev, const struct gh_superblock *sb,
                    uint64_t dir_ino, struct gh_inode *dir) {
    struct gh_dirhdr h = { GH_DIRHDR_MAGIC, 0, GH_DIR_INIT_SLOTS };
    int r = wr_hdr(dev, sb, dir_ino, dir, &h); if (r) return r;
    struct gh_dirent empty; memset(&empty, 0, sizeof(empty));
    for (uint32_t i = 1; i <= GH_DIR_INIT_SLOTS; i++) {
        r = wr_slot(dev, sb, dir_ino, dir, i, &empty); if (r) return r;
    }
    return 0;
}

static int dir_grow(struct gh_dev *dev, const struct gh_superblock *sb,
                    uint64_t dir_ino, struct gh_inode *dir) {
    struct gh_dirhdr h; int r = rd_hdr(dev, sb, dir, &h); if (r) return r;
    uint32_t old = h.nslots;
    struct gh_dirent *ents = malloc((size_t)old * sizeof(struct gh_dirent));
    if (!ents) return -ENOMEM;
    uint32_t cnt = 0;
    for (uint32_t s = 1; s <= old; s++) {
        struct gh_dirent de; r = rd_slot(dev, sb, dir, s, &de);
        if (r) { free(ents); return r; }
        if (de.ino != 0) ents[cnt++] = de;
    }
    uint32_t newn = old * 2;
    struct gh_dirhdr nh = { GH_DIRHDR_MAGIC, cnt, newn };
    r = wr_hdr(dev, sb, dir_ino, dir, &nh); if (r) { free(ents); return r; }
    struct gh_dirent empty; memset(&empty, 0, sizeof(empty));
    for (uint32_t i = 1; i <= newn; i++) {
        r = wr_slot(dev, sb, dir_ino, dir, i, &empty); if (r) { free(ents); return r; }
    }
    for (uint32_t e = 0; e < cnt; e++) {
        uint64_t hh = gh_crc32(ents[e].name, ents[e].name_len) % newn;
        for (uint32_t p = 0; p < newn; p++) {
            uint64_t fs = 1 + (hh + p) % newn;
            struct gh_dirent de; r = rd_slot(dev, sb, dir, fs, &de); if (r) { free(ents); return r; }
            if (slot_empty(&de)) { r = wr_slot(dev, sb, dir_ino, dir, fs, &ents[e]); if (r) { free(ents); return r; } break; }
        }
    }
    free(ents);
    return 0;
}

int gh_dir_lookup(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t dir_ino, const char *name, uint64_t *out_ino) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return -ENOENT;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -ENOENT;
    size_t nl = strlen(name); if (nl > GH_NAME_MAX) return -ENAMETOOLONG;
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de)) return -ENOENT;
        if (de.ino != 0 && de.name_len == nl && memcmp(de.name, name, nl) == 0) { *out_ino = de.ino; return 0; }
    }
    return -ENOENT;
}

int gh_dir_add(struct gh_dev *dev, const struct gh_superblock *sb,
               uint64_t dir_ino, const char *name, uint64_t ino) {
    size_t nl = strlen(name); if (nl > GH_NAME_MAX) return -ENAMETOOLONG;
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) {
        r = dir_init(dev, sb, dir_ino, &dir); if (r) return r;
        r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    }
    uint64_t ex; if (gh_dir_lookup(dev, sb, dir_ino, name, &ex) == 0) return -EEXIST;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if ((uint64_t)(h.used + 1) * 4 > (uint64_t)h.nslots * 3) {
        r = dir_grow(dev, sb, dir_ino, &dir); if (r) return r;
        r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
        r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    }
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de) || slot_tomb(&de)) {
            int was_empty = slot_empty(&de);
            struct gh_dirent ne; memset(&ne, 0, sizeof(ne));
            ne.ino = ino; ne.name_len = (uint16_t)nl; memcpy(ne.name, name, nl);
            r = wr_slot(dev, sb, dir_ino, &dir, fs, &ne); if (r) return r;
            if (was_empty) { h.used++; r = wr_hdr(dev, sb, dir_ino, &dir, &h); if (r) return r; }
            return 0;
        }
    }
    return -ENOSPC;
}

int gh_dir_remove(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t dir_ino, const char *name) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return -ENOENT;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -ENOENT;
    size_t nl = strlen(name);
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de)) return -ENOENT;
        if (de.ino != 0 && de.name_len == nl && memcmp(de.name, name, nl) == 0) {
            struct gh_dirent tomb; memset(&tomb, 0, sizeof(tomb)); tomb.name_len = GH_DIR_TOMB;
            return wr_slot(dev, sb, dir_ino, &dir, fs, &tomb);
        }
    }
    return -ENOENT;
}

int gh_dir_set_ino(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t dir_ino, const char *name, uint64_t new_ino) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return -ENOENT;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -ENOENT;
    size_t nl = strlen(name);
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de)) return -ENOENT;
        if (de.ino != 0 && de.name_len == nl && memcmp(de.name, name, nl) == 0) {
            de.ino = new_ino;
            return wr_slot(dev, sb, dir_ino, &dir, fs, &de);
        }
    }
    return -ENOENT;
}

int gh_dir_is_empty(struct gh_dev *dev, const struct gh_superblock *sb,
                    uint64_t dir_ino, int *empty) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    *empty = 1;
    if (dir.size < DENT_SZ * 2) return 0;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC) return 0;
    for (uint32_t s = 1; s <= h.nslots; s++) {
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, s, &de); if (r) return r;
        if (de.ino != 0 && !(de.name_len == 1 && de.name[0] == '.')
            && !(de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.')) { *empty = 0; return 0; }
    }
    return 0;
}

int gh_dir_iterate(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t dir_ino, gh_dir_iter_fn cb, void *ctx) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return 0;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC) return 0;
    for (uint32_t s = 1; s <= h.nslots; s++) {
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, s, &de); if (r) return r;
        if (de.ino != 0) { int c = cb(&de, ctx); if (c) return c; }
    }
    return 0;
}

/* gh_path_split i gh_path_resolve — PRZEKLEJ z dotychczasowego dir.c bez zmian */
```

(WAŻNE: skopiuj istniejące `gh_path_split` i `gh_path_resolve` z poprzedniej wersji dir.c —
nie zmieniają się; `gh_path_resolve` używa `gh_dir_lookup`, więc działa haszowo.)

- [ ] **Step 4: `src/super.c` — root `.`/`..` przez `gh_dir_add` + flaga**

Dodaj `#include "dir.h"`. W `gh_format_enc`, ustaw `sb.flags |= GH_SB_HASHDIR;` (obok innych
flag). Zastąp fragment tworzący wpisy `.`/`..` roota (pętla `gh_inode_pwrite` z `names[2]`)
wywołaniami:

```c
    if ((r = gh_dir_add(&dev, &sb, GH_ROOT_INO, ".", GH_ROOT_INO))) { gh_dev_close(&dev); return r; }
    if ((r = gh_dir_add(&dev, &sb, GH_ROOT_INO, "..", GH_ROOT_INO))) { gh_dev_close(&dev); return r; }
```

(Root i-węzeł musi być wcześniej zapisany jako typ DIR o size 0; `gh_dir_add` zainicjalizuje
tablicę. Zachowaj `nlink=2` roota — zapisz i-węzeł root przed `gh_dir_add`, a `gh_dir_add`
modyfikuje tylko size/mtime, nie nlink.)

- [ ] **Step 5: `src/fs.c` — make_node `.`/`..` + rename przez `gh_dir_set_ino`**

W `make_node`, dla `type == GH_DIR`, zastąp pętlę `.`/`..` (bezpośredni `gh_inode_pwrite`):

```c
    if (type == GH_DIR) {
        int e = gh_dir_add(&fs->dev, &fs->sb, ino, ".", ino);
        if (e) { gh_inode_free(&fs->dev, &fs->sb, ino); return e; }
        e = gh_dir_add(&fs->dev, &fs->sb, ino, "..", pino);
        if (e) { gh_inode_free(&fs->dev, &fs->sb, ino); return e; }
        gh_inode_read(&fs->dev, &fs->sb, ino, &n);   /* re-read: size zmienione */
        n.mode = mode; n.nlink = 2;
    }
```

(Upewnij się, że dla pliku `n.mode = mode` jest ustawiane jak dotąd; powyższy blok dotyczy
tylko katalogu. `gh_inode_write` na końcu zapisuje `n`.)

Usuń statyczne `update_dotdot` i `set_entry_ino`. W `gh_fs_rename2`/`exchange_locked`/sekcji
rename zamień:
- `update_dotdot(&fs->dev, &fs->sb, sino, &sd, npino)` → `gh_dir_set_ino(&fs->dev, &fs->sb, sino, "..", npino)` (usuń odczyt `sd` jeśli zbędny),
- `update_dotdot(... tino ... opino)` → `gh_dir_set_ino(&fs->dev, &fs->sb, tino, "..", opino)`,
- `set_entry_ino(&fs->dev, &fs->sb, opino, on, tino)` → `gh_dir_set_ino(&fs->dev, &fs->sb, opino, on, tino)`,
- `set_entry_ino(... npino, nn, sino)` → `gh_dir_set_ino(&fs->dev, &fs->sb, npino, nn, sino)`.

- [ ] **Step 6: Zaktualizuj `tests/test_dir.c`** na model haszowy

Istniejące testy add/lookup/remove/nested/path_split powinny przejść (API stabilne). USUŃ
asercje zakładające layout liniowy (np. konkretne offsety/„prefix/tombstone" zależne od
kolejności). Dodaj testy haszowe: (a) kolizje — wstaw nazwy o tym samym `crc%INIT_SLOTS`,
wszystkie znajdowane; (b) tombston — add, remove, add tej samej nazwy → OK; (c) wzrost —
dodaj 20 wpisów (>16*3/4) → rehash, wszystkie nadal `lookup`-owalne, `is_empty`/`iterate`
poprawne; (d) `gh_dir_set_ino` zmienia ino i `lookup` zwraca nowe.

- [ ] **Step 7: Build + testy + ASan**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed` (test_dir haszowy; regresja A–J: `.`/`..`, path resolve,
rename z `..`, EXCHANGE, fsck==0). Brak raportów ASan (zwłaszcza brak wycieków `malloc` w
`dir_grow`).

- [ ] **Step 8: Integracja FUSE + urządzenie blokowe**

Run: `make cli fuse && ./tests/integration.sh && ./tests/integration_blockdev.sh`
Expected: WSZYSTKIE PRZESZŁY (`ls`, `mkdir -p a/b/c`, `mv`, `rm -r`, recover po kill-9,
fsck czysty, współbieżność, szyfrowanie; urządzenie blokowe). Posprzątaj mounty.

- [ ] **Step 9: Commit**

```bash
git add src/ghostfs.h src/dir.h src/dir.c src/super.c src/fs.c tests/test_dir.c
git commit -m "feat: katalogi haszowane (open addressing + wzrost) — O(1) lookup/add"
```

---

## Task 2: Skala + benchmark + regresja

**Files:** Modify: `tests/test_dir.c` (skala), `tests/test_bench.c`

- [ ] **Step 1: Test skali w `tests/test_dir.c`** (+ RUN_TEST):

```c
static void test_dir_scale(void) {
    char tmp[] = "/tmp/ghost_dscXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 8192, 2048), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);
    /* 1000 wpisow */
    char nm[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "plik%d", i);
        uint64_t fino; CHECK_EQ(gh_inode_alloc(&dev, &sb, GH_FILE, &fino), 0);
        CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, nm, fino), 0);
    }
    /* wszystkie znajdowane */
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "plik%d", i);
        uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, nm, &got), 0);
    }
    /* usun parzyste; nieparzyste nadal znajdowane, parzyste -ENOENT */
    for (int i = 0; i < 1000; i += 2) { snprintf(nm, sizeof(nm), "plik%d", i); CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, nm), 0); }
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "plik%d", i);
        uint64_t got; int r = gh_dir_lookup(&dev, &sb, GH_ROOT_INO, nm, &got);
        CHECK_EQ(r, (i % 2 == 0) ? -ENOENT : 0);
    }
    gh_dev_close(&dev); unlink(tmp);
}
```

(Dodaj `RUN_TEST(test_dir_scale);`. Test szybki = dowód braku O(n²).)

- [ ] **Step 2: Benchmark wielu plików w katalogu (`tests/test_bench.c`)**

Bench już tworzy M plików w jednym katalogu (`/fN`) — zmierz czas. Z katalogami haszowymi
tworzenie M plików nie jest O(M²). Wypisz czas; oczekiwany wyraźny spadek względem
liniowego. (Asercje poprawności bez zmian.)

- [ ] **Step 3: Pełna regresja**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed`; czasy bench wypisane (tworzenie plików szybsze).

- [ ] **Step 4: Commit**

```bash
git add tests/test_dir.c tests/test_bench.c
git commit -m "test: skala katalogow haszowanych (1000 wpisow) + benchmark"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec K:** nagłówek+stałe (Task 1) ✓; haszowe add/lookup/remove/is_empty/iterate +
set_ino + wzrost (Task 1) ✓; `.`/`..` przez gh_dir_add (super/fs) ✓; rename przez set_ino ✓;
skala+benchmark (Task 2) ✓.
**Placeholdery:** brak; pełny dir.c.
**Spójność:** `gh_dirhdr`/`GH_DIRHDR_MAGIC`/`GH_DIR_INIT_SLOTS`/`GH_DIR_TOMB` (ghostfs.h);
`gh_dir_set_ino` (dir.h/.c, fs.c); nagłówek w slocie 0, wpisy w slotach 1..nslots; hash =
gh_crc32; sondowanie liniowe; wzrost ×2 przy load 0,75. `.`/`..` jako zwykłe wpisy.
**Ryzyka:** (1) dir.c load-bearing → pełna regresja A–J + integracja jako bramka; (2) wzrost
atomowy (przez txn/group commit) i bez wycieku malloc (free na każdej ścieżce); (3) `.`/`..`
przez gh_dir_add zamiast offsetów (super.c/fs.c); (4) rename `..`/EXCHANGE przez set_ino; (5)
tombston: name_len==0xFFFF sentinel; sondowanie zatrzymuje się na pustym, nie na tombstonie;
(6) korupcja bloku katalogu → -EIO propagowane (J).
```
