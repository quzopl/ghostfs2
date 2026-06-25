# ghostfs pod-projekt L — węzły specjalne (mknod): plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan + fsck czysty (rdev w direct[0] NIE psuje mapy).

**Goal:** FIFO/gniazda/urządzenia znakowe+blokowe (mknod) bez zmiany formatu; `rdev` w `direct[0]`; krytyczne strażniki w inode_free/fsck/truncate. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-L-mknod-design.md`.

**Tech Stack:** C11, libfuse3, OpenSSL, pthreads, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- Wszystkie testy A–K `0 failed`. Brak zmiany formatu (typy 4–7, rdev w direct[0]).

## File Structure
| Plik | Zmiana |
|---|---|
| `src/ghostfs.h` | typy `GH_FIFO/GH_SOCK/GH_CHR/GH_BLK` |
| `src/fs.h`/`fs.c` | `gh_fs_mknod`; `make_node` +rdev; strażniki w `gh_fsck`/`gh_fs_truncate` |
| `src/inode.c` | `gh_inode_free` pomija bloki węzłów specjalnych |
| `src/fuse_main.c` | getattr (typy+rdev); handler `mknod` |
| `src/cli.c` | komenda `mknod` |
| `tests/test_fs.c`, `tests/integration.sh` | testy |

---

## Task 1: Rdzeń mknod + strażniki + FUSE

**Files:** Modify: `src/ghostfs.h`, `src/fs.h`, `src/fs.c`, `src/inode.c`, `src/fuse_main.c`; Test: `tests/test_fs.c`

- [ ] **Step 1: Typy w `src/ghostfs.h`**

```c
enum gh_itype { GH_FREE = 0, GH_FILE = 1, GH_DIR = 2, GH_SYMLINK = 3,
                GH_FIFO = 4, GH_SOCK = 5, GH_CHR = 6, GH_BLK = 7 };
```

- [ ] **Step 2: `gh_inode_free` pomija bloki węzłów specjalnych (`src/inode.c`)**

Na początku `gh_inode_free`, po wczytaniu `n`, owiń zwalnianie bloków warunkiem typu z
treścią:

```c
int gh_inode_free(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t ino) {
    struct gh_inode n; int r = gh_inode_read(dev, sb, ino, &n); if (r) return r;
    int first_err = 0;
    int has_blocks = (n.type == GH_FILE || n.type == GH_DIR || n.type == GH_SYMLINK);
    if (has_blocks) {
        /* ... CAŁA dotychczasowa logika zwalniania direct/indirect/double ... */
    }
    /* xattr_block zwalniany ZAWSZE (po bloku has_blocks) — zachowaj istniejącą pętlę łańcucha xattr */
    /* ... istniejące zwalnianie xattr_block ... */
    memset(&n, 0, sizeof(n)); n.type = GH_FREE;
    r = gh_inode_write(dev, sb, ino, &n);
    return r ? r : first_err;
}
```

(Zachowaj istniejące zwalnianie łańcucha `xattr_block` POZA blokiem `has_blocks` — węzły
specjalne też mogą mieć xattr. Tylko `direct/indirect/double_indirect` są pod
`has_blocks`.)

- [ ] **Step 3: `gh_fsck` pomija znaczenie bloków węzłów specjalnych (`src/fs.c`)**

W pętli po i-węzłach, owiń znaczenie `direct/indirect/double_indirect` warunkiem typu;
`xattr_block` znacz zawsze:

```c
        if (gh_inode_read(&fs->dev, sb, ino, &n) || n.type == GH_FREE) continue;
        int has_blocks = (n.type == GH_FILE || n.type == GH_DIR || n.type == GH_SYMLINK);
        if (has_blocks) {
            for (int i = 0; i < GH_NDIRECT; i++)
                if (n.direct[i]) mark(want, n.direct[i], sb->total_blocks, &bad);
        }
        /* xattr_block: łańcuch — ZAWSZE (zachowaj istniejącą pętlę) */
        { ... istniejąca pętla xattr_block ... }
        if (has_blocks && n.indirect) { ... istniejące ... }
        if (has_blocks && n.double_indirect) { ... istniejące ... }
