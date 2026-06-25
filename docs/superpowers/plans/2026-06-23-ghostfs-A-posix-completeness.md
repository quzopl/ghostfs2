# ghostfs pod-projekt A — kompletność POSIX: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Subagenci (na życzenie użytkownika):** zadania można delegować do subagentów. Tryb rekomendowany to subagent-driven-development: świeży subagent na zadanie + przegląd między zadaniami. Zadania 1→8 dotykają wspólnych plików (`src/fs.c`, `src/fs.h`, `src/inode.c`) i muszą iść **sekwencyjnie** (jeden subagent naraz, bramką jest zielony test). Zadania 9 (FUSE) i 10 (CLI) zależą od gotowego API rdzenia (1–8). Jeśli chcesz prawdziwej równoległości — uruchom subagentów w osobnych worktree (`superpowers:using-git-worktrees`) i scal po kolei; przy wspólnym `fs.c` i tak zalecam sekwencję, by uniknąć konfliktów.

**Goal:** Domknąć realne braki systemu plików ghostfs (truncate, utimens, rename, statfs, chmod/chown, flush/fsync, symlink, hardlink, xattr) w rdzeniu, FUSE i CLI — zgodnie ze spec `docs/superpowers/specs/2026-06-23-ghostfs-A-posix-completeness-design.md`.

**Architecture:** Cała logika w rdzeniu `libghostfs`; FUSE/CLI to cienkie nakładki. Nowe operacje powstają najpierw w rdzeniu z testem jednostkowym bez FUSE, potem są podpinane. Format on-disk zmienia się kompatybilnie wstecz (nowe pole `xattr_block` zajmuje dotychczasowy zerowany padding i-węzła — bez zmiany magic, bez reformatu).

**Tech Stack:** C11 (`-std=c11 -Wall -Wextra -Werror`), libfuse3, GNU Make, mini-harness `tests/test.h`, AddressSanitizer.

## Global Constraints

- Rozmiar i-węzła pozostaje **256 B** (nowe pole kosztem paddingu).
- Każde zadanie kończy się zielonym testem (`0 failed`), brakiem raportów ASan i commitem.
- Stałe i typy z `src/ghostfs.h` są wspólne; nie duplikuj definicji.
- Funkcje rdzenia zwracają `0`/`-errno` lub liczbę bajtów (`ssize_t`).

## File Structure

