# ghostfs pod-projekt G — funkcje odłożone: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan + brak regresji A–F.

**Goal:** UX/utwardzenie szyfrowania (prompt hasła, odrzucenie pustego hasła, wymazanie klucza), `RENAME_NOREPLACE`/`RENAME_EXCHANGE`, wielo-blokowe xattr. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-G-features-design.md`.

**Tech Stack:** C11, OpenSSL, termios, mini-harness, ASan.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto -lpthread`.
- Wszystkie testy A–F + integracja `0 failed`.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/crypto.h/.c` | `gh_crypto_wipe`, `gh_read_password` |
| `src/super.c` | `gh_format_enc` odrzuca puste hasło |
| `src/fs.c/.h` | `gh_fs_unmount` wymazuje klucz; `gh_fs_rename2` + flagi |
| `src/cli.c`, `src/fuse_main.c` | prompt hasła na EACCES; FUSE przekazuje flagi rename |
| `src/xattr.c/.h` | łańcuch bloków xattr |
| `src/inode.c` | `gh_inode_free` zwalnia łańcuch xattr |
| `tests/test_enc.c`, `tests/test_fs.c`, `tests/test_links_xattr.c` | testy |

---

## Task 1: UX i utwardzenie szyfrowania

**Files:** Modify: `src/crypto.h`, `src/crypto.c`, `src/super.c`, `src/fs.c`, `src/cli.c`, `src/fuse_main.c`; Test: `tests/test_enc.c`

- [ ] **Step 1: `gh_crypto_wipe` + `gh_read_password` w `src/crypto.h`**

```c
void gh_crypto_wipe(struct gh_cipher *c);
int  gh_read_password(const char *prompt, char *buf, size_t n);
```

- [ ] **Step 2: Implementacja w `src/crypto.c`**

Dodaj `#include <termios.h>`, `#include <stdio.h>`, `#include <openssl/crypto.h>`:

```c
void gh_crypto_wipe(struct gh_cipher *c) {
    if (c) OPENSSL_cleanse(c->key, sizeof(c->key));
}

int gh_read_password(const char *prompt, char *buf, size_t n) {
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return -ENOTTY;
    struct termios old, noecho;
    fputs(prompt, tty); fflush(tty);
    if (tcgetattr(fileno(tty), &old)) { fclose(tty); return -EIO; }
    noecho = old; noecho.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(fileno(tty), TCSANOW, &noecho);
    char *r = fgets(buf, (int)n, tty);
    tcsetattr(fileno(tty), TCSANOW, &old);
    fputc('\n', tty); fclose(tty);
    if (!r) return -EIO;
    size_t L = strlen(buf);
    if (L && buf[L-1] == '\n') buf[L-1] = '\0';
    return 0;
}
```

(Dodaj `#include <errno.h>`, `#include <string.h>` jeśli brak.)

- [ ] **Step 3: Odrzuć puste hasło w `gh_format_enc` (`src/super.c`)**

Zamień warunek szyfrowania. Zamiast `if (passphrase && passphrase[0]) { ... }` użyj:

```c
    if (passphrase != NULL) {
        if (passphrase[0] == '\0') { gh_dev_close(&dev); return -EINVAL; }  /* puste haslo = blad */
        encrypted = 1;
        sb.flags = GH_SB_ENCRYPTED;
        sb.enc_kdf_iters = 200000;
        if (gh_crypto_random(sb.enc_salt, sizeof(sb.enc_salt))) { gh_dev_close(&dev); return -EIO; }
        if (gh_crypto_derive(passphrase, sb.enc_salt, sb.enc_kdf_iters, cipher.key)) { gh_dev_close(&dev); return -EIO; }
        gh_crypto_verifier(cipher.key, sb.enc_salt, sb.enc_verifier);
    }
```

(Dostosuj do faktycznego układu zmiennych w `gh_format_enc` — kluczowe: `passphrase==""` → `-EINVAL`; `NULL` → jawny.)

- [ ] **Step 4: Wymaż klucz w `gh_fs_unmount` (`src/fs.c`)**

```c
void gh_fs_unmount(struct gh_fs *fs) {
    gh_bcache_destroy(&fs->dev);
    if (fs->dev.cipher) { gh_crypto_wipe(fs->dev.cipher); free(fs->dev.cipher); fs->dev.cipher = NULL; }
    gh_dev_close(&fs->dev);
}
```

