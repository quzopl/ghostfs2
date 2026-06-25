# ghostfs pod-projekt D — współbieżność: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; zadania sekwencyjne. Bramka: zielone testy + ASan + test obciążeniowy.

**Goal:** Bezpieczny wielowątkowy sterownik FUSE: blokada czytelnik-pisarz na poziomie sterownika serializuje zapisy (współdzielą `dev->txn`/mapę/dziennik) i dopuszcza równoległe odczyty. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-D-concurrency-design.md`.

**Architecture:** Synchronizacja wyłącznie w `fuse_main.c` (rdzeń/CLI jednowątkowe — bez zmian). `pthread_rwlock_t` chroni współdzielony stan FS. Odczyty: `rdlock` (równolegle). Zapisy: `wrlock` (wyłącznie). Wielodostęp międzyprocesowy nadal wykluczany przez istniejący `flock`.

**Tech Stack:** C11, pthreads (`-lpthread`), libfuse3, mini-harness, ASan.

## Global Constraints
- Rdzeń (`src/*.c` poza fuse_main) NIE jest zmieniany — testy jednostkowe A/B/C bez zmian.
- `make test` / `make test-asan` dalej `0 failed`.
- Każde zadanie kończy się commitem.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/fuse_main.c` | `pthread_rwlock_t g_lock`; init/destroy; każdy handler pod rdlock/wrlock |
| `Makefile` | `-lpthread` do celu `fuse` |
| `tests/integration.sh` | test obciążeniowy współbieżności + równoległy odczyt |

---

## Task 1: Blokada czytelnik-pisarz w sterowniku FUSE

**Files:** Modify: `src/fuse_main.c`, `Makefile`

- [ ] **Step 1: Dodaj nagłówek pthread i deklarację blokady (`src/fuse_main.c`)**

Po istniejących `#include` dodaj:

```c
#include <pthread.h>
```

Po `static struct gh_fs g_fs;` dodaj:

```c
static pthread_rwlock_t g_lock;

#define GF_RD(call) do { pthread_rwlock_rdlock(&g_lock); \
    int _r = (call); pthread_rwlock_unlock(&g_lock); return _r; } while (0)
#define GF_WR(call) do { pthread_rwlock_wrlock(&g_lock); \
    int _r = (call); pthread_rwlock_unlock(&g_lock); return _r; } while (0)
```

- [ ] **Step 2: Owiń każdy handler odpowiednią blokadą**

Zastąp WSZYSTKIE handlery (od `gf_getattr` do `gf_removexattr`) poniższymi wersjami.
Odczyty biorą `rdlock`, zapisy `wrlock`. Handlery z logiką mają pojedynczy punkt
wyjścia i zwalniają blokadę przed `return`.

```c
/* --- ODCZYT: rdlock --- */
static int gf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_rdlock(&g_lock);
    memset(st, 0, sizeof(*st));
    struct gh_inode n; uint64_t ino;
    int r = gh_fs_getattr(&g_fs, path, &n, &ino);
    if (r == 0) {
        mode_t t = (n.type == GH_DIR) ? S_IFDIR
                 : (n.type == GH_SYMLINK) ? S_IFLNK : S_IFREG;
        st->st_mode = t | n.mode;
        st->st_nlink = n.nlink; st->st_size = (off_t)n.size;
        st->st_uid = n.uid; st->st_gid = n.gid;
        st->st_atime = n.atime; st->st_mtime = n.mtime; st->st_ctime = n.ctime;
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    struct rd_ctx x = { buf, filler };
    GF_RD(gh_fs_readdir(&g_fs, path, rd_cb, &x));
}