| Plik | Zmiana | Odpowiedzialność |
|---|---|---|
| `src/ghostfs.h` | modyfikacja | `GH_SYMLINK`, pole `xattr_block`, stałe `GH_XATTR_MAX`/`GH_SYMLINK_MAX`, `struct gh_statfs` |
| `src/inode.c/.h` | modyfikacja | `gh_inode_truncate`; `gh_inode_free` zwalnia `xattr_block` |
| `src/xattr.c/.h` | **nowy** | format bloku xattr (set/get/list/remove) |
| `src/fs.c/.h` | modyfikacja | nowe `gh_fs_*` (truncate/utimens/chmod/chown/statfs/sync/rename/symlink/readlink/link/*xattr); zmiana `nlink` w usuwaniu; `fsck` znaczy `xattr_block` |
| `src/fuse_main.c` | modyfikacja | realne handlery zamiast atrap |
| `src/cli.c` | modyfikacja | komendy `mv`/`truncate`/`ln`/`lns`/`chmod`/`stat`/`df` |
| `Makefile` | modyfikacja | dodanie `src/xattr.c` do `CORE` |
| `tests/test_inode.c` | modyfikacja | testy truncate |
| `tests/test_fs.c` | modyfikacja | testy utimens/chmod/chown/statfs/sync/rename |
| `tests/test_links_xattr.c` | **nowy** | testy symlink/hardlink/xattr |
| `tests/integration.sh` | modyfikacja | mv/truncate/df/chmod/ln -s/xattr |

---

## Task 1: Zmiany formatu on-disk (nagłówek wspólny)

**Files:**
- Modify: `src/ghostfs.h`

- [ ] **Step 1: Dodaj typ symlinka, pole `xattr_block`, stałe i `struct gh_statfs`**

W `src/ghostfs.h` zmień enum typów:

```c
enum gh_itype { GH_FREE = 0, GH_FILE = 1, GH_DIR = 2, GH_SYMLINK = 3 };
```

Dodaj stałe pod istniejącymi `#define` (np. po `GH_ROOT_INO`):

```c
#define GH_XATTR_MAX    GH_BLOCK_SIZE   /* limit laczny xattr na i-wezel */
#define GH_SYMLINK_MAX  GH_BLOCK_SIZE   /* limit dlugosci celu symlinka */
```

W `struct gh_inode` dodaj pole `xattr_block` po `double_indirect` i zmniejsz padding o 8 B:

```c
struct gh_inode {                 /* dokładnie GH_INODE_SIZE bajtów */
    uint16_t type;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t atime, mtime, ctime;
    uint64_t direct[GH_NDIRECT];
    uint64_t indirect;
    uint64_t double_indirect;
    uint64_t xattr_block;         /* 0 = brak xattr */
    uint8_t  pad[GH_INODE_SIZE - (2+2+4+4+4+8+8+8+8 + 8*GH_NDIRECT + 8 + 8 + 8)];
};
```

Dodaj strukturę wyniku statfs przed `#endif`:

```c
struct gh_statfs {
    uint32_t block_size;
    uint64_t total_blocks, free_blocks;
    uint64_t total_inodes, free_inodes;
    uint32_t name_max;
};
```

- [ ] **Step 2: Sprawdź, że rozmiar i-węzła to nadal 256 B (statyczna asercja)**

Dopisz w `src/ghostfs.h` przed `#endif`:

```c
_Static_assert(sizeof(struct gh_inode) == GH_INODE_SIZE, "i-wezel != 256B");
```

- [ ] **Step 3: Uruchom istniejące testy — mają nadal przechodzić (kompatybilność)**

Run: `make clean && make test`
Expected: wszystkie testy `0 failed` (zmiana paddingu nie rusza układu pól; stare kontenery dalej działają).

- [ ] **Step 4: Commit**

```bash
git add src/ghostfs.h
git commit -m "feat: pole xattr_block + typ symlink + gh_statfs (format kompatybilny)"
```

---

## Task 2: `gh_inode_truncate` (skracanie/wydłużanie treści)

**Files:**
- Modify: `src/inode.h`, `src/inode.c`
- Test: `tests/test_inode.c`

- [ ] **Step 1: Zadeklaruj `gh_inode_truncate` w `src/inode.h`**

Dodaj przy pozostałych deklaracjach:

```c
int gh_inode_truncate(struct gh_dev*, const struct gh_superblock*, uint64_t ino,
                      struct gh_inode *node, uint64_t new_size);
```

- [ ] **Step 2: Napisz failujący test w `tests/test_inode.c`**

Dodaj funkcję testową i zarejestruj ją w `main`:

```c
static void test_truncate(void) {
    char tmp[] = "/tmp/ghost_trXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    CHECK_EQ(gh_dev_open(tmp, &dev), 0); CHECK_EQ(gh_mount_sb(&dev, &sb), 0);

    uint64_t ino; CHECK_EQ(gh_inode_alloc(&dev, &sb, GH_FILE, &ino), 0);
    struct gh_inode node; CHECK_EQ(gh_inode_read(&dev, &sb, ino, &node), 0);

    /* zapisz 20 blokow (wejdzie w indirect) */
    size_t sz = 20 * GH_BLOCK_SIZE;
    char *src = malloc(sz);
    for (size_t i = 0; i < sz; i++) src[i] = (char)(i * 5 + 1);
    CHECK_EQ(gh_inode_pwrite(&dev, &sb, ino, &node, src, sz, 0), (ssize_t)sz);

    /* policz wolne bloki przed skroceniem */
    uint64_t free_before = 0;
    for (uint64_t b = sb.data_start; b < sb.total_blocks; b++) {
        int s = 0; gh_bitmap_test(&dev, &sb, b, &s); if (!s) free_before++;
    }

    /* skroc do 5000 bajtow (2 bloki, ogon czesciowy) */
    CHECK_EQ(gh_inode_truncate(&dev, &sb, ino, &node, 5000), 0);
    CHECK_EQ(node.size, 5000);

    /* zwolnilo bloki => wiecej wolnych niz przed */
    uint64_t free_after = 0;
    for (uint64_t b = sb.data_start; b < sb.total_blocks; b++) {
        int s = 0; gh_bitmap_test(&dev, &sb, b, &s); if (!s) free_after++;
    }
    CHECK(free_after > free_before);

    /* dane do 5000 niezmienione */
    char *chk = malloc(5000);
    CHECK_EQ(gh_inode_pread(&dev, &sb, &node, chk, 5000, 0), (ssize_t)5000);
    CHECK_EQ(memcmp(chk, src, 5000), 0);

    /* wydluzenie z powrotem: ogon po 5000 = zera (POSIX) */
    CHECK_EQ(gh_inode_truncate(&dev, &sb, ino, &node, 8000), 0);
    CHECK_EQ(node.size, 8000);
    char tail[3000];
    CHECK_EQ(gh_inode_pread(&dev, &sb, &node, tail, 3000, 5000), (ssize_t)3000);
    char zeros[3000]; memset(zeros, 0, sizeof(zeros));
    CHECK_EQ(memcmp(tail, zeros, 3000), 0);

    free(src); free(chk);
    gh_dev_close(&dev); unlink(tmp);
}
```

W `main` dodaj: `RUN_TEST(test_truncate);`

- [ ] **Step 3: Uruchom test — ma failować (brak implementacji)**

Run: `cc -std=c11 -g tests/test_inode.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c -o build/test_inode 2>&1 | head`
Expected: błąd linkera „undefined reference to `gh_inode_truncate`".

- [ ] **Step 4: Zaimplementuj `gh_inode_truncate` w `src/inode.c`**

Dodaj na końcu pliku (po `gh_inode_free`). Wykorzystuje istniejący statyczny `bmap`:

```c
/* zwolnij wszystkie liscie wskazywane przez blok posredni + sam blok */
static void free_indirect_full(struct gh_dev *dev, const struct gh_superblock *sb,
                               uint64_t blk) {
    uint64_t p[GH_PTRS_PER_BLK];
    if (gh_block_read(dev, blk, p) == 0)
        for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++)
            if (p[i]) gh_free_block(dev, sb, p[i]);
    gh_free_block(dev, sb, blk);
}

/* zwolnij liscie bloku posredniego od indeksu `from`, wyzeruj wskazniki */
static void free_indirect_from(struct gh_dev *dev, const struct gh_superblock *sb,
                               uint64_t blk, uint64_t from) {
    uint64_t p[GH_PTRS_PER_BLK];
    if (gh_block_read(dev, blk, p)) return;
    int dirty = 0;
    for (uint64_t i = from; i < GH_PTRS_PER_BLK; i++)
        if (p[i]) { gh_free_block(dev, sb, p[i]); p[i] = 0; dirty = 1; }
    if (dirty) gh_block_write(dev, blk, p);
}

int gh_inode_truncate(struct gh_dev *dev, const struct gh_superblock *sb,
                      uint64_t ino, struct gh_inode *node, uint64_t new_size) {
    uint64_t old_size = node->size;
    uint64_t new_nblk = (new_size + GH_BLOCK_SIZE - 1) / GH_BLOCK_SIZE;

    if (new_size < old_size) {
        /* bezpośrednie */
        for (uint64_t i = (new_nblk < GH_NDIRECT ? new_nblk : GH_NDIRECT);
             i < GH_NDIRECT; i++)
            if (node->direct[i]) { gh_free_block(dev, sb, node->direct[i]); node->direct[i] = 0; }

        /* pojedynczo pośrednie: zakres [NDIRECT, NDIRECT+PTRS) */
        if (node->indirect) {
            if (new_nblk <= GH_NDIRECT) {
                free_indirect_full(dev, sb, node->indirect); node->indirect = 0;
            } else if (new_nblk < GH_NDIRECT + GH_PTRS_PER_BLK) {
                free_indirect_from(dev, sb, node->indirect, new_nblk - GH_NDIRECT);
            }
        }

        /* podwójnie pośrednie */
        if (node->double_indirect) {
            uint64_t dstart = GH_NDIRECT + GH_PTRS_PER_BLK;
            if (new_nblk <= dstart) {
                uint64_t l1[GH_PTRS_PER_BLK];
                if (gh_block_read(dev, node->double_indirect, l1) == 0)
                    for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++)
                        if (l1[i]) free_indirect_full(dev, sb, l1[i]);
                gh_free_block(dev, sb, node->double_indirect);
                node->double_indirect = 0;
            } else {
                uint64_t rel = new_nblk - dstart;
                uint64_t i1_start = rel / GH_PTRS_PER_BLK;
                uint64_t i2_start = rel % GH_PTRS_PER_BLK;
                uint64_t l1[GH_PTRS_PER_BLK];
                if (gh_block_read(dev, node->double_indirect, l1) == 0) {
                    int l1dirty = 0;
                    for (uint64_t i1 = i1_start; i1 < GH_PTRS_PER_BLK; i1++) {
                        if (!l1[i1]) continue;
                        uint64_t from = (i1 == i1_start) ? i2_start : 0;
                        if (from == 0) { free_indirect_full(dev, sb, l1[i1]); l1[i1] = 0; l1dirty = 1; }
                        else free_indirect_from(dev, sb, l1[i1], from);
                    }
                    if (l1dirty) gh_block_write(dev, node->double_indirect, l1);
                }
            }
        }

        /* zeruj ogon ostatniego zachowanego bloku */
        uint64_t tail = new_size % GH_BLOCK_SIZE;
        if (tail != 0 && new_nblk > 0) {
            uint64_t phys;
            if (bmap(dev, sb, node, new_nblk - 1, 0, &phys) == 0 && phys) {
                uint8_t blk[GH_BLOCK_SIZE];
                if (gh_block_read(dev, phys, blk) == 0) {
                    memset(blk + tail, 0, GH_BLOCK_SIZE - tail);
                    gh_block_write(dev, phys, blk);
                }
            }
        }
    }

    node->size = new_size;
    node->mtime = node->ctime = (uint64_t)time(NULL);
    return gh_inode_write(dev, sb, ino, node);
}
```

- [ ] **Step 5: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_inode.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c -o build/test_inode && ./build/test_inode`
Expected: `0 failed`

- [ ] **Step 6: ASan**

Run: `cc -std=c11 -fsanitize=address,undefined -g tests/test_inode.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c -o build/test_inode_asan && ./build/test_inode_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 7: Commit**

```bash
git add src/inode.h src/inode.c tests/test_inode.c
git commit -m "feat: gh_inode_truncate (skracanie zwalnia bloki, wydluzanie=zera)"
```

---

## Task 3: API metadanych — `truncate`, `utimens`, `chmod`, `chown`

**Files:**
- Modify: `src/fs.h`, `src/fs.c`
- Test: `tests/test_fs.c`

- [ ] **Step 1: Zadeklaruj funkcje w `src/fs.h`**

Dodaj przed `int gh_fsck(...)`:

```c
int gh_fs_truncate(struct gh_fs*, const char *path, uint64_t new_size);
int gh_fs_utimens(struct gh_fs*, const char *path, uint64_t atime, uint64_t mtime);
int gh_fs_chmod(struct gh_fs*, const char *path, uint16_t mode);
int gh_fs_chown(struct gh_fs*, const char *path, uint32_t uid, uint32_t gid);
```

- [ ] **Step 2: Napisz failujący test w `tests/test_fs.c`**

Dodaj funkcję i zarejestruj w `main`:

```c
static void test_meta_ops(void) {
    char tmp[] = "/tmp/ghost_metaXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/f.txt", 0644), 0);
    const char *msg = "0123456789ABCDEF";
    CHECK_EQ(gh_fs_write(&fs, "/f.txt", msg, 16, 0), 16);

    /* truncate w dol */
    CHECK_EQ(gh_fs_truncate(&fs, "/f.txt", 8), 0);
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.size, 8);
    char buf[32] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/f.txt", buf, sizeof(buf), 0), 8);
    CHECK_EQ(memcmp(buf, "01234567", 8), 0);

    /* truncate na katalogu = EISDIR */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_truncate(&fs, "/d", 0), -EISDIR);

    /* chmod */
    CHECK_EQ(gh_fs_chmod(&fs, "/f.txt", 0600), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.mode, 0600);

    /* chown (uid=1000, gid bez zmiany) */
    CHECK_EQ(gh_fs_chown(&fs, "/f.txt", 1000, (uint32_t)-1), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.uid, 1000);

    /* utimens */
    CHECK_EQ(gh_fs_utimens(&fs, "/f.txt", 111, 222), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.atime, 111);
    CHECK_EQ(st.mtime, 222);

    /* nieistniejaca sciezka */
    CHECK_EQ(gh_fs_chmod(&fs, "/brak", 0600), -ENOENT);

    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj: `RUN_TEST(test_meta_ops);`

- [ ] **Step 3: Uruchom test — ma failować**

Run: `cc -std=c11 -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs 2>&1 | head`
Expected: błąd linkera (brak `gh_fs_truncate` itd.).

- [ ] **Step 4: Zaimplementuj funkcje w `src/fs.c`**

Na górze pliku dodaj `#include <time.h>` (przy istniejących include).
Dodaj funkcje (np. po `gh_fs_getattr`):

```c
int gh_fs_truncate(struct gh_fs *fs, const char *path, uint64_t new_size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type == GH_DIR) return -EISDIR;
    return gh_inode_truncate(&fs->dev, &fs->sb, ino, &n, new_size);
}

int gh_fs_utimens(struct gh_fs *fs, const char *path, uint64_t atime, uint64_t mtime) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    n.atime = atime; n.mtime = mtime; n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}

int gh_fs_chmod(struct gh_fs *fs, const char *path, uint16_t mode) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    n.mode = mode & 0777; n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}

int gh_fs_chown(struct gh_fs *fs, const char *path, uint32_t uid, uint32_t gid) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (uid != (uint32_t)-1) n.uid = uid;
    if (gid != (uint32_t)-1) n.gid = gid;
    n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}
```

- [ ] **Step 5: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs && ./build/test_fs`
Expected: `0 failed`

- [ ] **Step 6: ASan**

Run: `cc -std=c11 -fsanitize=address,undefined -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs_asan && ./build/test_fs_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 7: Commit**

```bash
git add src/fs.h src/fs.c tests/test_fs.c
git commit -m "feat: gh_fs_truncate/utimens/chmod/chown"
```

---

## Task 4: `statfs` i `sync`

**Files:**
- Modify: `src/fs.h`, `src/fs.c`
- Test: `tests/test_fs.c`

- [ ] **Step 1: Zadeklaruj w `src/fs.h`**

```c
int gh_fs_statfs(struct gh_fs*, struct gh_statfs *out);
int gh_fs_sync(struct gh_fs*);
```

- [ ] **Step 2: Napisz failujący test w `tests/test_fs.c`**

```c
static void test_statfs_sync(void) {
    char tmp[] = "/tmp/ghost_stfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    struct gh_statfs a;
    CHECK_EQ(gh_fs_statfs(&fs, &a), 0);
    CHECK_EQ(a.block_size, GH_BLOCK_SIZE);
    CHECK_EQ(a.total_blocks, 1024);
    CHECK(a.free_blocks > 0);
    uint64_t free_inodes_before = a.free_inodes;

    /* utworz plik z jednym blokiem -> mniej wolnych blokow i i-wezlow */
    CHECK_EQ(gh_fs_create(&fs, "/x", 0644), 0);
    char data[100]; memset(data, 'a', sizeof(data));
    CHECK_EQ(gh_fs_write(&fs, "/x", data, sizeof(data), 0), (ssize_t)sizeof(data));

    struct gh_statfs b;
    CHECK_EQ(gh_fs_statfs(&fs, &b), 0);
    CHECK(b.free_blocks < a.free_blocks);          /* zajeto blok danych */
    CHECK_EQ(b.free_inodes, free_inodes_before - 1); /* zajeto i-wezel */

    CHECK_EQ(gh_fs_sync(&fs), 0);

    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj: `RUN_TEST(test_statfs_sync);`

- [ ] **Step 3: Uruchom test — ma failować**

Run: `cc -std=c11 -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs 2>&1 | head`
Expected: błąd linkera.

- [ ] **Step 4: Zaimplementuj w `src/fs.c`**

Na górze dodaj `#include <unistd.h>` (dla `fsync`).
Dodaj funkcje:

```c
int gh_fs_statfs(struct gh_fs *fs, struct gh_statfs *out) {
    struct gh_superblock *sb = &fs->sb;
    out->block_size = GH_BLOCK_SIZE;
    out->total_blocks = sb->total_blocks;
    out->total_inodes = sb->inode_count;
    out->name_max = GH_NAME_MAX;
    uint64_t freeb = 0;
    for (uint64_t b = sb->data_start; b < sb->total_blocks; b++) {
        int s = 0; gh_bitmap_test(&fs->dev, sb, b, &s); if (!s) freeb++;
    }
    out->free_blocks = freeb;
    uint64_t freei = 0;
    for (uint64_t i = 0; i < sb->inode_count; i++) {
        struct gh_inode n;
        if (gh_inode_read(&fs->dev, sb, i, &n) == 0 && n.type == GH_FREE) freei++;
    }
    out->free_inodes = freei;
    return 0;
}

int gh_fs_sync(struct gh_fs *fs) {
    return fsync(fs->dev.fd) == 0 ? 0 : -errno;
}
```

- [ ] **Step 5: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs && ./build/test_fs`
Expected: `0 failed`

- [ ] **Step 6: ASan**

Run: `cc -std=c11 -fsanitize=address,undefined -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs_asan && ./build/test_fs_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 7: Commit**

```bash
git add src/fs.h src/fs.c tests/test_fs.c
git commit -m "feat: gh_fs_statfs + gh_fs_sync"
```

---

## Task 5: Twarde linki (`nlink`) + `gh_fs_link`

**Files:**
- Modify: `src/fs.h`, `src/fs.c`
- Test: `tests/test_fs.c`

- [ ] **Step 1: Zadeklaruj w `src/fs.h`**

```c
int gh_fs_link(struct gh_fs*, const char *oldpath, const char *newpath);
```

- [ ] **Step 2: Napisz failujący test w `tests/test_fs.c`**

```c
static void test_hardlink(void) {
    char tmp[] = "/tmp/ghost_lnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    const char *msg = "wspolna tresc";
    CHECK_EQ(gh_fs_write(&fs, "/a", msg, strlen(msg), 0), (ssize_t)strlen(msg));

    /* twardy link /b -> ten sam i-wezel */
    CHECK_EQ(gh_fs_link(&fs, "/a", "/b"), 0);
    struct gh_inode sa, sbb; uint64_t ia, ib;
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &sa, &ia), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &sbb, &ib), 0);
    CHECK_EQ(ia, ib);              /* ten sam i-wezel */
    CHECK_EQ(sbb.nlink, 2);

    /* odczyt przez /b daje te sama tresc */
    char buf[32] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/b", buf, sizeof(buf), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    /* unlink /a: i-wezel zyje (nlink 2->1), /b dalej czytelny */
    CHECK_EQ(gh_fs_unlink(&fs, "/a"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &sa, &ia), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &sbb, &ib), 0);
    CHECK_EQ(sbb.nlink, 1);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(gh_fs_read(&fs, "/b", buf, sizeof(buf), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    /* link na katalog = EPERM */
    CHECK_EQ(gh_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh_fs_link(&fs, "/dir", "/dir2"), -EPERM);

    /* po unlink /b mapa spojna (i-wezel zwolniony) */
    CHECK_EQ(gh_fs_unlink(&fs, "/b"), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj: `RUN_TEST(test_hardlink);`

- [ ] **Step 3: Uruchom test — ma failować**

Run: `cc -std=c11 -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs 2>&1 | head`
Expected: błąd linkera (brak `gh_fs_link`) i/lub FAIL (na razie `unlink` zawsze zwalnia).

- [ ] **Step 4: Zmień `remove_node` w `src/fs.c` (dekrement `nlink` dla plików)**

Zastąp końcówkę `remove_node` (od `r = gh_dir_remove(...)` do końca funkcji) tym:

```c
    r = gh_dir_remove(&fs->dev, &fs->sb, pino, name); if (r) return r;
    if (want_dir) {
        return gh_inode_free(&fs->dev, &fs->sb, ino);   /* katalogi nie sa wspoldzielone */
    }
    /* pliki/symlinki: zwolnij i-wezel dopiero przy ostatnim linku */
    struct gh_inode tn; r = gh_inode_read(&fs->dev, &fs->sb, ino, &tn); if (r) return r;
    if (tn.nlink > 1) {
        tn.nlink--; tn.ctime = (uint64_t)time(NULL);
        return gh_inode_write(&fs->dev, &fs->sb, ino, &tn);
    }
    return gh_inode_free(&fs->dev, &fs->sb, ino);
```

- [ ] **Step 5: Dodaj `gh_fs_link` w `src/fs.c`**

```c
int gh_fs_link(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, oldpath, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type == GH_DIR) return -EPERM;
    char parent[1024], name[256];
    r = gh_path_split(newpath, parent, name); if (r) return r;
    uint64_t pino; r = gh_path_resolve(&fs->dev, &fs->sb, parent, &pino);
    if (r) return r;
    uint64_t ex;
    if (gh_dir_lookup(&fs->dev, &fs->sb, pino, name, &ex) == 0) return -EEXIST;
    r = gh_dir_add(&fs->dev, &fs->sb, pino, name, ino); if (r) return r;
    n.nlink++; n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}