- [ ] **Step 5: Prompt hasła w CLI (`src/cli.c`)**

Dodaj helper i użyj go wszędzie zamiast `gh_fs_mount(&fs, argv[2])`:

```c
static int cli_mount(struct gh_fs *fs, const char *path) {
    int r = gh_fs_mount(fs, path);
    if (r == -EACCES) {
        char pw[256];
        if (gh_read_password("Haslo ghostfs: ", pw, sizeof(pw)) == 0)
            r = gh_fs_mount_key(fs, path, pw);
    }
    return r;
}
```

Zamień wszystkie `gh_fs_mount(&fs, argv[2])` → `cli_mount(&fs, argv[2])` w komendach
(ls/put/get/mkdir/rm/mv/truncate/ln/lns/chmod/stat/df/fsck). Dodaj `#include "crypto.h"`
jeśli brak. (`cmd_format` bez zmian — używa `GHOSTFS_KEY` przez `gh_format_enc`.)

- [ ] **Step 6: Prompt hasła w FUSE (`src/fuse_main.c`)**

W `main`, gdzie jest `gh_fs_mount(&g_fs, container)`, na `-EACCES` spróbuj promptu:

```c
    int r = gh_fs_mount(&g_fs, container);
    if (r == -EACCES) {
        char pw[256];
        if (gh_read_password("Haslo ghostfs: ", pw, sizeof(pw)) == 0)
            r = gh_fs_mount_key(&g_fs, container, pw);
    }
    if (r) { fprintf(stderr, "mount: %s\n", strerror(-r)); ... }
```

(Dostosuj do istniejącej obsługi błędu mount; dodaj `#include "crypto.h"` jeśli brak.)

- [ ] **Step 7: Test polityki pustego hasła w `tests/test_enc.c`** (+ RUN_TEST):

```c
static void test_empty_pass_rejected(void) {
    char tmp[] = "/tmp/ghost_epXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 1024, 64, ""), -EINVAL);     /* puste -> blad */
    CHECK_EQ(gh_format_enc(tmp, 1024, 64, NULL), 0);         /* NULL -> jawny OK */
    unlink(tmp);
}
```

- [ ] **Step 8: Zbuduj wszystko i testy**

Run: `make clean && make test && make cli fuse`
Expected: `make test` `0 failed` (w tym `test_empty_pass_rejected`); CLI i FUSE budują się
czysto (`-Wall -Wextra -Werror`). `make test-asan` → `0 failed` (wymazanie klucza nie psuje
ASan — zero wycieków).

- [ ] **Step 9: Commit**

```bash
git add src/crypto.h src/crypto.c src/super.c src/fs.c src/cli.c src/fuse_main.c tests/test_enc.c
git commit -m "feat: prompt hasla (getpass), odrzucenie pustego hasla, wymazanie klucza (OPENSSL_cleanse)"
```

---

## Task 2: `RENAME_NOREPLACE` i `RENAME_EXCHANGE`

**Files:** Modify: `src/fs.h`, `src/fs.c`, `src/fuse_main.c`; Test: `tests/test_fs.c`

- [ ] **Step 1: API w `src/fs.h`**

```c
#define GH_RENAME_NOREPLACE 0x1u
#define GH_RENAME_EXCHANGE  0x2u
int gh_fs_rename2(struct gh_fs*, const char *oldpath, const char *newpath, unsigned flags);
```

- [ ] **Step 2: Napisz failujący test w `tests/test_fs.c`** (+ RUN_TEST):