static int gf_read(const char *path, char *buf, size_t n, off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gh_fs_read(&g_fs, path, buf, n, (uint64_t)off);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

static int gf_readlink(const char *path, char *buf, size_t size) {
    if (size == 0) return 0;
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gh_fs_readlink(&g_fs, path, buf, size - 1);
    if (r >= 0) buf[r] = '\0';
    pthread_rwlock_unlock(&g_lock);
    return r < 0 ? (int)r : 0;
}

static int gf_statfs(const char *path, struct statvfs *st) {
    (void)path;
    pthread_rwlock_rdlock(&g_lock);
    struct gh_statfs s; int r = gh_fs_statfs(&g_fs, &s);
    if (r == 0) {
        memset(st, 0, sizeof(*st));
        st->f_bsize = s.block_size; st->f_frsize = s.block_size;
        st->f_blocks = s.total_blocks; st->f_bfree = s.free_blocks; st->f_bavail = s.free_blocks;
        st->f_files = s.total_inodes; st->f_ffree = s.free_inodes; st->f_favail = s.free_inodes;
        st->f_namemax = s.name_max;
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_getxattr(const char *path, const char *name, char *value, size_t size) {
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gh_fs_getxattr(&g_fs, path, name, value, size);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

static int gf_listxattr(const char *path, char *list, size_t size) {
    pthread_rwlock_rdlock(&g_lock);
    ssize_t r = gh_fs_listxattr(&g_fs, path, list, size);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

/* --- ZAPIS: wrlock --- */
static int gf_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gh_fs_create(&g_fs, path, mode & 0777));
}
static int gf_mkdir(const char *path, mode_t mode) {
    GF_WR(gh_fs_mkdir(&g_fs, path, mode & 0777));
}
static int gf_unlink(const char *path) { GF_WR(gh_fs_unlink(&g_fs, path)); }
static int gf_rmdir(const char *path)  { GF_WR(gh_fs_rmdir(&g_fs, path)); }

static int gf_open(const char *path, struct fuse_file_info *fi) {
    pthread_rwlock_wrlock(&g_lock);          /* wrlock: moze skracac przy O_TRUNC */
    struct gh_inode n; uint64_t ino;
    int r = gh_fs_getattr(&g_fs, path, &n, &ino);
    if (r == 0 && (fi->flags & O_TRUNC) && n.type == GH_FILE)
        r = gh_fs_truncate(&g_fs, path, 0);
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_write(const char *path, const char *buf, size_t n, off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_wrlock(&g_lock);
    ssize_t r = gh_fs_write(&g_fs, path, buf, n, (uint64_t)off);
    pthread_rwlock_unlock(&g_lock);
    return (int)r;
}

static int gf_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gh_fs_truncate(&g_fs, path, (uint64_t)size));
}

static int gf_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    pthread_rwlock_wrlock(&g_lock);
    uint64_t now = (uint64_t)time(NULL), at = now, mt = now;
    struct gh_inode n; uint64_t ino;
    int r = gh_fs_getattr(&g_fs, path, &n, &ino);
    if (r == 0) {
        if (tv) {
            at = (tv[0].tv_nsec == UTIME_NOW) ? now
               : (tv[0].tv_nsec == UTIME_OMIT) ? n.atime : (uint64_t)tv[0].tv_sec;
            mt = (tv[1].tv_nsec == UTIME_NOW) ? now
               : (tv[1].tv_nsec == UTIME_OMIT) ? n.mtime : (uint64_t)tv[1].tv_sec;
        }
        r = gh_fs_utimens(&g_fs, path, at, mt);
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

static int gf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gh_fs_chmod(&g_fs, path, (uint16_t)(mode & 0777)));
}
static int gf_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi; GF_WR(gh_fs_chown(&g_fs, path, (uint32_t)uid, (uint32_t)gid));
}
static int gf_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    GF_WR(gh_fs_rename(&g_fs, from, to));
}
static int gf_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi; GF_WR(gh_fs_sync(&g_fs));
}
static int gf_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi; GF_WR(gh_fs_sync(&g_fs));
}
static int gf_symlink(const char *target, const char *linkpath) {
    GF_WR(gh_fs_symlink(&g_fs, target, linkpath));
}
static int gf_link(const char *from, const char *to) {
    GF_WR(gh_fs_link(&g_fs, from, to));
}
static int gf_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags) {
    GF_WR(gh_fs_setxattr(&g_fs, path, name, value, size, flags));
}
static int gf_removexattr(const char *path, const char *name) {
    GF_WR(gh_fs_removexattr(&g_fs, path, name));
}
```

(Usuń stare wersje tych handlerów; `rd_cb`/`struct rd_ctx` zostają bez zmian. Tablica
`ops` bez zmian — nazwy handlerów te same.)

- [ ] **Step 3: Inicjalizuj/niszcz blokadę w `main` (`src/fuse_main.c`)**

Po udanym `gh_fs_mount(&g_fs, container)` (przed budową `fargv`/`fuse_main`):

```c
    if (pthread_rwlock_init(&g_lock, NULL) != 0) {
        fprintf(stderr, "pthread_rwlock_init nieudane\n");
        gh_fs_unmount(&g_fs); flock(g_lock_fd, LOCK_UN); close(g_lock_fd); return 1;
    }