```

- [ ] **Step 6: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs && ./build/test_fs`
Expected: `0 failed`

- [ ] **Step 7: ASan**

Run: `cc -std=c11 -fsanitize=address,undefined -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_fs_asan && ./build/test_fs_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 8: Commit**

```bash
git add src/fs.h src/fs.c tests/test_fs.c
git commit -m "feat: twarde linki (gh_fs_link) + dekrement nlink przy unlink"
```

---

## Task 6: Dowiązania symboliczne — `symlink`/`readlink`

**Files:**
- Modify: `src/fs.h`, `src/fs.c`
- Create: `tests/test_links_xattr.c`

- [ ] **Step 1: Zadeklaruj w `src/fs.h`**

```c
int     gh_fs_symlink(struct gh_fs*, const char *target, const char *linkpath);
ssize_t gh_fs_readlink(struct gh_fs*, const char *path, char *buf, size_t size);
```

- [ ] **Step 2: Utwórz failujący test `tests/test_links_xattr.c`**

```c
#include "test.h"
#include "../src/fs.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void test_symlink(void) {
    char tmp[] = "/tmp/ghost_symXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    const char *target = "/a/b/c/cel.txt";
    CHECK_EQ(gh_fs_symlink(&fs, target, "/link"), 0);

    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/link", &st, &ino), 0);
    CHECK_EQ(st.type, GH_SYMLINK);

    char buf[256] = {0};
    ssize_t r = gh_fs_readlink(&fs, "/link", buf, sizeof(buf));
    CHECK_EQ(r, (ssize_t)strlen(target));
    CHECK_EQ(memcmp(buf, target, strlen(target)), 0);

    /* readlink na nie-symlinku = EINVAL */
    CHECK_EQ(gh_fs_create(&fs, "/plik", 0644), 0);
    CHECK_EQ(gh_fs_readlink(&fs, "/plik", buf, sizeof(buf)), -EINVAL);

    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_symlink);
    return TEST_SUMMARY();
}
```

- [ ] **Step 3: Uruchom test — ma failować**

Run: `cc -std=c11 -g tests/test_links_xattr.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_links_xattr 2>&1 | head`
Expected: błąd linkera (brak `gh_fs_symlink`/`gh_fs_readlink`).

- [ ] **Step 4: Zaimplementuj w `src/fs.c`**

```c
int gh_fs_symlink(struct gh_fs *fs, const char *target, const char *linkpath) {
    size_t tlen = strlen(target);
    if (tlen == 0) return -EINVAL;
    if (tlen > GH_SYMLINK_MAX) return -ENAMETOOLONG;
    char parent[1024], name[256];
    int r = gh_path_split(linkpath, parent, name); if (r) return r;
    uint64_t pino; r = gh_path_resolve(&fs->dev, &fs->sb, parent, &pino);
    if (r) return r;
    uint64_t ex;
    if (gh_dir_lookup(&fs->dev, &fs->sb, pino, name, &ex) == 0) return -EEXIST;
    uint64_t ino; r = gh_inode_alloc(&fs->dev, &fs->sb, GH_SYMLINK, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n);
    if (r) { gh_inode_free(&fs->dev, &fs->sb, ino); return r; }
    n.mode = 0777;
    if (gh_inode_pwrite(&fs->dev, &fs->sb, ino, &n, target, tlen, 0) != (ssize_t)tlen) {
        gh_inode_free(&fs->dev, &fs->sb, ino); return -EIO;
    }
    r = gh_dir_add(&fs->dev, &fs->sb, pino, name, ino);
    if (r) { gh_inode_free(&fs->dev, &fs->sb, ino); return r; }
    return 0;
}