```c
static void test_rename_flags(void) {
    char tmp[] = "/tmp/ghost_rfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    const char *ma = "AAAA"; CHECK_EQ(gh_fs_write(&fs, "/a", ma, 4, 0), 4);
    CHECK_EQ(gh_fs_create(&fs, "/b", 0644), 0);
    const char *mb = "BBBBBB"; CHECK_EQ(gh_fs_write(&fs, "/b", mb, 6, 0), 6);

    /* NOREPLACE: cel istnieje -> EEXIST */
    CHECK_EQ(gh_fs_rename2(&fs, "/a", "/b", GH_RENAME_NOREPLACE), -EEXIST);
    /* NOREPLACE: cel nie istnieje -> przenosi */
    CHECK_EQ(gh_fs_rename2(&fs, "/a", "/c", GH_RENAME_NOREPLACE), 0);
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &st, &ino), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/c", &st, &ino), 0);

    /* EXCHANGE: zamiana /c (AAAA) i /b (BBBBBB) */
    uint64_t ic, ib;
    CHECK_EQ(gh_fs_getattr(&fs, "/c", &st, &ic), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &st, &ib), 0);
    CHECK_EQ(gh_fs_rename2(&fs, "/c", "/b", GH_RENAME_EXCHANGE), 0);
    uint64_t ic2, ib2;
    CHECK_EQ(gh_fs_getattr(&fs, "/c", &st, &ic2), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &st, &ib2), 0);
    CHECK_EQ(ic2, ib);   /* zamienione */
    CHECK_EQ(ib2, ic);
    char buf[8] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/c", buf, sizeof(buf), 0), 6);   /* /c ma teraz BBBBBB */
    CHECK_EQ(memcmp(buf, mb, 6), 0);

    /* EXCHANGE: brak celu -> ENOENT */
    CHECK_EQ(gh_fs_rename2(&fs, "/c", "/niema", GH_RENAME_EXCHANGE), -ENOENT);
    /* obie flagi -> EINVAL */
    CHECK_EQ(gh_fs_rename2(&fs, "/c", "/b", GH_RENAME_NOREPLACE | GH_RENAME_EXCHANGE), -EINVAL);

    /* EXCHANGE katalog<->plik ze zmiana rodzica: ".." aktualizowane */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d2", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1/sub", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d2/file", 0644), 0);
    CHECK_EQ(gh_fs_rename2(&fs, "/d1/sub", "/d2/file", GH_RENAME_EXCHANGE), 0);
    uint64_t dd;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2/file/..", &dd), 0);  /* /d2/file to teraz katalog */
    uint64_t d2;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2", &d2), 0);
    CHECK_EQ(dd, d2);

    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 3: Uruchom — ma failować** (brak `gh_fs_rename2`):

Run: `make clean && make test 2>&1 | grep -iE 'rename2|test_fs' | head`
Expected: błąd linkera.

- [ ] **Step 4: Zaimplementuj w `src/fs.c`**

Obecne `gh_fs_rename` jest opakowane w transakcję i woła `rename_locked`. Dodaj helper
ustawiający `ino` wpisu po nazwie, funkcję wymiany i `rename2`:

```c
/* ustaw ino wpisu o nazwie `name` w katalogu `dir_ino` na `new_ino` */
static int set_entry_ino(struct gh_dev *dev, const struct gh_superblock *sb,
                         uint64_t dir_ino, const char *name, uint64_t new_ino) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    uint64_t DENT = sizeof(struct gh_dirent);
    for (uint64_t off = 0; off < dir.size; off += DENT) {
        struct gh_dirent de;
        if (gh_inode_pread(dev, sb, &dir, &de, DENT, off) != (ssize_t)DENT) break;
        if (de.ino != 0 && de.name_len == strlen(name)
            && strncmp(de.name, name, de.name_len) == 0) {
            de.ino = new_ino;
            if (gh_inode_pwrite(dev, sb, dir_ino, &dir, &de, DENT, off) != (ssize_t)DENT) return -EIO;
            return 0;
        }
    }
    return -ENOENT;
}