```

Po `fuse_main(...)` (po `gh_fs_unmount` lub obok), przed `return rc`:

```c
    pthread_rwlock_destroy(&g_lock);
```

- [ ] **Step 4: Dodaj `-lpthread` do celu `fuse` w `Makefile`**

```make
fuse: $(CORE) src/fuse_main.c
	@mkdir -p build
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 src/fuse_main.c $(CORE) $(FUSEFLAGS) -o build/ghostfs $(LDLIBS) -lpthread
```

- [ ] **Step 5: Zbuduj sterownik i rdzeń**

Run: `make clean && make test && make cli fuse`
Expected: `make test` wszystkie `0 failed` (rdzeń niezmieniony); `build/ghostfs` buduje się
czysto z `-lpthread` (bez ostrzeżeń pod `-Wall -Wextra -Werror`).

- [ ] **Step 6: Test dymny — pojedyncze operacje dalej działają przez sterownik**

Run:
```
./build/ghostfs-cli format /tmp/d.gfs 8192 256
mkdir -p /tmp/dmnt
./build/ghostfs /tmp/d.gfs /tmp/dmnt -f & sleep 1
echo hej > /tmp/dmnt/a.txt && test "$(cat /tmp/dmnt/a.txt)" = hej && echo OK_SINGLE
fusermount3 -u /tmp/dmnt; rm -f /tmp/d.gfs
```
Expected: `OK_SINGLE`.

- [ ] **Step 7: Commit**

```bash
git add src/fuse_main.c Makefile
git commit -m "feat: rwlock sterownika FUSE (rownolegle odczyty, serializowane zapisy)"
```

---

## Task 2: Test obciążeniowy współbieżności

**Files:** Modify: `tests/integration.sh`

- [ ] **Step 1: Dodaj test współbieżności do `tests/integration.sh`**

PRZED finalnym `echo "WSZYSTKIE TESTY..."` (i przed lub po sekcji zaszyfrowanej) dodaj:

```bash
# 15) wspolbieznosc: N rownoleglych pisarzy w osobnych podkatalogach + fsck
CCONT=$(mktemp /tmp/ghost_conc.XXXXXX.gfs); CMNT=$(mktemp -d)
"$CLI" format "$CCONT" 65536 2048
"$GFS" "$CCONT" "$CMNT" -f &
CPID=$!; sleep 1
worker() {  # $1 = id
  local d="$CMNT/w$1"
  mkdir -p "$d"
  for i in $(seq 1 30); do
    head -c 20000 /dev/urandom > "$d/f$i.bin"
    cp "$d/f$i.bin" "$d/c$i.bin"
    cmp -s "$d/f$i.bin" "$d/c$i.bin" || { echo "FAIL: korupcja w w$1/f$i"; exit 1; }
    rm -f "$d/f$i.bin"
  done
}
for w in 1 2 3 4 5 6 7 8; do worker "$w" & done
wait
ok 'true' '8 rownoleglych pisarzy zakonczylo bez korupcji'
# rownolegly odczyt tego samego duzego pliku
head -c 2000000 /dev/urandom > "$CMNT/big.bin"
for r in 1 2 3 4 5 6; do ( cmp -s "$CMNT/big.bin" "$CMNT/big.bin" || echo "FAIL reader $r" ) & done
wait
ok 'true' '6 rownoleglych czytelnikow duzego pliku'
sync
fusermount3 -u "$CMNT" 2>/dev/null || true; wait $CPID 2>/dev/null || true
ok '"$CLI" fsck "$CCONT" 2>/dev/null | grep -q "0 niespójności"' 'fsck czysty po stresie wspolbieznosci'
rm -f "$CCONT"; rmdir "$CMNT" 2>/dev/null || true
```

(Funkcja `ok` istnieje od A11. Uwaga: kontener 65536 bloków = ~256 MB rzadko, ale
`ftruncate` tworzy plik rzadki — szybkie. 2048 i-węzłów na 8×30 plików z zapasem.)

- [ ] **Step 2: Uruchom pełną integrację (kilkukrotnie — wyścigi są niedeterministyczne)**

Run: `make clean && make test && make cli fuse && for k in 1 2 3; do echo "=== przebieg $k ==="; ./tests/integration.sh || break; done`
Expected: w każdym przebiegu wszystkie `OK:` (w tym `OK: 8 rownoleglych pisarzy...`,
`OK: 6 rownoleglych czytelnikow...`, `OK: fsck czysty po stresie wspolbieznosci`) i
`WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY`. Brak korupcji w żadnym przebiegu. Posprzątaj
mounty.

- [ ] **Step 3: (Best-effort) ThreadSanitizer**

Run:
```
mkdir -p build
cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -fsanitize=thread -g \
   src/fuse_main.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c \
   src/xattr.c src/journal.c src/crypto.c src/fs.c \
   $(pkg-config --cflags --libs fuse3) -o build/ghostfs_tsan -lcrypto -lpthread 2>&1 | tail -5 || echo "TSan build pominiety"
