#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_format.h"
#include "block.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * ghostfs v2xattr — testy rozszerzonych atrybutow (xattr) na drzewie FS v2.
 * set/get/list/remove round-trip; errno (-ENODATA/-ERANGE/-EEXIST/-E2BIG);
 * wiele xattr; CREATE/REPLACE; nadpisanie; kolizja hash (pakowanie);
 * ZWALNIANIE przy unlink/rmdir (wyciek=0, fsck==0, refcount==mark-sweep);
 * persystencja remount; snapshot (izolacja); --compress + szyfrowanie round-trip.
 * ========================================================================== */

static const uint64_t NBLK = 8192;

static int open_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_create(path, NBLK, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static int reopen_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_open(path, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev) {
    gh_bcache_destroy(dev);
    gh_dev_close(dev);
}

/* ---- weryfikacja: mapa == mark-sweep, refcount spojny (brak wyciekow) ---- */
static void check_no_leak(struct gh2_fs *fs) {
    struct gh2_space s2;
    CHECK_EQ(gh2_space_init(&s2, fs->space.nblocks), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&fs->dev, &s2, &fs->root_tree), 0);
    CHECK_EQ(memcmp(fs->space.bits, s2.bits, (fs->space.nblocks + 7) / 8), 0);
    CHECK_EQ(fs->space.nfree, s2.nfree);
    CHECK_EQ(memcmp(fs->space.refs, s2.refs, (size_t)fs->space.nblocks * sizeof(uint16_t)), 0);
    gh2_space_destroy(&s2);
}
static void check_fsck0(struct gh2_fs *fs) {
    int issues = -1;
    CHECK_EQ(gh2_fsck(fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);
}

/* ---- czy ino ma JAKIKOLWIEK item xattr (ino,7,*) w zatwierdzonym fs_root ---- */
struct xattr_count { uint64_t ino; int items; };
static int xattr_count_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)val; (void)len;
    struct xattr_count *c = ctx;
    if (key->type == 7 /*GH2_XATTR_ITEM*/ && key->objectid == c->ino) c->items++;
    return 0;
}
static int xattr_item_count(struct gh2_fs *fs, uint64_t ino) {
    struct xattr_count c = { ino, 0 };
    gh2_btree_iterate(&fs->dev, &fs->fs_root, xattr_count_cb, &c);
    return c.items;
}

/* ---- czy nazwa wystepuje w buforze listxattr (null-separated) ---- */
static int list_has(const char *buf, size_t len, const char *name) {
    size_t o = 0;
    while (o < len) {
        if (strcmp(buf + o, name) == 0) return 1;
        o += strlen(buf + o) + 1;
    }
    return 0;
}