static int exchange_locked(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t sino, tino;
    int r = gh_path_resolve(&fs->dev, &fs->sb, oldpath, &sino); if (r) return r;
    r = gh_path_resolve(&fs->dev, &fs->sb, newpath, &tino); if (r) return r;
    if (sino == tino) return 0;
    if (sino == GH_ROOT_INO || tino == GH_ROOT_INO) return -EBUSY;
    char op[1024], on[256], np[1024], nn[256];
    r = gh_path_split(oldpath, op, on); if (r) return r;
    r = gh_path_split(newpath, np, nn); if (r) return r;
    uint64_t opino, npino;
    r = gh_path_resolve(&fs->dev, &fs->sb, op, &opino); if (r) return r;
    r = gh_path_resolve(&fs->dev, &fs->sb, np, &npino); if (r) return r;
    struct gh_inode sn, tn;
    r = gh_inode_read(&fs->dev, &fs->sb, sino, &sn); if (r) return r;
    r = gh_inode_read(&fs->dev, &fs->sb, tino, &tn); if (r) return r;
    /* zamien wpisy */
    r = set_entry_ino(&fs->dev, &fs->sb, opino, on, tino); if (r) return r;
    r = set_entry_ino(&fs->dev, &fs->sb, npino, nn, sino); if (r) return r;
    /* aktualizuj ".." gdy katalog zmienia rodzica */
    if (opino != npino) {
        if (sn.type == GH_DIR) {
            struct gh_inode sd; gh_inode_read(&fs->dev, &fs->sb, sino, &sd);
            update_dotdot(&fs->dev, &fs->sb, sino, &sd, npino);
        }
        if (tn.type == GH_DIR) {
            struct gh_inode td; gh_inode_read(&fs->dev, &fs->sb, tino, &td);
            update_dotdot(&fs->dev, &fs->sb, tino, &td, opino);
        }
    }
    return 0;
}

int gh_fs_rename2(struct gh_fs *fs, const char *oldpath, const char *newpath, unsigned flags) {
    if (flags & ~(GH_RENAME_NOREPLACE | GH_RENAME_EXCHANGE)) return -EINVAL;
    if ((flags & GH_RENAME_NOREPLACE) && (flags & GH_RENAME_EXCHANGE)) return -EINVAL;
    int b = txn_begin(fs); if (b) return b;
    int rc;
    if (flags & GH_RENAME_EXCHANGE) {
        rc = exchange_locked(fs, oldpath, newpath);
    } else if (flags & GH_RENAME_NOREPLACE) {
        uint64_t pino, t; char p[1024], n[256];
        if ((rc = gh_path_split(newpath, p, n)) == 0
            && (rc = gh_path_resolve(&fs->dev, &fs->sb, p, &pino)) == 0) {
            if (gh_dir_lookup(&fs->dev, &fs->sb, pino, n, &t) == 0) rc = -EEXIST;
            else rc = rename_locked(fs, oldpath, newpath);
        }
    } else {
        rc = rename_locked(fs, oldpath, newpath);
    }
    return txn_end_i(fs, rc);
}
```

Upewnij się, że istnieje statyczny `rename_locked(fs, old, new)` (z pod-projektu B —
wydzielone ciało rename). Jeśli `gh_fs_rename` ma ciało inline (nie `rename_locked`),
wydziel je do `static int rename_locked(struct gh_fs*, const char*, const char*)` i niech
`gh_fs_rename` woła `gh_fs_rename2(fs, old, new, 0)`. `update_dotdot` jest już statyczne w
fs.c (z B). Zachowaj opakowanie transakcją.

Zmień też `gh_fs_rename`, by delegował:

```c
int gh_fs_rename(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    return gh_fs_rename2(fs, oldpath, newpath, 0);
}
```

- [ ] **Step 5: FUSE przekazuje flagi (`src/fuse_main.c`)**

```c
static int gf_rename(const char *from, const char *to, unsigned int flags) {
    GF_WR(gh_fs_rename2(&g_fs, from, to, flags));
}
```

(Nieznane flagi odrzuci `gh_fs_rename2` → `-EINVAL`.)

- [ ] **Step 6: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_fs|failed'`
Expected: `test_fs` `0 failed`; reszta `0 failed`.

- [ ] **Step 7: ASan + pełny make test-asan**

Run: `make test-asan`
Expected: wszystkie `0 failed`.

- [ ] **Step 8: Commit**

```bash
git add src/fs.h src/fs.c src/fuse_main.c tests/test_fs.c
git commit -m "feat: RENAME_NOREPLACE i RENAME_EXCHANGE (gh_fs_rename2)"
```

---

## Task 3: Wielo-blokowe xattr (łańcuch)

**Files:** Modify: `src/xattr.c`, `src/inode.c`, `src/fs.c`; Test: `tests/test_links_xattr.c`

- [ ] **Step 1: Napisz failujący test w `tests/test_links_xattr.c`** (+ RUN_TEST):