ssize_t gh_fs_readlink(struct gh_fs *fs, const char *path, char *buf, size_t size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type != GH_SYMLINK) return -EINVAL;
    size_t cn = (n.size < size) ? n.size : size;
    return gh_inode_pread(&fs->dev, &fs->sb, &n, buf, cn, 0);
}
```

- [ ] **Step 5: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_links_xattr.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_links_xattr && ./build/test_links_xattr`
Expected: `0 failed`

- [ ] **Step 6: ASan**

Run: `cc -std=c11 -fsanitize=address,undefined -g tests/test_links_xattr.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/fs.c -o build/test_links_xattr_asan && ./build/test_links_xattr_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 7: Commit**

```bash
git add src/fs.h src/fs.c tests/test_links_xattr.c
git commit -m "feat: dowiazania symboliczne (gh_fs_symlink/readlink)"
```

---

## Task 7: Atrybuty rozszerzone (xattr) + zwalnianie `xattr_block` + `fsck`

**Files:**
- Create: `src/xattr.h`, `src/xattr.c`
- Modify: `src/fs.h`, `src/fs.c`, `src/inode.c`, `Makefile`
- Test: `tests/test_links_xattr.c`

- [ ] **Step 1: Utwórz `src/xattr.h`**

```c
#ifndef GH_XATTR_H
#define GH_XATTR_H
#include "ghostfs.h"
#include "block.h"
#include "inode.h"
#include "alloc.h"
#include <sys/types.h>

#define GH_XATTR_CREATE  1   /* zawiedź gdy istnieje */
#define GH_XATTR_REPLACE 2   /* zawiedź gdy nie istnieje */

int     gh_xattr_set(struct gh_dev*, const struct gh_superblock*, struct gh_inode *node,
                     uint64_t ino, const char *name, const void *val, size_t vlen, int flags);
ssize_t gh_xattr_get(struct gh_dev*, const struct gh_superblock*, const struct gh_inode *node,
                     const char *name, void *buf, size_t size);
ssize_t gh_xattr_list(struct gh_dev*, const struct gh_superblock*, const struct gh_inode *node,
                      char *buf, size_t size);
int     gh_xattr_remove(struct gh_dev*, const struct gh_superblock*, struct gh_inode *node,
                        uint64_t ino, const char *name);