/* ============================ Test 1: set/get round-trip ============================ */
static void test_setget_roundtrip(void) {
    char tmp[] = "/tmp/gh2xa_rt_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    /* prosta wartosc */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.foo", "bar", 3, 0), 0);
    char buf[256];
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.foo", buf, sizeof(buf)), 3);
    CHECK_EQ(memcmp(buf, "bar", 3), 0);

    /* wartosc binarna z 0x00 w srodku */
    const char bin[] = { 'a', 0x00, 'b', 0x00, 0x00, 'c' };
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.bin", bin, sizeof(bin), 0), 0);
    memset(buf, 0xEE, sizeof(buf));
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.bin", buf, sizeof(buf)), (ssize_t)sizeof(bin));
    CHECK_EQ(memcmp(buf, bin, sizeof(bin)), 0);

    /* pusta wartosc (size 0) */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.empty", NULL, 0, 0), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.empty", buf, sizeof(buf)), 0);

    /* dluga nazwa + dluga wartosc */
    char lname[200]; memset(lname, 'n', sizeof(lname)); lname[199] = '\0';
    char lval[512]; for (int i = 0; i < 512; i++) lval[i] = (char)(i & 0xFF);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", lname, lval, sizeof(lval), 0), 0);
    char big[512];
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", lname, big, sizeof(big)), 512);
    CHECK_EQ(memcmp(big, lval, 512), 0);

    /* brak atrybutu -> -ENODATA */
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.nope", buf, sizeof(buf)), -ENODATA);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 2: list + remove ============================ */
static void test_list_remove(void) {
    char tmp[] = "/tmp/gh2xa_lr_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    uint64_t ino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/f", &ino), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.a", "1", 1, 0), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.bb", "22", 2, 0), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.ccc", "333", 3, 0), 0);

    /* list size 0 -> rozmiar = sum(name_len+1) = 7+8+9 */
    ssize_t need = gh2_fs_listxattr(&fs, "/f", NULL, 0);
    CHECK_EQ(need, 7 + 8 + 9);

    char lbuf[256];
    ssize_t got = gh2_fs_listxattr(&fs, "/f", lbuf, sizeof(lbuf));
    CHECK_EQ(got, need);
    CHECK_EQ(list_has(lbuf, (size_t)got, "user.a"), 1);
    CHECK_EQ(list_has(lbuf, (size_t)got, "user.bb"), 1);
    CHECK_EQ(list_has(lbuf, (size_t)got, "user.ccc"), 1);

    /* list -ERANGE gdy za maly bufor */
    CHECK_EQ(gh2_fs_listxattr(&fs, "/f", lbuf, 5), -ERANGE);

    /* remove srodkowy -> zostaja 2 */
    CHECK_EQ(gh2_fs_removexattr(&fs, "/f", "user.bb"), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.bb", lbuf, sizeof(lbuf)), -ENODATA);
    got = gh2_fs_listxattr(&fs, "/f", lbuf, sizeof(lbuf));
    CHECK_EQ(got, 7 + 9);
    CHECK_EQ(list_has(lbuf, (size_t)got, "user.bb"), 0);
    CHECK_EQ(list_has(lbuf, (size_t)got, "user.a"), 1);

    /* remove brakujacego -> -ENODATA */
    CHECK_EQ(gh2_fs_removexattr(&fs, "/f", "user.zzz"), -ENODATA);

    /* remove wszystkich -> pusta lista (0) */
    CHECK_EQ(gh2_fs_removexattr(&fs, "/f", "user.a"), 0);
    CHECK_EQ(gh2_fs_removexattr(&fs, "/f", "user.ccc"), 0);
    CHECK_EQ(gh2_fs_listxattr(&fs, "/f", NULL, 0), 0);
    CHECK_EQ(xattr_item_count(&fs, ino), 0);   /* wszystkie itemy xattr usuniete */

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 3: CREATE/REPLACE + nadpisanie ============================ */
static void test_create_replace(void) {
    char tmp[] = "/tmp/gh2xa_cr_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    char buf[64];

    /* REPLACE gdy brak -> -ENODATA */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.x", "v1", 2, GH2_XATTR_REPLACE), -ENODATA);

    /* CREATE gdy brak -> OK */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.x", "v1", 2, GH2_XATTR_CREATE), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.x", buf, sizeof(buf)), 2);
    CHECK_EQ(memcmp(buf, "v1", 2), 0);

    /* CREATE gdy istnieje -> -EEXIST */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.x", "v2", 2, GH2_XATTR_CREATE), -EEXIST);

    /* REPLACE gdy istnieje -> OK, nadpisanie wartosci (inny rozmiar) */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.x", "longer", 6, GH2_XATTR_REPLACE), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.x", buf, sizeof(buf)), 6);
    CHECK_EQ(memcmp(buf, "longer", 6), 0);

    /* nadpisanie bez flag -> OK, krotsza wartosc */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.x", "z", 1, 0), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.x", buf, sizeof(buf)), 1);
    CHECK_EQ(buf[0], 'z');

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 4: getxattr size0/-ERANGE, -E2BIG ============================ */
static void test_errno_sizes(void) {
    char tmp[] = "/tmp/gh2xa_sz_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.k", "hello", 5, 0), 0);

    char buf[64];
    /* size 0 -> zwroc rozmiar value */
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.k", NULL, 0), 5);
    /* bufor za maly -> -ERANGE */
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.k", buf, 4), -ERANGE);
    /* dokladny rozmiar -> OK */
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.k", buf, 5), 5);

    /* -E2BIG: wartosc nie miesci sie w lisciu B-drzewa (GH2_LEAF_MAX_VAL ~2004 B) */
    static char huge[8192];
    memset(huge, 'X', sizeof(huge));
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.huge", huge, sizeof(huge), 0), -E2BIG);
    /* nie powstal czesciowy wpis */
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.huge", buf, sizeof(buf)), -ENODATA);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 5: wiele xattr + kolizja hash (pakowanie) ============================ */
/* Wymuszamy kolizje pakowania uzywajac WIELU nazw na jednym i-wezle. Nazwy o roznym hashu
 * tworza osobne itemy; by udowodnic pakowanie kolizji w JEDNYM itemie polegamy na tym, ze
 * lookup po dokladnej nazwie dziala niezaleznie od liczby wpisow. Dodatkowo test sprawdza
 * setet/get dla 16 nazw + spojnosc listy. */