```

(Owiń tylko bloki danych warunkiem `has_blocks`; xattr_block bez zmian.)

- [ ] **Step 4: `gh_fs_truncate` chroni węzły specjalne (`src/fs.c`)**

Zmień tak, by truncate był dozwolony tylko dla pliku zwykłego:

```c
int gh_fs_truncate(struct gh_fs *fs, const char *path, uint64_t new_size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type == GH_DIR) return -EISDIR;
    if (n.type != GH_FILE) return -EINVAL;     /* symlink/węzeł specjalny: chroń rdev/treść */
    return gh_inode_truncate(&fs->dev, &fs->sb, ino, &n, new_size);
}
```

(Jeśli `gh_fs_truncate` jest opakowane przez `truncate_locked`/txn — zastosuj zmianę w
ciele `truncate_locked`.)

- [ ] **Step 5: `make_node` + `gh_fs_mknod` (`src/fs.c`, `src/fs.h`)**

Dodaj `#include <sys/stat.h>` w `src/fs.c`. Zmień sygnaturę `make_node` o `rdev` i ustaw
`direct[0]` dla urządzeń:

```c
static int make_node(struct gh_fs *fs, const char *path, uint16_t type, uint16_t mode, uint64_t rdev) {
    /* ... split, resolve parent, EEXIST, gh_inode_alloc(type) -> ino, read n ... */
    n.mode = mode;
    if (type == GH_DIR) {
        /* ... istniejące dodanie ./.. przez gh_dir_add, re-read, mode, nlink=2 ... */
    } else if (type == GH_CHR || type == GH_BLK) {
        n.direct[0] = rdev;
    }
    /* ... gh_inode_write(ino, &n); gh_dir_add(parent, name, ino); ... */
}
```

Zaktualizuj `gh_fs_create`/`gh_fs_mkdir`, by przekazywały `rdev=0`:

```c
int gh_fs_create(struct gh_fs *fs, const char *path, uint16_t mode) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, GH_FILE, mode, 0));
}
int gh_fs_mkdir(struct gh_fs *fs, const char *path, uint16_t mode) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, GH_DIR, mode, 0));
}
```

(Jeśli create/mkdir wołają `make_node` przez inne helpery — dostosuj wywołania o `0`.)

Dodaj `gh_fs_mknod`:

```c
int gh_fs_mknod(struct gh_fs *fs, const char *path, uint16_t mode, uint64_t rdev) {
    uint16_t type;
    if (S_ISFIFO(mode)) type = GH_FIFO;
    else if (S_ISSOCK(mode)) type = GH_SOCK;
    else if (S_ISCHR(mode)) type = GH_CHR;
    else if (S_ISBLK(mode)) type = GH_BLK;
    else if (S_ISREG(mode)) type = GH_FILE;
    else return -EINVAL;
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, type, (uint16_t)(mode & 07777), rdev));
}
```

Deklaracja w `src/fs.h`:

```c
int gh_fs_mknod(struct gh_fs*, const char *path, uint16_t mode, uint64_t rdev);
```

- [ ] **Step 6: FUSE getattr (typy + rdev) + handler mknod (`src/fuse_main.c`)**

W `gf_getattr` zastąp mapowanie typu:

```c
        mode_t t;
        switch (n.type) {
            case GH_DIR:     t = S_IFDIR;  break;
            case GH_SYMLINK: t = S_IFLNK;  break;
            case GH_FIFO:    t = S_IFIFO;  break;
            case GH_SOCK:    t = S_IFSOCK; break;
            case GH_CHR:     t = S_IFCHR;  break;
            case GH_BLK:     t = S_IFBLK;  break;
            default:         t = S_IFREG;  break;
        }
        st->st_mode = t | n.mode;
        if (n.type == GH_CHR || n.type == GH_BLK) st->st_rdev = (dev_t)n.direct[0];
        st->st_nlink = n.nlink; st->st_size = (off_t)n.size;
        st->st_uid = n.uid; st->st_gid = n.gid;
        st->st_atime = n.atime; st->st_mtime = n.mtime; st->st_ctime = n.ctime;
```