#endif
```

- [ ] **Step 2: Napisz failujący test (dopisz do `tests/test_links_xattr.c`)**

Dodaj funkcję i zarejestruj w `main` (`RUN_TEST(test_xattr);`):

```c
static void test_xattr(void) {
    char tmp[] = "/tmp/ghost_xaXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);

    /* set + get */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.kolor", "zielony", 7, 0), 0);
    char val[64] = {0};
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, sizeof(val)), 7);
    CHECK_EQ(memcmp(val, "zielony", 7), 0);

    /* get size-only */
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", NULL, 0), 7);
    /* ERANGE gdy za maly bufor */
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, 3), -ERANGE);
    /* ENODATA gdy brak */
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.brak", val, sizeof(val)), -ENODATA);

    /* drugi atrybut + list */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.rozmiar", "duzy", 4, 0), 0);
    char list[128] = {0};
    ssize_t ln = gh_fs_listxattr(&fs, "/f", list, sizeof(list));
    CHECK_EQ(ln, (ssize_t)(strlen("user.kolor") + 1 + strlen("user.rozmiar") + 1));

    /* XATTR_CREATE na istniejacym = EEXIST; XATTR_REPLACE na braku = ENODATA */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.kolor", "x", 1, GH_XATTR_CREATE), -EEXIST);
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.nowy", "x", 1, GH_XATTR_REPLACE), -ENODATA);

    /* nadpisanie wartosci */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.kolor", "niebieski", 9, 0), 0);
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, sizeof(val)), 9);
    CHECK_EQ(memcmp(val, "niebieski", 9), 0);

    /* remove */
    CHECK_EQ(gh_fs_removexattr(&fs, "/f", "user.kolor"), 0);
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, sizeof(val)), -ENODATA);
    CHECK_EQ(gh_fs_removexattr(&fs, "/f", "user.brak"), -ENODATA);

    /* usun ostatni -> blok xattr zwolniony, fsck czyste */
    CHECK_EQ(gh_fs_removexattr(&fs, "/f", "user.rozmiar"), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 3: Utwórz `src/xattr.c`**

```c
#include "xattr.h"
#include <string.h>
#include <errno.h>

static int load(struct gh_dev *dev, const struct gh_superblock *sb,
                const struct gh_inode *node, uint8_t *buf) {
    memset(buf, 0, GH_BLOCK_SIZE);
    if (node->xattr_block == 0) return 0;
    return gh_block_read(dev, node->xattr_block, buf);
}

/* znajdz rekord po nazwie; zwroc offset lub -1; ustaw *vlen,*voff */
static long find(const uint8_t *buf, const char *name, uint16_t *vlen, size_t *voff) {
    size_t off = 0, nlen = strlen(name);
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, buf + off, 2); memcpy(&vl, buf + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        if (nl == nlen && memcmp(buf + off + 4, name, nl) == 0) {
            *vlen = vl; *voff = off + 4 + nl; return (long)off;
        }
        off += 4 + (size_t)nl + vl;
    }
    return -1;
}

int gh_xattr_set(struct gh_dev *dev, const struct gh_superblock *sb, struct gh_inode *node,
                 uint64_t ino, const char *name, const void *val, size_t vlen, int flags) {
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 0xFFFF || vlen > 0xFFFF) return -EINVAL;
    uint8_t buf[GH_BLOCK_SIZE]; int r = load(dev, sb, node, buf); if (r) return r;
    uint16_t ovl; size_t ovoff; long pos = find(buf, name, &ovl, &ovoff);
    int exists = pos >= 0;
    if ((flags & GH_XATTR_CREATE) && exists) return -EEXIST;
    if ((flags & GH_XATTR_REPLACE) && !exists) return -ENODATA;

    /* przepisz wszystko poza starym rekordem, dopisz nowy na koniec */
    uint8_t nb[GH_BLOCK_SIZE]; memset(nb, 0, sizeof(nb));
    size_t w = 0, off = 0;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, buf + off, 2); memcpy(&vl, buf + off + 2, 2);
        if (nl == 0) break;
        size_t rec = 4 + (size_t)nl + vl;
        if (off + rec > GH_BLOCK_SIZE) break;
        if ((long)off != pos) { memcpy(nb + w, buf + off, rec); w += rec; }
        off += rec;
    }
    size_t need = 4 + nlen + vlen;
    if (w + need > GH_XATTR_MAX) return -ENOSPC;
    uint16_t nl16 = (uint16_t)nlen, vl16 = (uint16_t)vlen;
    memcpy(nb + w, &nl16, 2); memcpy(nb + w + 2, &vl16, 2);
    memcpy(nb + w + 4, name, nlen); memcpy(nb + w + 4 + nlen, val, vlen);

    if (node->xattr_block == 0) {
        uint64_t b; r = gh_alloc_block(dev, sb, &b); if (r) return r;
        node->xattr_block = b;
        r = gh_inode_write(dev, sb, ino, node); if (r) return r;
    }
    return gh_block_write(dev, node->xattr_block, nb);
}

ssize_t gh_xattr_get(struct gh_dev *dev, const struct gh_superblock *sb,
                     const struct gh_inode *node, const char *name, void *buf, size_t size) {
    uint8_t blk[GH_BLOCK_SIZE]; int r = load(dev, sb, node, blk); if (r) return r;
    uint16_t vl; size_t voff; long pos = find(blk, name, &vl, &voff);
    if (pos < 0) return -ENODATA;
    if (size == 0) return vl;
    if (size < vl) return -ERANGE;
    memcpy(buf, blk + voff, vl);
    return vl;
}

ssize_t gh_xattr_list(struct gh_dev *dev, const struct gh_superblock *sb,
                      const struct gh_inode *node, char *buf, size_t size) {
    uint8_t blk[GH_BLOCK_SIZE]; int r = load(dev, sb, node, blk); if (r) return r;
    size_t off = 0, need = 0;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        need += (size_t)nl + 1;
        off += 4 + (size_t)nl + vl;
    }
    if (size == 0) return (ssize_t)need;
    if (size < need) return -ERANGE;
    off = 0; size_t w = 0;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        memcpy(buf + w, blk + off + 4, nl); w += nl; buf[w++] = '\0';
        off += 4 + (size_t)nl + vl;
    }
    return (ssize_t)need;
}

int gh_xattr_remove(struct gh_dev *dev, const struct gh_superblock *sb, struct gh_inode *node,
                    uint64_t ino, const char *name) {
    if (node->xattr_block == 0) return -ENODATA;
    uint8_t blk[GH_BLOCK_SIZE]; int r = load(dev, sb, node, blk); if (r) return r;
    uint16_t vl; size_t voff; long pos = find(blk, name, &vl, &voff);
    if (pos < 0) return -ENODATA;
    uint8_t nb[GH_BLOCK_SIZE]; memset(nb, 0, sizeof(nb));
    size_t w = 0, off = 0;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, v2; memcpy(&nl, blk + off, 2); memcpy(&v2, blk + off + 2, 2);
        if (nl == 0) break;
        size_t rec = 4 + (size_t)nl + v2;
        if (off + rec > GH_BLOCK_SIZE) break;
        if ((long)off != pos) { memcpy(nb + w, blk + off, rec); w += rec; }
        off += rec;
    }
    if (w == 0) {   /* pusto -> zwolnij blok */
        gh_free_block(dev, sb, node->xattr_block);
        node->xattr_block = 0;
        return gh_inode_write(dev, sb, ino, node);
    }
    return gh_block_write(dev, node->xattr_block, nb);
}
```

- [ ] **Step 4: Dodaj wrappery `gh_fs_*xattr` w `src/fs.c` i deklaracje w `src/fs.h`**

W `src/fs.h` (dodaj `#include "xattr.h"` na górze przy innych include, oraz deklaracje):

```c
int     gh_fs_setxattr(struct gh_fs*, const char *path, const char *name,
                       const void *val, size_t size, int flags);
ssize_t gh_fs_getxattr(struct gh_fs*, const char *path, const char *name,
                       void *buf, size_t size);
ssize_t gh_fs_listxattr(struct gh_fs*, const char *path, char *buf, size_t size);
int     gh_fs_removexattr(struct gh_fs*, const char *path, const char *name);
```

W `src/fs.c` dodaj:

```c
int gh_fs_setxattr(struct gh_fs *fs, const char *path, const char *name,
                   const void *val, size_t size, int flags) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_set(&fs->dev, &fs->sb, &n, ino, name, val, size, flags);
}
ssize_t gh_fs_getxattr(struct gh_fs *fs, const char *path, const char *name,
                       void *buf, size_t size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_get(&fs->dev, &fs->sb, &n, name, buf, size);
}
ssize_t gh_fs_listxattr(struct gh_fs *fs, const char *path, char *buf, size_t size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_list(&fs->dev, &fs->sb, &n, buf, size);
}
int gh_fs_removexattr(struct gh_fs *fs, const char *path, const char *name) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_remove(&fs->dev, &fs->sb, &n, ino, name);
}
```

- [ ] **Step 5: Zwalniaj `xattr_block` w `gh_inode_free` (`src/inode.c`)**

W `gh_inode_free`, tuż przed `memset(&n, 0, sizeof(n)); n.type = GH_FREE;`, dodaj:

```c
    if (n.xattr_block) { int e = gh_free_block(dev, sb, n.xattr_block); if (e && !first_err) first_err = e; }
```

- [ ] **Step 6: Dolicz `xattr_block` w `fsck` (`src/fs.c`)**

W pętli po i-węzłach w `gh_fsck`, po blokach danych (np. zaraz po `for (int i = 0; i < GH_NDIRECT; i++) ...`), dodaj:

```c
        if (n.xattr_block) mark(want, n.xattr_block, sb->total_blocks, &bad);
```

- [ ] **Step 7: Dodaj `src/xattr.c` do `CORE` w `Makefile`**

Zmień linię `CORE`:

```make
CORE    := src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/fs.c
```

- [ ] **Step 8: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_links_xattr.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/fs.c -o build/test_links_xattr && ./build/test_links_xattr`
Expected: `0 failed`

- [ ] **Step 9: ASan + pełny `make test`**

Run:
```
cc -std=c11 -fsanitize=address,undefined -g tests/test_links_xattr.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/fs.c -o build/test_links_xattr_asan && ./build/test_links_xattr_asan
make clean && make test
```
Expected: `0 failed` wszędzie, brak raportów ASan. (Reguła `build/%` zbuduje też `build/test_links_xattr`, bo `CORE` zawiera teraz `xattr.c`.)

- [ ] **Step 10: Commit**

```bash
git add src/xattr.h src/xattr.c src/fs.h src/fs.c src/inode.c Makefile tests/test_links_xattr.c
git commit -m "feat: atrybuty rozszerzone (xattr) + zwalnianie/fsck xattr_block"
```

---

## Task 8: `rename` (mv)

**Files:**
- Modify: `src/fs.h`, `src/fs.c`
- Test: `tests/test_fs.c`

- [ ] **Step 1: Zadeklaruj w `src/fs.h`**

```c
int gh_fs_rename(struct gh_fs*, const char *oldpath, const char *newpath);
```

- [ ] **Step 2: Napisz failujący test w `tests/test_fs.c`**

```c
static void test_rename(void) {
    char tmp[] = "/tmp/ghost_rnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* rename pliku w tym samym katalogu */
    CHECK_EQ(gh_fs_create(&fs, "/a.txt", 0644), 0);
    const char *msg = "tresc";
    CHECK_EQ(gh_fs_write(&fs, "/a.txt", msg, strlen(msg), 0), (ssize_t)strlen(msg));
    CHECK_EQ(gh_fs_rename(&fs, "/a.txt", "/b.txt"), 0);
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/a.txt", &st, &ino), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/b.txt", &st, &ino), 0);
    char buf[16] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/b.txt", buf, sizeof(buf), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    /* rename nadpisujacy istniejacy plik */
    CHECK_EQ(gh_fs_create(&fs, "/c.txt", 0644), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/b.txt", "/c.txt"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b.txt", &st, &ino), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/c.txt", &st, &ino), 0);

    /* rename katalogu ze zmiana rodzica: aktualizacja ".." */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d2", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1/sub", 0755), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/d1/sub", "/d2/sub"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/d2/sub", &st, &ino), 0);
    uint64_t dotdot;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2/sub/..", &dotdot), 0);
    uint64_t d2ino;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2", &d2ino), 0);
    CHECK_EQ(dotdot, d2ino);    /* ".." pokazuje nowego rodzica */

    /* nadpisanie niepustego katalogu = ENOTEMPTY */
    CHECK_EQ(gh_fs_mkdir(&fs, "/full", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/full/x", 0644), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/mover", 0755), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/mover", "/full"), -ENOTEMPTY);

    /* przeniesienie katalogu w jego poddrzewo = EINVAL */
    CHECK_EQ(gh_fs_rename(&fs, "/d2", "/d2/sub/x"), -EINVAL);

    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj: `RUN_TEST(test_rename);`

- [ ] **Step 3: Uruchom test — ma failować**

Run: `cc -std=c11 -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/fs.c -o build/test_fs 2>&1 | head`
Expected: błąd linkera (brak `gh_fs_rename`).

- [ ] **Step 4: Zaimplementuj w `src/fs.c`**

Dodaj dwa statyczne helpery i funkcję:

```c
/* czy `child` lezy w poddrzewie `parent` (po sciezce) */
static int is_subpath(const char *parent, const char *child) {
    size_t pl = strlen(parent);
    if (strncmp(parent, child, pl) != 0) return 0;
    return child[pl] == '/' || child[pl] == '\0';
}

/* nadpisz wpis ".." w katalogu `dino` na `newpar` */
static void update_dotdot(struct gh_dev *dev, const struct gh_superblock *sb,
                          uint64_t dino, struct gh_inode *dn, uint64_t newpar) {
    uint64_t DENT = sizeof(struct gh_dirent);
    for (uint64_t off = 0; off < dn->size; off += DENT) {
        struct gh_dirent de;
        if (gh_inode_pread(dev, sb, dn, &de, DENT, off) != (ssize_t)DENT) break;
        if (de.ino != 0 && de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.') {
            de.ino = newpar;
            gh_inode_pwrite(dev, sb, dino, dn, &de, DENT, off);
            return;
        }
    }
}

int gh_fs_rename(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t sino; int r = gh_path_resolve(&fs->dev, &fs->sb, oldpath, &sino);
    if (r) return r;
    if (sino == GH_ROOT_INO) return -EBUSY;
    struct gh_inode sn; r = gh_inode_read(&fs->dev, &fs->sb, sino, &sn); if (r) return r;

    char op[1024], on[256], np[1024], nn[256];
    r = gh_path_split(oldpath, op, on); if (r) return r;
    r = gh_path_split(newpath, np, nn); if (r) return r;
    uint64_t opino, npino;
    r = gh_path_resolve(&fs->dev, &fs->sb, op, &opino); if (r) return r;
    r = gh_path_resolve(&fs->dev, &fs->sb, np, &npino); if (r) return r;

    if (sn.type == GH_DIR && is_subpath(oldpath, newpath)) return -EINVAL;

    uint64_t tino;
    if (gh_dir_lookup(&fs->dev, &fs->sb, npino, nn, &tino) == 0) {
        if (tino == sino) return 0;
        struct gh_inode tn; r = gh_inode_read(&fs->dev, &fs->sb, tino, &tn); if (r) return r;
        if (sn.type == GH_DIR) {
            if (tn.type != GH_DIR) return -ENOTDIR;
            int empty; r = gh_dir_is_empty(&fs->dev, &fs->sb, tino, &empty); if (r) return r;
            if (!empty) return -ENOTEMPTY;
        } else {
            if (tn.type == GH_DIR) return -EISDIR;
        }
        r = gh_dir_remove(&fs->dev, &fs->sb, npino, nn); if (r) return r;
        if (tn.type != GH_DIR && tn.nlink > 1) {
            tn.nlink--; gh_inode_write(&fs->dev, &fs->sb, tino, &tn);
        } else {
            gh_inode_free(&fs->dev, &fs->sb, tino);
        }
    }

    r = gh_dir_add(&fs->dev, &fs->sb, npino, nn, sino); if (r) return r;
    r = gh_dir_remove(&fs->dev, &fs->sb, opino, on); if (r) return r;

    if (sn.type == GH_DIR && opino != npino) {
        struct gh_inode sd; gh_inode_read(&fs->dev, &fs->sb, sino, &sd);
        update_dotdot(&fs->dev, &fs->sb, sino, &sd, npino);
        struct gh_inode po, pn;
        if (gh_inode_read(&fs->dev, &fs->sb, opino, &po) == 0 && po.nlink > 0) {
            po.nlink--; gh_inode_write(&fs->dev, &fs->sb, opino, &po);
        }
        if (gh_inode_read(&fs->dev, &fs->sb, npino, &pn) == 0) {
            pn.nlink++; gh_inode_write(&fs->dev, &fs->sb, npino, &pn);
        }
    }
    return 0;
}
```

- [ ] **Step 5: Uruchom test — ma przejść**

Run: `cc -std=c11 -Wall -Wextra -Werror -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/fs.c -o build/test_fs && ./build/test_fs`
Expected: `0 failed`

- [ ] **Step 6: ASan**

Run: `cc -std=c11 -fsanitize=address,undefined -g tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/fs.c -o build/test_fs_asan && ./build/test_fs_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 7: Pełny `make test-asan`**

Run: `make test-asan`
Expected: każdy test `0 failed`, brak raportów ASan.

- [ ] **Step 8: Commit**

```bash
git add src/fs.h src/fs.c tests/test_fs.c
git commit -m "feat: rename (mv) z aktualizacja .. i nlink + ochrona poddrzewa"
```

---

## Task 9: Sterownik FUSE — realne handlery

**Files:**
- Modify: `src/fuse_main.c`

- [ ] **Step 1: Rozszerz `getattr` o symlink, uid/gid (`src/fuse_main.c`)**

Zastąp ciało `gf_getattr` (linie ustawiające `st_mode`/uid/gid) tym:

```c
static int gf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi; memset(st, 0, sizeof(*st));
    struct gh_inode n; uint64_t ino;
    int r = gh_fs_getattr(&g_fs, path, &n, &ino);
    if (r) return r;
    mode_t t = (n.type == GH_DIR) ? S_IFDIR
             : (n.type == GH_SYMLINK) ? S_IFLNK : S_IFREG;
    st->st_mode = t | n.mode;
    st->st_nlink = n.nlink; st->st_size = (off_t)n.size;
    st->st_uid = n.uid; st->st_gid = n.gid;
    st->st_atime = n.atime; st->st_mtime = n.mtime; st->st_ctime = n.ctime;
    return 0;
}
```

- [ ] **Step 2: Dodaj include'y u góry `src/fuse_main.c`**

Po istniejących `#include` dodaj:

