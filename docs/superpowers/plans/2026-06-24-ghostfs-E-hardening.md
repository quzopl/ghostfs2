# ghostfs pod-projekt E — utwardzenie: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie. Bramka: zielone testy + ASan.

**Goal:** fsck weryfikuje/naprawia całe drzewo (nlink/sieroty), deterministyczny test atomowości po awarii, odporność na uszkodzone kontenery, CI. Zgodnie ze spec `docs/superpowers/specs/2026-06-24-ghostfs-E-hardening-design.md`.

**Tech Stack:** C11, mini-harness, ASan, GitHub Actions.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L -lcrypto` (+ `-lpthread` dla fuse).
- Każde zadanie: zielone testy + ASan + commit.

## File Structure
| Plik | Zmiana |
|---|---|
| `src/fs.c` | `gh_fsck` rozszerzony o weryfikację/naprawę drzewa |
| `src/block.h`, `src/block.c` | pole `fail_after` w `gh_dev`; hook w `gh_disk_write` |
| `tests/test_fs.c` | testy fsck (nlink/sieroty/repair) |
| `tests/test_crash.c` | nowy: sweep awarii + odporność na uszkodzone kontenery |
| `.github/workflows/ci.yml` | nowy: CI |

---

## Task 1: fsck — weryfikacja i naprawa drzewa

**Files:** Modify: `src/fs.c`; Test: `tests/test_fs.c`

- [ ] **Step 1: Napisz failujący test w `tests/test_fs.c`** (+ RUN_TEST):

```c
static void test_fsck_tree(void) {
    char tmp[] = "/tmp/ghost_ftXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/a.txt", 0644), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    /* zepsuj nlink pliku recznie */
    uint64_t ino; struct gh_inode n;
    CHECK_EQ(gh_fs_getattr(&fs, "/a.txt", &n, &ino), 0);
    n.nlink = 5;
    CHECK_EQ(gh_inode_write(&fs.dev, &fs.sb, ino, &n), 0);
    issues = 0; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);  /* wykryte */
    issues = 0; CHECK_EQ(gh_fsck(&fs, 1, &issues), 0);                     /* napraw */
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0); /* czyste */
    CHECK_EQ(gh_fs_getattr(&fs, "/a.txt", &n, &ino), 0);
    CHECK_EQ(n.nlink, 1);   /* naprawione do liczby referencji */

    /* osierocony i-wezel: zaalokuj bez wpisu katalogowego */
    uint64_t orphan;
    CHECK_EQ(gh_inode_alloc(&fs.dev, &fs.sb, GH_FILE, &orphan), 0);
    issues = 0; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);  /* sierota wykryta */
    issues = 0; CHECK_EQ(gh_fsck(&fs, 1, &issues), 0);                     /* napraw: zwolnij */
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    struct gh_inode on; CHECK_EQ(gh_inode_read(&fs.dev, &fs.sb, orphan, &on), 0);
    CHECK_EQ(on.type, GH_FREE);   /* zwolniony */

    gh_fs_unmount(&fs); unlink(tmp);
}
```

- [ ] **Step 2: Uruchom — ma failować** (fsck jeszcze nie sprawdza drzewa):

Run: `make clean && make test 2>&1 | grep -E 'test_fs|failed' | head`
Expected: FAIL w `test_fsck_tree` (issues==0 mimo zepsucia).

- [ ] **Step 3: Rozszerz `gh_fsck` w `src/fs.c`**

W `gh_fsck`, ZARAZ po `struct gh_superblock *sb = &fs->sb;` (a przed alokacją `want`),
wstaw weryfikację i naprawę drzewa:

```c
    int tree_issues = 0;
    {
        uint8_t  *reach  = calloc(sb->inode_count, 1);
        uint32_t *refcnt = calloc(sb->inode_count, sizeof(uint32_t));
        uint64_t *stack  = malloc(sb->inode_count * sizeof(uint64_t));
        if (!reach || !refcnt || !stack) { free(reach); free(refcnt); free(stack); return -ENOMEM; }
        uint64_t sp = 0;
        if (sb->root_inode < sb->inode_count) { reach[sb->root_inode] = 1; stack[sp++] = sb->root_inode; }
        uint64_t DENT = sizeof(struct gh_dirent);
        while (sp > 0) {
            uint64_t dino = stack[--sp];
            struct gh_inode dn;
            if (gh_inode_read(&fs->dev, sb, dino, &dn) || dn.type != GH_DIR) continue;
            for (uint64_t off = 0; off < dn.size; off += DENT) {
                struct gh_dirent de;
                if (gh_inode_pread(&fs->dev, sb, &dn, &de, DENT, off) != (ssize_t)DENT) break;
                if (de.ino == 0 || de.ino >= sb->inode_count) continue;
                if (de.name_len == 1 && de.name[0] == '.') continue;
                if (de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.') continue;
                struct gh_inode tn;
                if (gh_inode_read(&fs->dev, sb, de.ino, &tn)) continue;
                if (tn.type == GH_DIR) {
                    if (!reach[de.ino]) { reach[de.ino] = 1; stack[sp++] = de.ino; }
                } else if (tn.type == GH_FILE || tn.type == GH_SYMLINK) {
                    reach[de.ino] = 1; refcnt[de.ino]++;
                }
            }
        }
        for (uint64_t ino = GH_ROOT_INO; ino < sb->inode_count; ino++) {
            if (ino == sb->root_inode) continue;
            struct gh_inode n;
            if (gh_inode_read(&fs->dev, sb, ino, &n) || n.type == GH_FREE) continue;
            if (!reach[ino]) {                       /* osierocony i-wezel */
                tree_issues++;
                if (repair) gh_inode_free(&fs->dev, sb, ino);
            } else if (n.type == GH_FILE || n.type == GH_SYMLINK) {
                if (n.nlink != refcnt[ino]) {        /* zly nlink */
                    tree_issues++;
                    if (repair) { n.nlink = refcnt[ino]; gh_inode_write(&fs->dev, sb, ino, &n); }
                }
            }
        }
        free(reach); free(refcnt); free(stack);
    }
```

Następnie zmień końcowe zliczanie, by uwzględniało `tree_issues`. Znajdź linię:

```c
    if (issues) *issues = (int)(diff + bad);
```

i zamień na:

```c
    if (issues) *issues = (int)(diff + bad + tree_issues);
```

(Naprawa drzewa odbywa się PRZED budową `want`/mapy, więc zwolnione sieroty są już
`GH_FREE` i ich bloki zwolnione przez `gh_inode_free` — mapa pozostaje spójna.)

- [ ] **Step 4: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_fs|failed'`
Expected: `test_fs` `0 failed`; cała reszta `0 failed` (zdrowe FS: brak fałszywych alarmów — `nlink`==refcnt, brak sierot; testy z `fsck==0` z A–D dalej zielone).

- [ ] **Step 5: ASan (test_fs)**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_fs.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c src/fs.c -o build/test_fs_asan -lcrypto && ./build/test_fs_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 6: Commit**

```bash
git add src/fs.c tests/test_fs.c
git commit -m "feat: fsck weryfikuje i naprawia drzewo (nlink plikow + osierocone i-wezly)"
```

---

## Task 2: Wstrzykiwanie awarii + odporność na uszkodzone kontenery

**Files:** Modify: `src/block.h`, `src/block.c`; Create: `tests/test_crash.c`

- [ ] **Step 1: Dodaj `fail_after` do `struct gh_dev` (`src/block.h`)**

```c
struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn;
                struct gh_cipher *cipher; long fail_after; };
```

- [ ] **Step 2: Hook w `gh_disk_write` (`src/block.c`)** — na początku funkcji:

```c
int gh_disk_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (dev->fail_after > 0) {
        if (--dev->fail_after == 0) return -EIO;   /* symulacja awarii zapisu */
    }
    if (blkno >= dev->total_blocks) return -EINVAL;
    /* ... reszta bez zmian ... */
```

Ustaw `dev->fail_after = 0;` w `gh_dev_create` i `gh_dev_open` (przy `dev->cipher=NULL`).

- [ ] **Step 3: Napisz `tests/test_crash.c`**

```c
#include "test.h"
#include "../src/fs.h"
#include "../src/super.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

/* sweep: awaria po N zapisach -> remount/recover -> fsck czysty + atomowosc */
static void test_crash_atomic(void) {
    for (int N = 1; N <= 40; N++) {
        char tmp[] = "/tmp/ghost_crXXXXXX"; int fd = mkstemp(tmp); close(fd);
        CHECK_EQ(gh_format(tmp, 4096, 128), 0);
        struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
        CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);   /* przygotowanie (bez awarii) */

        /* operacja z awaria po N zapisach */
        fs.dev.fail_after = N;
        const char *msg = "atomowa-tresc-pliku";
        gh_fs_create(&fs, "/d/f.txt", 0644);
        gh_fs_write(&fs, "/d/f.txt", msg, strlen(msg), 0);
        fs.dev.fail_after = 0;
        gh_fs_unmount(&fs);

        /* remount (recover) */
        CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
        int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
        CHECK_EQ(issues, 0);                          /* zawsze spojny */

        /* atomowosc: jesli plik istnieje, ma pelna tresc */
        struct gh_inode st; uint64_t ino;
        if (gh_fs_getattr(&fs, "/d/f.txt", &st, &ino) == 0 && st.type == GH_FILE && st.size > 0) {
            char buf[64] = {0};
            ssize_t r = gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0);
            if (r == (ssize_t)strlen(msg)) CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);
        }
        gh_fs_unmount(&fs); unlink(tmp);
    }
}

/* odpornosc: uszkodzone kontenery -> mount odmawia, nie crashuje */
static void test_corrupt_mount(void) {
    /* za krotki / zerowy plik */
    char tmp[] = "/tmp/ghost_c0XXXXXX"; int fd = mkstemp(tmp);
    ftruncate(fd, 100); close(fd);
    struct gh_fs fs; CHECK(gh_fs_mount(&fs, tmp) != 0);   /* odmowa, brak crasha */
    unlink(tmp);

    /* zdrowy kontener z losowymi bit-flipami w superbloku -> odmowa albo mount bez crasha */
    char g[] = "/tmp/ghost_cgXXXXXX"; int gfd = mkstemp(g); close(gfd);
    CHECK_EQ(gh_format(g, 1024, 64), 0);
    for (int t = 0; t < 50; t++) {
        char c[] = "/tmp/ghost_ccXXXXXX"; int cfd = mkstemp(c); close(cfd);
        /* skopiuj zdrowy -> c */
        int s = open(g, O_RDONLY), d = open(c, O_WRONLY|O_TRUNC);
        char blk[4096]; ssize_t k;
        while ((k = read(s, blk, sizeof(blk))) > 0) { ssize_t w = write(d, blk, (size_t)k); (void)w; }
        close(s);
        /* flip kilka bajtow w pierwszym bloku (superblok) */
        for (int f = 0; f < 8; f++) {
            off_t pos = (off_t)(rand() % 4096);
            uint8_t b; pread(d, &b, 1, pos); b ^= (uint8_t)(1u << (rand() % 8));
            pwrite(d, &b, 1, pos);
        }
        close(d);
        struct gh_fs cf;
        int r = gh_fs_mount(&cf, c);     /* nie crashuje: albo -errno, albo 0 */
        if (r == 0) gh_fs_unmount(&cf);
        unlink(c);
    }
    unlink(g);
    CHECK(1);  /* dotarliśmy bez crasha/ASan */
}

int main(void) {
    srand(2026);
    RUN_TEST(test_crash_atomic);
    RUN_TEST(test_corrupt_mount);
    return TEST_SUMMARY();
}
```

- [ ] **Step 4: Uruchom — ma failować, potem przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_crash|test_block|failed'`
Expected: PRZED implementacją hooka (jeśli test_crash zbudowany bez `fail_after`) — błąd
kompilacji (`fail_after` nieznane); PO Step 1-2 — `test_crash` `0 failed`. `test_block`
i reszta `0 failed`.

- [ ] **Step 5: ASan (test_crash)**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_crash.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c src/fs.c -o build/test_crash_asan -lcrypto && ./build/test_crash_asan`
Expected: `0 failed`, brak raportów (zwłaszcza w teście uszkodzonych kontenerów).

- [ ] **Step 6: Pełny make test-asan**

Run: `make test-asan`
Expected: wszystkie `0 failed`.

- [ ] **Step 7: Commit**

```bash
git add src/block.h src/block.c tests/test_crash.c
git commit -m "feat: wstrzykiwanie awarii (fail_after) + test atomowosci sweep + odpornosc na uszkodzone kontenery"
```

---

## Task 3: CI — GitHub Actions

**Files:** Create: `.github/workflows/ci.yml`

- [ ] **Step 1: Utwórz `.github/workflows/ci.yml`**

```yaml
name: ci
on:
  push:
    branches: [ master ]
  pull_request:
jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Zainstaluj zależności
        run: |
          sudo apt-get update
          sudo apt-get install -y libfuse3-dev libssl-dev attr pkg-config build-essential
      - name: Testy jednostkowe
        run: make test
      - name: Testy pod AddressSanitizer
        run: make test-asan
      - name: Budowa CLI i sterownika FUSE
        run: make cli fuse
      - name: Integracja FUSE (jeśli /dev/fuse dostępne)
        run: |
          if [ -e /dev/fuse ]; then
            chmod +x tests/integration.sh && ./tests/integration.sh
          else
            echo "POMINIETO: brak /dev/fuse w runnerze CI"
          fi
```

- [ ] **Step 2: Walidacja składni YAML lokalnie**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); print('YAML OK')"`
Expected: `YAML OK`. (Jeśli brak modułu yaml — pomiń, ale sprawdź wcięcia ręcznie.)

- [ ] **Step 3: Lokalna „próba" kroków CI**

Run: `make clean && make test && make test-asan && make cli fuse`
Expected: wszystkie `0 failed`, binarki zbudowane (to samo, co odpali CI).

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: GitHub Actions — make test + test-asan + build + integracja FUSE"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec E:** fsck drzewa (nlink/sieroty) + repair (Task 1) ✓; wstrzykiwanie awarii
+ sweep atomowości + odporność na uszkodzone kontenery (Task 2) ✓; CI (Task 3) ✓.
**Placeholdery:** brak; pełny kod fsck i testów.
**Spójność:** `tree_issues` dodane do `issues`; `fail_after` w `gh_dev` + hook w
`gh_disk_write`; naprawa drzewa PRZED mapą bloków (sieroty zwolnione → mapa spójna).
**Ryzyka:** (1) brak fałszywych alarmów na zdrowym FS — `nlink`==refcnt po normalnych
operacjach (zweryfikowane logicznie; testy A–D z `fsck==0` to bramka); (2) DFS z ochroną
zbiorem odwiedzonych — brak zapętlenia na uszkodzeniu; (3) hook `fail_after` domyślnie 0 =
zero wpływu na produkcję.