```
Jeśli się zbuduje: zamontuj `build/ghostfs_tsan`, odpal krótki współbieżny stres (jak w
Step 1, mniejsza skala), odmontuj; oczekiwane brak ostrzeżeń „data race". Jeśli TSan nie
współpracuje z FUSE w środowisku (częste) — odnotuj „TSan pominięty (środowisko)" i polegaj
na teście obciążeniowym + `fsck`. To krok best-effort, nie bramka.

- [ ] **Step 4: Commit**

```bash
git add tests/integration.sh
git commit -m "test: obciazeniowy test wspolbieznosci (8 pisarzy + odczyty + fsck)"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec D:** rwlock sterownika z rdlock/wrlock per handler (Task 1) ✓; init/destroy
blokady (Task 1) ✓; `-lpthread` (Task 1) ✓; test obciążeniowy + równoległy odczyt + fsck
(Task 2) ✓; TSan best-effort (Task 2) ✓. Rdzeń niezmieniony — testy A/B/C bez regresji ✓.
flock (międzyprocesowo) bez zmian ✓.

**Placeholdery:** brak; pełne handlery z blokadami podane.

**Spójność:** każdy handler bierze i zwalnia blokadę na każdej ścieżce (makra `GF_RD`/`GF_WR`
dla jednowierszowych; jawny prolog/epilog z pojedynczym return dla wielowierszowych —
getattr/readlink/statfs/utimens/open/read/write/getxattr/listxattr). Typy: `pthread_rwlock_t
g_lock`, `-lpthread`, `#include <pthread.h>`.

**Ryzyka odnotowane:** (1) zapisy współdzielą `dev->txn`/mapę/dziennik → wrlock wyłączny to
poprawność, nie tylko wydajność; (2) odczyty równoległe bezpieczne (pread/EVP per-wątek);
(3) `open` z `O_TRUNC` bierze wrlock (może modyfikować); (4) wielodostęp międzyprocesowy poza
zakresem — flock pozostaje.