```c
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
```

- [ ] **Step 3: Zastąp atrapy i dodaj nowe handlery (przed `static const struct fuse_operations ops`)**

Usuń stare `gf_truncate` i `gf_utimens` (atrapy `return 0`) i wstaw:

```c
static int gf_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi; return gh_fs_truncate(&g_fs, path, (uint64_t)size);
}
static int gf_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    uint64_t now = (uint64_t)time(NULL), at = now, mt = now;
    struct gh_inode n; uint64_t ino;
    int r = gh_fs_getattr(&g_fs, path, &n, &ino); if (r) return r;
    if (tv) {
        at = (tv[0].tv_nsec == UTIME_NOW) ? now
           : (tv[0].tv_nsec == UTIME_OMIT) ? n.atime : (uint64_t)tv[0].tv_sec;
        mt = (tv[1].tv_nsec == UTIME_NOW) ? now
           : (tv[1].tv_nsec == UTIME_OMIT) ? n.mtime : (uint64_t)tv[1].tv_sec;
    }
    return gh_fs_utimens(&g_fs, path, at, mt);
}
static int gf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; return gh_fs_chmod(&g_fs, path, (uint16_t)(mode & 0777));
}
static int gf_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi; return gh_fs_chown(&g_fs, path, (uint32_t)uid, (uint32_t)gid);
}
static int gf_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;            /* RENAME_EXCHANGE/NOREPLACE poza v1 */
    return gh_fs_rename(&g_fs, from, to);
}
static int gf_statfs(const char *path, struct statvfs *st) {
    (void)path; struct gh_statfs s; int r = gh_fs_statfs(&g_fs, &s); if (r) return r;
    memset(st, 0, sizeof(*st));
    st->f_bsize = s.block_size; st->f_frsize = s.block_size;
    st->f_blocks = s.total_blocks; st->f_bfree = s.free_blocks; st->f_bavail = s.free_blocks;
    st->f_files = s.total_inodes; st->f_ffree = s.free_inodes; st->f_favail = s.free_inodes;
    st->f_namemax = s.name_max;
    return 0;
}
static int gf_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi; return gh_fs_sync(&g_fs);
}
static int gf_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi; return gh_fs_sync(&g_fs);
}
static int gf_symlink(const char *target, const char *linkpath) {
    return gh_fs_symlink(&g_fs, target, linkpath);
}
static int gf_readlink(const char *path, char *buf, size_t size) {
    if (size == 0) return 0;
    ssize_t r = gh_fs_readlink(&g_fs, path, buf, size - 1);
    if (r < 0) return (int)r;
    buf[r] = '\0';
    return 0;
}
static int gf_link(const char *from, const char *to) {
    return gh_fs_link(&g_fs, from, to);
}
static int gf_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags) {
    return gh_fs_setxattr(&g_fs, path, name, value, size, flags);
}
static int gf_getxattr(const char *path, const char *name, char *value, size_t size) {
    return (int)gh_fs_getxattr(&g_fs, path, name, value, size);
}
static int gf_listxattr(const char *path, char *list, size_t size) {
    return (int)gh_fs_listxattr(&g_fs, path, list, size);
}
static int gf_removexattr(const char *path, const char *name) {
    return gh_fs_removexattr(&g_fs, path, name);
}
```