```c
static void test_xattr_multiblock(void) {
    char tmp[] = "/tmp/ghost_xmXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);

    /* ustaw 10 atrybutow po 600 B wartosci => > 4 KB lacznie => wymusza lancuch */
    char val[600]; char nm[32];
    for (int i = 0; i < 10; i++) {
        memset(val, 'A' + i, sizeof(val));
        snprintf(nm, sizeof(nm), "user.attr%d", i);
        CHECK_EQ(gh_fs_setxattr(&fs, "/f", nm, val, sizeof(val), 0), 0);
    }
    /* odczytaj wszystkie poprawnie */
    char out[700];
    for (int i = 0; i < 10; i++) {
        snprintf(nm, sizeof(nm), "user.attr%d", i);
        CHECK_EQ(gh_fs_getxattr(&fs, "/f", nm, out, sizeof(out)), 600);
        char exp[600]; memset(exp, 'A' + i, sizeof(exp));
        CHECK_EQ(memcmp(out, exp, 600), 0);
    }
    /* lista zawiera 10 nazw */
    char list[512] = {0};
    ssize_t ln = gh_fs_listxattr(&fs, "/f", list, sizeof(list));
    CHECK(ln > 0);
    int cnt = 0; for (ssize_t k = 0; k < ln; k++) if (list[k] == '\0') cnt++;
    CHECK_EQ(cnt, 10);
    /* fsck czysty (lancuch osiagalny) */
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* usun wszystkie -> lancuch zwolniony */
    for (int i = 0; i < 10; i++) {
        snprintf(nm, sizeof(nm), "user.attr%d", i);
        CHECK_EQ(gh_fs_removexattr(&fs, "/f", nm), 0);
    }
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.attr0", out, sizeof(out)), -ENODATA);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* usun plik -> brak wyciekow (fsck czysty) */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.x", "y", 1, 0), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/f"), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 2: Przepisz `src/xattr.c` na łańcuch bloków**

Zastąp CAŁY `src/xattr.c`:

```c
#include "xattr.h"
#include <string.h>
#include <errno.h>

#define XREC_OFF 8                            /* rekordy po 8-bajtowym wskazniku next */
#define XREC_CAP (GH_BLOCK_SIZE - XREC_OFF)   /* pojemnosc rekordow w bloku */

static uint64_t next_of(const uint8_t *blk) { uint64_t n; memcpy(&n, blk, 8); return n; }
static void set_next(uint8_t *blk, uint64_t n) { memcpy(blk, &n, 8); }

/* znajdz nazwe w obrebie bloku; zwroc offset rekordu lub -1; ustaw *vlen,*voff */
static long find_in_block(const uint8_t *blk, const char *name, uint16_t *vlen, size_t *voff) {
    size_t off = XREC_OFF, nlen = strlen(name);
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        if (nl == nlen && memcmp(blk + off + 4, name, nl) == 0) {
            *vlen = vl; *voff = off + 4 + nl; return (long)off;
        }
        off += 4 + (size_t)nl + vl;
    }
    return -1;
}

static size_t used_in_block(const uint8_t *blk) {
    size_t off = XREC_OFF;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        off += 4 + (size_t)nl + vl;
    }
    return off - XREC_OFF;
}

/* usun rekord na offsecie `pos`, kompaktujac obszar rekordow (next nietkniety) */
static void remove_record(uint8_t *blk, long pos) {
    uint8_t tmp[GH_BLOCK_SIZE]; memcpy(tmp, blk, GH_BLOCK_SIZE);
    size_t w = XREC_OFF, off = XREC_OFF;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, tmp + off, 2); memcpy(&vl, tmp + off + 2, 2);
        if (nl == 0) break;
        size_t rec = 4 + (size_t)nl + vl;
        if (off + rec > GH_BLOCK_SIZE) break;
        if ((long)off != pos) { memcpy(blk + w, tmp + off, rec); w += rec; }
        off += rec;
    }
    memset(blk + w, 0, GH_BLOCK_SIZE - w);
}