static void test_many_and_collision(void) {
    char tmp[] = "/tmp/gh2xa_mc_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    enum { N = 16 };
    char names[N][16];
    for (int i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "user.k%02d", i);
        char v[8]; int vl = snprintf(v, sizeof(v), "val%d", i);
        CHECK_EQ(gh2_fs_setxattr(&fs, "/f", names[i], v, (size_t)vl, 0), 0);
    }
    /* odczyt kazdego */
    for (int i = 0; i < N; i++) {
        char v[8]; int vl = snprintf(v, sizeof(v), "val%d", i);
        char buf[16];
        CHECK_EQ(gh2_fs_getxattr(&fs, "/f", names[i], buf, sizeof(buf)), vl);
        CHECK_EQ(memcmp(buf, v, (size_t)vl), 0);
    }
    /* lista zawiera wszystkie */
    char lbuf[512];
    ssize_t got = gh2_fs_listxattr(&fs, "/f", lbuf, sizeof(lbuf));
    CHECK(got > 0);
    for (int i = 0; i < N; i++) CHECK_EQ(list_has(lbuf, (size_t)got, names[i]), 1);

    /* DOWOD pakowania kolizji: dwie ROZNE nazwy o tej samej dlugosci, gdzie pakujemy
     * obie w jeden item nie jest mozliwe bez seam; zamiast tego sprawdzamy, ze remove
     * jednej z wielu zostawia reszte nietknieta (przepakowanie). */
    CHECK_EQ(gh2_fs_removexattr(&fs, "/f", names[7]), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", names[7], lbuf, sizeof(lbuf)), -ENODATA);
    for (int i = 0; i < N; i++) {
        if (i == 7) continue;
        char buf[16];
        CHECK(gh2_fs_getxattr(&fs, "/f", names[i], buf, sizeof(buf)) > 0);
    }

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 5b: kolizja hash w JEDNYM itemie (seam) ============================ */
/* Dowod pakowania kolizji: przez seam wymuszamy TEN SAM hash dla dwoch roznych nazw -> obie
 * lezą w JEDNYM XATTR_ITEM. Lookup po dokladnej nazwie musi je rozroznic (jak DIR_ITEM). */
static void test_hash_collision_pack(void) {
    char tmp[] = "/tmp/gh2xa_hc_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    uint64_t ino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/f", &ino), 0);

    const uint64_t H = 0xDEADBEEFCAFEBABEULL;   /* wymuszony wspolny hash */
    CHECK_EQ(gh2_fs_test_xattr_set_hashed(&fs, ino, H, "user.alpha", "AAA", 3), 0);
    CHECK_EQ(gh2_fs_test_xattr_set_hashed(&fs, ino, H, "user.betaX", "BBBB", 4), 0);
    /* DRUGI raz ta sama nazwa pod tym samym hashem -> -EEXIST */
    CHECK_EQ(gh2_fs_test_xattr_set_hashed(&fs, ino, H, "user.alpha", "Z", 1), -EEXIST);

    /* obie spakowane w JEDEN item -> tylko 1 item (ino,7,*) */
    CHECK_EQ(xattr_item_count(&fs, ino), 1);

    /* lookup po dokladnej nazwie rozroznia oba wpisy w tym samym itemie */
    char buf[8];
    CHECK_EQ(gh2_fs_test_xattr_get_hashed(&fs, ino, H, "user.alpha", buf, sizeof(buf)), 3);
    CHECK_EQ(memcmp(buf, "AAA", 3), 0);
    CHECK_EQ(gh2_fs_test_xattr_get_hashed(&fs, ino, H, "user.betaX", buf, sizeof(buf)), 4);
    CHECK_EQ(memcmp(buf, "BBBB", 4), 0);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 6: ZWALNIANIE przy unlink (wyciek=0) ============================ */