- [ ] **Step 4: Rozszerz tablicę `ops`**

Zastąp inicjalizator `ops`:

```c
static const struct fuse_operations ops = {
    .getattr = gf_getattr, .readdir = gf_readdir, .create = gf_create,
    .mkdir = gf_mkdir, .unlink = gf_unlink, .rmdir = gf_rmdir,
    .open = gf_open, .read = gf_read, .write = gf_write,
    .truncate = gf_truncate, .utimens = gf_utimens,
    .chmod = gf_chmod, .chown = gf_chown, .rename = gf_rename,
    .statfs = gf_statfs, .flush = gf_flush, .fsync = gf_fsync,
    .symlink = gf_symlink, .readlink = gf_readlink, .link = gf_link,
    .setxattr = gf_setxattr, .getxattr = gf_getxattr,
    .listxattr = gf_listxattr, .removexattr = gf_removexattr,
};
```

- [ ] **Step 5: Zbuduj sterownik**

Run: `make clean && make fuse`
Expected: powstaje `build/ghostfs` bez ostrzeżeń (wymaga `fuse3` + `pkg-config`).

- [ ] **Step 6: Test dymny montowania (jeśli dostępny FUSE)**

Run:
```
./build/ghostfs-cli format /tmp/fu.gfs 8192 256 2>/dev/null || make cli && ./build/ghostfs-cli format /tmp/fu.gfs 8192 256
mkdir -p /tmp/fumnt
./build/ghostfs /tmp/fu.gfs /tmp/fumnt -f &
sleep 1
echo "0123456789" > /tmp/fumnt/f.txt
: > /tmp/fumnt/f.txt   # truncate do 0 przez '>'
test -s /tmp/fumnt/f.txt && echo "BLAD: truncate nie dziala" || echo "OK: truncate"
ln -s /cel /tmp/fumnt/lnk && readlink /tmp/fumnt/lnk | grep -q '/cel' && echo "OK: symlink"
mkdir /tmp/fumnt/x && mv /tmp/fumnt/x /tmp/fumnt/y && test -d /tmp/fumnt/y && echo "OK: mv"
df /tmp/fumnt >/dev/null && echo "OK: statfs"
fusermount3 -u /tmp/fumnt
```
Expected: `OK: truncate`, `OK: symlink`, `OK: mv`, `OK: statfs`. (Jeśli środowisko nie ma FUSE — pomiń ten krok; pełne pokrycie i tak daje Task 11.)

- [ ] **Step 7: Commit**

```bash
git add src/fuse_main.c
git commit -m "feat: realne handlery FUSE (truncate/rename/statfs/chmod/chown/sync/symlink/link/xattr)"
```

---

## Task 10: CLI — `mv`/`truncate`/`ln`/`lns`/`chmod`/`stat`/`df`

**Files:**
- Modify: `src/cli.c`

- [ ] **Step 1: Dodaj funkcje komend w `src/cli.c`** (przed `int main`)

```c
static int cmd_mv(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "mv <plik> <stara> <nowa>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    int r = gh_fs_rename(&fs, argv[3], argv[4]); gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "mv: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_truncate(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "truncate <plik> <ścieżka> <rozmiar>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    int r = gh_fs_truncate(&fs, argv[3], strtoull(argv[4], 0, 10));
    gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "truncate: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_ln(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "ln <plik> <cel> <link>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    int r = gh_fs_link(&fs, argv[3], argv[4]); gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "ln: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_lns(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "lns <plik> <cel> <link>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    int r = gh_fs_symlink(&fs, argv[3], argv[4]); gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "lns: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_chmod(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "chmod <plik> <ścieżka> <ósemkowo>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    int r = gh_fs_chmod(&fs, argv[3], (uint16_t)strtol(argv[4], 0, 8));
    gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "chmod: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_stat(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "stat <plik> <ścieżka>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    struct gh_inode st; uint64_t ino;
    int r = gh_fs_getattr(&fs, argv[3], &st, &ino);
    if (!r) {
        const char *t = st.type == GH_DIR ? "katalog"
                      : st.type == GH_SYMLINK ? "symlink" : "plik";
        printf("typ=%s ino=%llu rozmiar=%llu mode=%o nlink=%u uid=%u gid=%u\n",
               t, (unsigned long long)ino, (unsigned long long)st.size,
               st.mode, st.nlink, st.uid, st.gid);
    }
    gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "stat: %s\n", strerror(-r)); return 1; }
    return 0;
}
static int cmd_df(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "df <plik>\n"); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    struct gh_statfs s; int r = gh_fs_statfs(&fs, &s); gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "df: %s\n", strerror(-r)); return 1; }
    printf("bloki: %llu/%llu wolne, i-wezly: %llu/%llu wolne, blok=%u B\n",
           (unsigned long long)s.free_blocks, (unsigned long long)s.total_blocks,
           (unsigned long long)s.free_inodes, (unsigned long long)s.total_inodes,
           s.block_size);
    return 0;
}
```