int gh_xattr_set(struct gh_dev *dev, const struct gh_superblock *sb, struct gh_inode *node,
                 uint64_t ino, const char *name, const void *val, size_t vlen, int flags) {
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 0xFFFF || vlen > 0xFFFF) return -EINVAL;
    size_t need = 4 + nlen + vlen;
    if (need > XREC_CAP) return -E2BIG;

    uint8_t blk[GH_BLOCK_SIZE];
    int exists = 0;
    /* usun istniejacy rekord (jesli jest) */
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        int r = gh_block_read(dev, b, blk); if (r) return r;
        uint16_t vl; size_t voff; long pos = find_in_block(blk, name, &vl, &voff);
        if (pos >= 0) {
            exists = 1;
            if (flags & GH_XATTR_CREATE) return -EEXIST;
            remove_record(blk, pos);
            r = gh_block_write(dev, b, blk); if (r) return r;
            break;
        }
    }
    if ((flags & GH_XATTR_REPLACE) && !exists) return -ENODATA;

    /* znajdz blok z miejscem */
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        int r = gh_block_read(dev, b, blk); if (r) return r;
        size_t used = used_in_block(blk);
        if (used + need <= XREC_CAP) {
            size_t pos = XREC_OFF + used;
            uint16_t nl16 = (uint16_t)nlen, vl16 = (uint16_t)vlen;
            memcpy(blk + pos, &nl16, 2); memcpy(blk + pos + 2, &vl16, 2);
            memcpy(blk + pos + 4, name, nlen); memcpy(blk + pos + 4 + nlen, val, vlen);
            return gh_block_write(dev, b, blk);
        }
    }
    /* brak miejsca -> nowy blok jako glowa */
    uint64_t old_head = node->xattr_block;
    uint64_t nb; int r = gh_alloc_block(dev, sb, &nb); if (r) return r;
    memset(blk, 0, sizeof(blk));
    set_next(blk, old_head);
    uint16_t nl16 = (uint16_t)nlen, vl16 = (uint16_t)vlen;
    memcpy(blk + XREC_OFF, &nl16, 2); memcpy(blk + XREC_OFF + 2, &vl16, 2);
    memcpy(blk + XREC_OFF + 4, name, nlen); memcpy(blk + XREC_OFF + 4 + nlen, val, vlen);
    if ((r = gh_block_write(dev, nb, blk))) { gh_free_block(dev, sb, nb); return r; }
    node->xattr_block = nb;
    r = gh_inode_write(dev, sb, ino, node);
    if (r) { node->xattr_block = old_head; gh_free_block(dev, sb, nb); return r; }
    return 0;
}

ssize_t gh_xattr_get(struct gh_dev *dev, const struct gh_superblock *sb,
                     const struct gh_inode *node, const char *name, void *buf, size_t size) {
    uint8_t blk[GH_BLOCK_SIZE];
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        uint16_t vl; size_t voff; long pos = find_in_block(blk, name, &vl, &voff);
        if (pos >= 0) {
            if (size == 0) return vl;
            if (size < vl) return -ERANGE;
            memcpy(buf, blk + voff, vl); return vl;
        }
    }
    return -ENODATA;
}

ssize_t gh_xattr_list(struct gh_dev *dev, const struct gh_superblock *sb,
                      const struct gh_inode *node, char *buf, size_t size) {
    uint8_t blk[GH_BLOCK_SIZE];
    size_t need = 0;
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        size_t off = XREC_OFF;
        while (off + 4 <= GH_BLOCK_SIZE) {
            uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
            if (nl == 0) break;
            if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
            need += (size_t)nl + 1;
            off += 4 + (size_t)nl + vl;
        }
    }
    if (size == 0) return (ssize_t)need;
    if (size < need) return -ERANGE;
    size_t w = 0;
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        size_t off = XREC_OFF;
        while (off + 4 <= GH_BLOCK_SIZE) {
            uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
            if (nl == 0) break;
            if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
            memcpy(buf + w, blk + off + 4, nl); w += nl; buf[w++] = '\0';
            off += 4 + (size_t)nl + vl;
        }
    }
    return (ssize_t)need;
}