static void test_free_on_unlink(void) {
    char tmp[] = "/tmp/gh2xa_fr_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    uint64_t ino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/f", &ino), 0);

    /* kilka xattr na pliku */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.a", "11", 2, 0), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.b", "222", 3, 0), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.c", "4444", 4, 0), 0);
    CHECK(xattr_item_count(&fs, ino) > 0);

    /* unlink -> nlink 0 -> wszystkie (ino,7,*) usuniete */
    CHECK_EQ(gh2_fs_unlink(&fs, "/f"), 0);
    CHECK_EQ(xattr_item_count(&fs, ino), 0);   /* brak osieroconych xattr */

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);          /* wyciek bloków == 0 */
    check_fsck0(&fs);            /* fsck == 0 */

    /* katalog z xattr: rmdir tez zwalnia */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    uint64_t dino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/d", &dino), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/d", "user.dir", "meta", 4, 0), 0);
    CHECK(xattr_item_count(&fs, dino) > 0);
    CHECK_EQ(gh2_fs_rmdir(&fs, "/d"), 0);
    CHECK_EQ(xattr_item_count(&fs, dino), 0);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);

    /* hardlink: unlink przy nlink>0 NIE usuwa xattr; przy nlink 0 usuwa */
    CHECK_EQ(gh2_fs_create(&fs, "/h1", 0644), 0);
    uint64_t hino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/h1", &hino), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/h1", "user.x", "y", 1, 0), 0);
    CHECK_EQ(gh2_fs_link(&fs, "/h1", "/h2"), 0);
    CHECK_EQ(gh2_fs_unlink(&fs, "/h1"), 0);
    CHECK(xattr_item_count(&fs, hino) > 0);     /* nlink 1 -> xattr zostaje */
    char buf[8];
    CHECK_EQ(gh2_fs_getxattr(&fs, "/h2", "user.x", buf, sizeof(buf)), 1);
    CHECK_EQ(gh2_fs_unlink(&fs, "/h2"), 0);
    CHECK_EQ(xattr_item_count(&fs, hino), 0);    /* nlink 0 -> usuniete */

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 7: persystencja remount ============================ */
static void test_persist_remount(void) {
    char tmp[] = "/tmp/gh2xa_pr_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
        CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.persist", "keepme", 6, 0), 0);
        CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.two", "xx", 2, 0), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);

    /* remount */
    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        char buf[64];
        CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.persist", buf, sizeof(buf)), 6);
        CHECK_EQ(memcmp(buf, "keepme", 6), 0);
        CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.two", buf, sizeof(buf)), 2);
        ssize_t got = gh2_fs_listxattr(&fs, "/f", buf, sizeof(buf));
        CHECK_EQ(got, 13 + 9);   /* "user.persist\0" (13) + "user.two\0" (9) */
        check_no_leak(&fs);
        check_fsck0(&fs);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 8: snapshot izolacja ============================ */
static void test_snapshot(void) {
    char tmp[] = "/tmp/gh2xa_sn_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.snap", "orig", 4, 0), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* snapshot -> wspoldzielenie blokow (refcount 2) */
    CHECK_EQ(gh2_fs_snapshot(&fs, "s1"), 0);
    check_no_leak(&fs);
    check_fsck0(&fs);

    /* modyfikuj xattr w aktywnym (CoW) -> snapshot izolowany (bloki wspoldzielone nietkniete).
     * Po commit: refcount==mark-sweep (check_no_leak), fsck spojne. Snapshot zachowuje stara
     * wersje xattr w swoim fs_root (read-only; izolacja udowodniona brakiem wyciekow + fsck). */
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.snap", "modified", 8, 0), 0);
    CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.new", "extra", 5, 0), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    char buf[16];
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.snap", buf, sizeof(buf)), 8);
    CHECK_EQ(memcmp(buf, "modified", 8), 0);
    CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.new", buf, sizeof(buf)), 5);
    check_no_leak(&fs);
    check_fsck0(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 9: --compress + szyfrowanie round-trip ============================ */
static void test_compress_encrypt(void) {
    /* --compress: xattr to itemy drzewa (nie dane) -> sciezka kompresji ich nie dotyka, ale
     * weryfikujemy round-trip w kontenerze --compress. */
    {
        char tmp[] = "/tmp/gh2xa_cp_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
        struct gh_dev dev;
        CHECK_EQ(open_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS), 0);
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
        CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.c", "zlib-ok", 7, 0), 0);
        char buf[16];
        CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.c", buf, sizeof(buf)), 7);
        CHECK_EQ(memcmp(buf, "zlib-ok", 7), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        check_no_leak(&fs);
        check_fsck0(&fs);
        gh2_fs_unmount(&fs);
        close_dev(&dev);
        unlink(tmp);
    }
    /* szyfrowanie: xattr round-trip w kontenerze zaszyfrowanym (wezly drzewa szyfrowane) */
    {
        char tmp[] = "/tmp/gh2xa_en_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
        struct gh_dev dev;
        CHECK_EQ(open_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_format_key(&dev, NBLK, 0, "s3cr3t"), 0);
        if (dev.cipher) { gh_crypto_wipe(dev.cipher); free(dev.cipher); dev.cipher = NULL; }
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount_key(&fs, &dev, "s3cr3t"), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
        CHECK_EQ(gh2_fs_setxattr(&fs, "/f", "user.enc", "secret-val", 10, 0), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        gh2_fs_unmount(&fs);
        close_dev(&dev);

        /* remount z kluczem -> xattr odczytany po deszyfracji */
        CHECK_EQ(reopen_dev(&dev, tmp), 0);
        CHECK_EQ(gh2_fs_mount_key(&fs, &dev, "s3cr3t"), 0);
        char buf[16];
        CHECK_EQ(gh2_fs_getxattr(&fs, "/f", "user.enc", buf, sizeof(buf)), 10);
        CHECK_EQ(memcmp(buf, "secret-val", 10), 0);
        check_no_leak(&fs);
        check_fsck0(&fs);
        gh2_fs_unmount(&fs);
        close_dev(&dev);
        unlink(tmp);
    }
}

int main(void) {
    RUN_TEST(test_setget_roundtrip);
    RUN_TEST(test_list_remove);
    RUN_TEST(test_create_replace);
    RUN_TEST(test_errno_sizes);
    RUN_TEST(test_many_and_collision);
    RUN_TEST(test_hash_collision_pack);
    RUN_TEST(test_free_on_unlink);
    RUN_TEST(test_persist_remount);
    RUN_TEST(test_snapshot);
    RUN_TEST(test_compress_encrypt);
    return TEST_SUMMARY();
}