- [ ] **Step 2: Podłącz komendy w `main` (`src/cli.c`)**

Po istniejących `if (!strcmp(...))` dodaj przed linią „nieznana komenda":

```c
    if (!strcmp(c, "mv"))       return cmd_mv(argc, argv);
    if (!strcmp(c, "truncate")) return cmd_truncate(argc, argv);
    if (!strcmp(c, "ln"))       return cmd_ln(argc, argv);
    if (!strcmp(c, "lns"))      return cmd_lns(argc, argv);
    if (!strcmp(c, "chmod"))    return cmd_chmod(argc, argv);
    if (!strcmp(c, "stat"))     return cmd_stat(argc, argv);
    if (!strcmp(c, "df"))       return cmd_df(argc, argv);
```

Zaktualizuj też komunikat użycia w `main`:

```c
        fprintf(stderr, "uzycie: ghostfs-cli <format|ls|put|get|mkdir|rm|mv|truncate|ln|lns|chmod|stat|df|fsck> ...\n");
```

- [ ] **Step 3: Zbuduj CLI**

Run: `make cli`
Expected: powstaje `build/ghostfs-cli` bez ostrzeżeń.

- [ ] **Step 4: Test dymny CLI (round-trip nowych komend)**

Run:
```
./build/ghostfs-cli format /tmp/cli.gfs 2048 128
printf '0123456789' > /tmp/in.txt
./build/ghostfs-cli put /tmp/cli.gfs /tmp/in.txt /a.txt
./build/ghostfs-cli truncate /tmp/cli.gfs /a.txt 4
./build/ghostfs-cli stat /tmp/cli.gfs /a.txt | grep -q 'rozmiar=4' && echo OK_TRUNC
./build/ghostfs-cli mv /tmp/cli.gfs /a.txt /b.txt
./build/ghostfs-cli ls /tmp/cli.gfs / | grep -q b.txt && echo OK_MV
./build/ghostfs-cli lns /tmp/cli.gfs /cel /link
./build/ghostfs-cli stat /tmp/cli.gfs /link | grep -q 'typ=symlink' && echo OK_LNS
./build/ghostfs-cli chmod /tmp/cli.gfs /b.txt 600
./build/ghostfs-cli stat /tmp/cli.gfs /b.txt | grep -q 'mode=600' && echo OK_CHMOD
./build/ghostfs-cli df /tmp/cli.gfs | grep -q 'bloki:' && echo OK_DF
./build/ghostfs-cli fsck /tmp/cli.gfs | grep -q '0 niespójności' && echo OK_FSCK
```
Expected: `OK_TRUNC`, `OK_MV`, `OK_LNS`, `OK_CHMOD`, `OK_DF`, `OK_FSCK`.

- [ ] **Step 5: Commit**

```bash
git add src/cli.c
git commit -m "feat: CLI mv/truncate/ln/lns/chmod/stat/df"
```

---

## Task 11: Testy integracyjne FUSE — rozszerzenie

**Files:**
- Modify: `tests/integration.sh`

- [ ] **Step 1: Dodaj sekcje testów do `tests/integration.sh`**

Przed linią `fusermount3 -u "$MNT"; wait $FPID ...` (czyli po istniejących testach 1–4) dodaj:

```bash
# 5) truncate (skracanie przez '>')
printf '0123456789' > "$MNT/tr.txt"
: > "$MNT/tr.txt"
test ! -s "$MNT/tr.txt" && echo "OK: truncate do zera"

# 6) mv (plik i katalog)
echo hej > "$MNT/m1.txt"
mv "$MNT/m1.txt" "$MNT/m2.txt"
test "$(cat "$MNT/m2.txt")" = "hej" && echo "OK: mv pliku"
mkdir -p "$MNT/md/sub"
mv "$MNT/md/sub" "$MNT/sub2"
test -d "$MNT/sub2" && echo "OK: mv katalogu"

# 7) df (statfs)
df "$MNT" >/dev/null && echo "OK: df"

# 8) chmod + ls -l
echo x > "$MNT/perm.txt"
chmod 600 "$MNT/perm.txt"
ls -l "$MNT/perm.txt" | grep -q '^-rw-------' && echo "OK: chmod"

# 9) symlink
ln -s /jakis/cel "$MNT/symn"
test "$(readlink "$MNT/symn")" = "/jakis/cel" && echo "OK: symlink"

# 10) twardy link
echo wspolne > "$MNT/h1"
ln "$MNT/h1" "$MNT/h2"
test "$(cat "$MNT/h2")" = "wspolne" && echo "OK: hardlink"

# 11) xattr (jesli dostepne narzedzia)
if command -v setfattr >/dev/null && command -v getfattr >/dev/null; then
  echo y > "$MNT/xa.txt"
  setfattr -n user.test -v wartosc "$MNT/xa.txt"
  getfattr -n user.test --only-values "$MNT/xa.txt" 2>/dev/null | grep -q wartosc && echo "OK: xattr"
else
  echo "POMINIETO: xattr (brak setfattr/getfattr)"
fi
```

Zmień numerację istniejącego kroku „5) fsck po wszystkim" na „12) fsck po wszystkim" (komentarz).

- [ ] **Step 2: Uruchom testy jednostkowe + integracyjne**

Run: `make clean && make test && make cli fuse && ./tests/integration.sh`
Expected: wszystkie testy jednostkowe `0 failed`; integracyjne kończą się `OK:` dla kroków 1–11 (lub `POMINIETO: xattr`) i `WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY`. (Jeśli środowisko nie ma FUSE/`fusermount3`, część integracyjna nie wystartuje — odnotuj to; pełne pokrycie rdzenia daje `make test`.)

- [ ] **Step 3: Commit**

```bash
git add tests/integration.sh
git commit -m "test: integracyjne FUSE dla truncate/mv/df/chmod/symlink/hardlink/xattr"
```

---

## Self-Review (wykonane przy pisaniu planu)

**Pokrycie spec (pod-projekt A):**
- Format on-disk: `xattr_block` + `GH_SYMLINK` + `gh_statfs` → Task 1 ✓
- `truncate` (rdzeń + FS) → Task 2, 3 ✓
- `utimens`/`chmod`/`chown` → Task 3 ✓
- `statfs`/`sync` → Task 4 ✓
- twarde linki (`nlink`, `gh_fs_link`) → Task 5 ✓
- symlink/readlink → Task 6 ✓
- xattr (set/get/list/remove) + zwalnianie `xattr_block` + `fsck` → Task 7 ✓
- `rename` (z `..`, `nlink`, ochrona poddrzewa) → Task 8 ✓
- FUSE handlery (wszystkie) → Task 9 ✓
- CLI (`mv`/`truncate`/`ln`/`lns`/`chmod`/`stat`/`df`) → Task 10 ✓
- Testy: jednostkowe (2–8), FUSE smoke (9), integracyjne (11), ASan w każdym ✓
- Kody errno (EISDIR/ENOENT/EEXIST/ENOTEMPTY/EINVAL/EPERM/ENODATA/ERANGE/ENOSPC/ENAMETOOLONG) → Task 3,5,6,7,8 ✓

**Skan placeholderów:** brak „TBD/TODO/itp."; każdy krok modyfikujący kod zawiera kompletny kod.

**Spójność typów/nazw:** `gh_inode_truncate`, `gh_fs_truncate/utimens/chmod/chown/statfs/sync/link/symlink/readlink/rename`, `gh_fs_setxattr/getxattr/listxattr/removexattr`, `gh_xattr_set/get/list/remove`, `struct gh_statfs`, `GH_SYMLINK`, `xattr_block`, `GH_XATTR_CREATE/REPLACE` — użyte spójnie między zadaniami, nagłówkami i FUSE/CLI. Sygnatury FUSE3 (`gf_rename` z `flags`, `gf_chmod`/`gf_chown`/`gf_truncate`/`gf_utimens` z `fuse_file_info*`) zgodne z FUSE_USE_VERSION 31.

**Świadome uproszczenia (zgodnie ze spec):** xattr/symlink ≤ 1 blok; `rename` tylko flagi=0; pełna atomowość wielokroku i bariery transakcyjne → pod-projekt B (journaling).