int gh_xattr_remove(struct gh_dev *dev, const struct gh_superblock *sb, struct gh_inode *node,
                    uint64_t ino, const char *name) {
    uint8_t blk[GH_BLOCK_SIZE];
    int found = 0;
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        uint16_t vl; size_t voff; long pos = find_in_block(blk, name, &vl, &voff);
        if (pos >= 0) {
            remove_record(blk, pos);
            if (gh_block_write(dev, b, blk)) return -EIO;
            found = 1; break;
        }
    }
    if (!found) return -ENODATA;
    /* caly lancuch pusty? -> zwolnij i wyzeruj xattr_block */
    int all_empty = 1;
    for (uint64_t b = node->xattr_block; b; b = next_of(blk)) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        if (used_in_block(blk) > 0) { all_empty = 0; break; }
    }
    if (all_empty) {
        uint64_t b = node->xattr_block;
        while (b) {
            if (gh_block_read(dev, b, blk)) break;
            uint64_t nx = next_of(blk);
            gh_free_block(dev, sb, b);
            b = nx;
        }
        node->xattr_block = 0;
        return gh_inode_write(dev, sb, ino, node);
    }
    return 0;
}
```

- [ ] **Step 3: `gh_inode_free` zwalnia łańcuch xattr (`src/inode.c`)**

Zamień fragment zwalniający pojedynczy `xattr_block`:

```c
    if (n.xattr_block) { int e = gh_free_block(dev, sb, n.xattr_block); if (e && !first_err) first_err = e; }
```

na przejście łańcucha:

```c
    {
        uint64_t xb = n.xattr_block;
        while (xb) {
            uint8_t xblk[GH_BLOCK_SIZE]; uint64_t nx = 0;
            if (gh_block_read(dev, xb, xblk) == 0) memcpy(&nx, xblk, 8);
            int e = gh_free_block(dev, sb, xb); if (e && !first_err) first_err = e;
            xb = nx;
        }
    }
```

- [ ] **Step 4: `gh_fsck` zaznacza cały łańcuch (`src/fs.c`)**

Zamień w pętli po i-węzłach:

```c
        if (n.xattr_block) mark(want, n.xattr_block, sb->total_blocks, &bad);
```

na przejście łańcucha z ochroną przed cyklem:

```c
        {
            uint64_t xb = n.xattr_block; uint64_t guard = 0;
            while (xb && guard < sb->total_blocks) {
                mark(want, xb, sb->total_blocks, &bad);
                uint8_t xblk[GH_BLOCK_SIZE]; uint64_t nx = 0;
                if (xb < sb->total_blocks && gh_block_read(&fs->dev, xb, xblk) == 0)
                    memcpy(&nx, xblk, 8);
                else break;
                xb = nx; guard++;
            }
        }
```

- [ ] **Step 5: Uruchom testy — mają przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_links_xattr|failed'`
Expected: `test_links_xattr` `0 failed` (w tym `test_xattr_multiblock` i istniejące
jednoblokowe testy xattr); cała reszta `0 failed`.

- [ ] **Step 6: Pełny ASan**

Run: `make test-asan`
Expected: wszystkie `0 failed`, brak raportów.

- [ ] **Step 7: Commit**

```bash
git add src/xattr.c src/inode.c src/fs.c tests/test_links_xattr.c
git commit -m "feat: wielo-blokowe xattr (lancuch blokow) + zwalnianie/fsck lancucha"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec G:** prompt hasła + puste hasło + wymazanie klucza (Task 1) ✓;
RENAME_NOREPLACE/EXCHANGE (Task 2) ✓; wielo-blokowe xattr (Task 3) ✓.
**Placeholdery:** brak; pełny kod crypto/rename2/xattr.
**Spójność:** `gh_crypto_wipe`/`gh_read_password` (crypto.h), `gh_fs_rename2`/
`GH_RENAME_*` (fs.h), łańcuch xattr (`next` w pierwszych 8 B) spójny w set/get/list/remove/
inode_free/fsck. `rename_locked`/`update_dotdot` reużyte z B.
**Ryzyka:** (1) prompt wymaga TTY → test sprawdza tylko politykę pustego hasła + ścieżkę
env; (2) EXCHANGE atomowy przez transakcję (B); aktualizacja `..` dla katalogów po obu
stronach; (3) łańcuch xattr — puste bloki pośrednie zostają (reużywane przez set), cały
pusty łańcuch zwalniany; fsck/inode_free idą po `next` z ochroną przed cyklem; (4)
wszystkie testy A–F bez regresji (jawne kontenery, jednoblokowe xattr, rename flags=0).