Dodaj handler (przy innych wr-handlerach):

```c
static int gf_mknod(const char *path, mode_t mode, dev_t rdev) {
    GF_WR(gh_fs_mknod(&g_fs, path, (uint16_t)mode, (uint64_t)rdev));
}
```

Uwaga: `mode` z FUSE zawiera bity typu (S_IFMT) — `gh_fs_mknod` ich potrzebuje, więc NIE
maskuj do 0777 w handlerze (maskowanie do 07777 robi rdzeń dla pola `mode`). Przekaż pełny
`mode`. Dodaj do tablicy `ops`: `.mknod = gf_mknod,`.

- [ ] **Step 7: Test w `tests/test_fs.c`** (+ RUN_TEST):

```c
#include <sys/stat.h>
#include <sys/sysmacros.h>
static void test_mknod(void) {
    char tmp[] = "/tmp/ghost_mkXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* FIFO */
    CHECK_EQ(gh_fs_mknod(&fs, "/p", S_IFIFO | 0644, 0), 0);
    struct gh_inode n; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/p", &n, &ino), 0); CHECK_EQ(n.type, GH_FIFO);
    /* gniazdo */
    CHECK_EQ(gh_fs_mknod(&fs, "/s", S_IFSOCK | 0644, 0), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/s", &n, &ino), 0); CHECK_EQ(n.type, GH_SOCK);
    /* urzadzenie znakowe 1,3 (/dev/null) */
    uint64_t rdev = makedev(1, 3);
    CHECK_EQ(gh_fs_mknod(&fs, "/cdev", S_IFCHR | 0666, rdev), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/cdev", &n, &ino), 0);
    CHECK_EQ(n.type, GH_CHR); CHECK_EQ(n.direct[0], rdev);     /* rdev w direct[0] */
    /* urzadzenie blokowe 8,0 */
    CHECK_EQ(gh_fs_mknod(&fs, "/bdev", S_IFBLK | 0660, makedev(8, 0)), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/bdev", &n, &ino), 0); CHECK_EQ(n.type, GH_BLK);

    /* fsck czysty: rdev w direct[0] NIE jest fałszywie osiagalnym blokiem */
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    /* truncate wezla specjalnego -> EINVAL (chroni rdev) */
    CHECK_EQ(gh_fs_truncate(&fs, "/cdev", 0), -EINVAL);
    /* mknod katalogu -> EINVAL */
    CHECK_EQ(gh_fs_mknod(&fs, "/d", S_IFDIR | 0755, 0), -EINVAL);
    /* hardlink urzadzenia: nlink rośnie */
    CHECK_EQ(gh_fs_link(&fs, "/cdev", "/cdev2"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/cdev2", &n, &ino), 0); CHECK_EQ(n.nlink, 2);

    /* unlink wezla -> fsck czysty, brak falszywego zwolnienia bloku */
    CHECK_EQ(gh_fs_unlink(&fs, "/cdev"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/cdev2"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/bdev"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/p"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/s"), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}
```

W `main` dodaj `RUN_TEST(test_mknod);`.

- [ ] **Step 8: Build + testy + ASan**

Run: `make clean && make test && make test-asan`
Expected: wszystkie `0 failed` (test_mknod; regresja A–K; **`fsck==0`** z węzłami specjalnymi
— `rdev` w `direct[0]` nie psuje mapy bitowej). Brak raportów ASan.

- [ ] **Step 9: Commit**

```bash
git add src/ghostfs.h src/fs.h src/fs.c src/inode.c src/fuse_main.c tests/test_fs.c
git commit -m "feat: wezly specjalne (mknod: FIFO/gniazdo/chr/blk) + straznicy inode_free/fsck/truncate"
```

---

## Task 2: CLI mknod + integracja

**Files:** Modify: `src/cli.c`, `tests/integration.sh`

- [ ] **Step 1: Komenda `mknod` w `src/cli.c`**

Dodaj `#include <sys/stat.h>`, `#include <sys/sysmacros.h>`. Dodaj funkcję i podłącz w `main`:

```c
static int cmd_mknod(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "mknod <plik> <ścieżka> <fifo|sock|chr|blk> [major minor]\n"); return 2; }
    uint16_t mode; uint64_t rdev = 0;
    const char *t = argv[4];
    if (!strcmp(t, "fifo")) mode = S_IFIFO | 0644;
    else if (!strcmp(t, "sock")) mode = S_IFSOCK | 0644;
    else if (!strcmp(t, "chr") || !strcmp(t, "blk")) {
        if (argc < 7) { fprintf(stderr, "chr/blk wymaga major minor\n"); return 2; }
        mode = (!strcmp(t, "chr") ? S_IFCHR : S_IFBLK) | 0666;
        rdev = makedev((unsigned)strtoul(argv[5], 0, 10), (unsigned)strtoul(argv[6], 0, 10));
    } else { fprintf(stderr, "nieznany typ: %s\n", t); return 2; }
    struct gh_fs fs; if (gh_fs_mount(&fs, argv[2])) return 1;
    int r = gh_fs_mknod(&fs, argv[3], mode, rdev); gh_fs_unmount(&fs);
    if (r) { fprintf(stderr, "mknod: %s\n", strerror(-r)); return 1; }
    return 0;
}
```

W `main`, przy innych komendach: `if (!strcmp(c, "mknod")) return cmd_mknod(argc, argv);`.
Dodaj `mknod` do komunikatu użycia.

- [ ] **Step 2: Integracja FUSE w `tests/integration.sh`**

Przed sekcją końcowego fsck dodaj test FIFO (zawsze) i urządzenia (gdy root):

```bash
# 16) wezly specjalne
mkfifo "$MNT/fifo1"
ok 'test -p "$MNT/fifo1"' 'mkfifo (FIFO)'
ls -l "$MNT/fifo1" | grep -q '^p' && echo "OK: ls -l FIFO (prw)" || echo "FAIL: FIFO ls"
if [ "$(id -u)" = 0 ] || sudo -n true 2>/dev/null; then
  SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
  $SUDO mknod "$MNT/nulldev" c 1 3 2>/dev/null && \
    ls -l "$MNT/nulldev" | grep -q '^c.* 1,  *3' && echo "OK: mknod urzadzenie znakowe (1,3)" || echo "POMINIETO: mknod dev"
else
  echo "POMINIETO: mknod urzadzenia (brak root)"
fi
```

(Funkcja `ok` istnieje. CLI `mknod` można też przetestować dymnie poza skryptem.)

- [ ] **Step 3: Build + pełna regresja**

Run: `make clean && make test && make cli fuse && ./tests/integration.sh`
Expected: testy jednostkowe `0 failed`; integracja `WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY`
(w tym `OK: mkfifo`, `OK: ls -l FIFO`). Posprzątaj mounty.

- [ ] **Step 4: Commit**

```bash
git add src/cli.c tests/integration.sh
git commit -m "feat: CLI mknod + integracja wezlow specjalnych (mkfifo/mknod)"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec L:** typy 4–7 (Task 1) ✓; gh_fs_mknod + make_node+rdev (Task 1) ✓; strażniki
inode_free/fsck/truncate (Task 1) ✓; getattr+handler FUSE (Task 1) ✓; CLI+integracja (Task 2)
✓.
**Placeholdery:** brak; kluczowy kod podany.
**Spójność:** `GH_FIFO/SOCK/CHR/BLK`; `gh_fs_mknod` (fs.h/.c, fuse, cli); `rdev` w
`direct[0]` spójnie ustawiane (make_node), czytane (getattr), POMIJANE w zwalnianiu
(inode_free)/znaczeniu (fsck)/truncate. Bez zmiany formatu.
**Ryzyka:** (1) KRYTYCZNE — rdev w direct[0] nie może być traktowany jak blok: strażniki
`has_blocks` w inode_free i fsck + truncate→EINVAL (bramka: fsck==0 z węzłami specjalnymi);
(2) xattr_block zwalniany/znaczony ZAWSZE (węzły specjalne mogą mieć xattr); (3) mode z FUSE
zawiera S_IFMT — handler przekazuje pełny mode, rdzeń maskuje do 07777 dla pola mode.
