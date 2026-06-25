#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_vfs.h"
#include "super.h"
#include "v2/gh2_super.h"
#include "v2/gh2_format.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * ghostfs v2.6 — testy fasady gfs (gh2_vfs): ten sam zestaw operacji przez fasade
 * na kontenerze v1 ORAZ v2. Format obu (v1: gh_format; v2: gh_dev_create+gh2_fs_format),
 * detect, mount, create/mkdir/write/read (bajt-exact)/getattr/readdir/unlink/sync/
 * remount/fsck==0.
 * ========================================================================== */

static const uint64_t NBLK = 16384;

/* format v1 (przez publiczne API gh_format). */
static void fmt_v1(const char *path) {
    int r = gh_format(path, NBLK, 512);
    CHECK_EQ(r, 0);
}

/* format v2 (gh_dev_create -> gh2_fs_format). */
static void fmt_v2(const char *path) {
    struct gh_dev dev;
    int r = gh_dev_create(path, NBLK, &dev);
    CHECK_EQ(r, 0);
    r = gh_bcache_create(&dev);
    CHECK_EQ(r, 0);
    r = gh2_fs_format(&dev, NBLK, 0);
    CHECK_EQ(r, 0);
    gh_bcache_destroy(&dev);
    gh_dev_close(&dev);
}

/* ---- readdir: zlicz wpisy (pomin . / ..) i znajdz nazwe ---- */
struct rd { const char *want; int found; int n; };
static int rd_cb(const char *name, uint16_t type, void *ctx) {
    (void)type;
    struct rd *r = ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    r->n++;
    if (r->want && !strcmp(name, r->want)) r->found = 1;
    return 0;
}

/* ten sam scenariusz na obu backendach (version w komunikatach przez plik). */
static void run_suite(const char *path, int expect_ver) {
    int v = gfs_detect(path);
    CHECK_EQ(v, expect_ver);

    struct gfs g;
    int r = gfs_mount(&g, path, NULL);
    CHECK_EQ(r, 0);
    if (r) return;
    CHECK_EQ(g.version, expect_ver);

    /* mkdir + getattr typu */
    CHECK_EQ(gfs_mkdir(&g, "/dir", 0755), 0);
    struct gfs_attr a;
    CHECK_EQ(gfs_getattr(&g, "/dir", &a), 0);
    CHECK_EQ(a.type, 2 /*DIR*/);

    /* create + write bajt-exact + read */
    CHECK_EQ(gfs_create(&g, "/dir/f.bin", 0644), 0);
    static uint8_t wbuf[20000], rbuf[20000];
    for (size_t i = 0; i < sizeof(wbuf); i++) wbuf[i] = (uint8_t)(i * 31 + 7);
    ssize_t w = gfs_write(&g, "/dir/f.bin", wbuf, sizeof(wbuf), 0);
    CHECK_EQ(w, (ssize_t)sizeof(wbuf));
    CHECK_EQ(gfs_getattr(&g, "/dir/f.bin", &a), 0);
    CHECK_EQ(a.type, 1 /*FILE*/);
    CHECK_EQ(a.size, sizeof(wbuf));
    memset(rbuf, 0, sizeof(rbuf));
    ssize_t rd = gfs_read(&g, "/dir/f.bin", rbuf, sizeof(rbuf), 0);
    CHECK_EQ(rd, (ssize_t)sizeof(rbuf));
    CHECK_EQ(memcmp(wbuf, rbuf, sizeof(wbuf)), 0);

    /* readdir /dir -> f.bin obecny */
    struct rd rc = { "f.bin", 0, 0 };
    CHECK_EQ(gfs_readdir(&g, "/dir", rd_cb, &rc), 0);
    CHECK_EQ(rc.found, 1);
    CHECK_EQ(rc.n, 1);

    /* chmod */
    CHECK_EQ(gfs_chmod(&g, "/dir/f.bin", 0600), 0);
    CHECK_EQ(gfs_getattr(&g, "/dir/f.bin", &a), 0);
    CHECK_EQ(a.mode & 0777, 0600);

    /* symlink + readlink */
    CHECK_EQ(gfs_symlink(&g, "/dir/f.bin", "/lnk"), 0);
    char lbuf[256];
    ssize_t ll = gfs_readlink(&g, "/lnk", lbuf, sizeof(lbuf));
    CHECK(ll > 0);
    CHECK_EQ(strcmp(lbuf, "/dir/f.bin"), 0);

    /* statfs */
    struct gfs_statfs sf;
    CHECK_EQ(gfs_statfs(&g, &sf), 0);
    CHECK_EQ(sf.block_size, 4096);
    CHECK(sf.total_blocks > 0);

    /* sync (trwalosc) */
    CHECK_EQ(gfs_sync(&g), 0);
    gfs_unmount(&g);

    /* remount -> dane przetrwaly */
    r = gfs_mount(&g, path, NULL);
    CHECK_EQ(r, 0);
    if (r) return;
    memset(rbuf, 0, sizeof(rbuf));
    rd = gfs_read(&g, "/dir/f.bin", rbuf, sizeof(rbuf), 0);
    CHECK_EQ(rd, (ssize_t)sizeof(rbuf));
    CHECK_EQ(memcmp(wbuf, rbuf, sizeof(wbuf)), 0);

    /* unlink + fsck czysty */
    CHECK_EQ(gfs_unlink(&g, "/dir/f.bin"), 0);
    CHECK_EQ(gfs_unlink(&g, "/lnk"), 0);
    CHECK_EQ(gfs_rmdir(&g, "/dir"), 0);
    CHECK_EQ(gfs_sync(&g), 0);
    int issues = -1;
    CHECK_EQ(gfs_fsck(&g, 0, &issues), 0);
    CHECK_EQ(issues, 0);
    /* v2 fsck jest read-only: repair=1 nie wywala, dalej waliduje (issues==0). */
    issues = -1;
    CHECK_EQ(gfs_fsck(&g, 1, &issues), 0);
    CHECK_EQ(issues, 0);
    gfs_unmount(&g);
}

static void test_v1_facade(void) {
    char path[] = "/tmp/gfs_vfs_v1.XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    fmt_v1(path);
    run_suite(path, 1);
    unlink(path);
}

static void test_v2_facade(void) {
    char path[] = "/tmp/gfs_vfs_v2.XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    fmt_v2(path);
    run_suite(path, 2);
    unlink(path);
}

static void test_detect_errors(void) {
    /* nieistniejacy plik -> -errno */
    CHECK(gfs_detect("/nonexistent/zzz") < 0);
    /* smieci -> -EINVAL */
    char path[] = "/tmp/gfs_vfs_bad.XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0);
    const char junk[16] = "NOTAGHOSTFSXXXXX";
    CHECK(write(fd, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
    close(fd);
    CHECK_EQ(gfs_detect(path), -EINVAL);
    unlink(path);
}

static void test_xattr_split(void) {
    /* v2: xattr dziala przez fasade (set/get/list/remove round-trip + errno) */
    char p2[] = "/tmp/gfs_vfs_xa2.XXXXXX";
    int fd = mkstemp(p2); CHECK(fd >= 0); close(fd);
    fmt_v2(p2);
    struct gfs g;
    CHECK_EQ(gfs_mount(&g, p2, NULL), 0);
    CHECK_EQ(gfs_create(&g, "/f", 0644), 0);

    /* set/get round-trip */
    CHECK_EQ(gfs_setxattr(&g, "/f", "user.x", "v", 1, 0), 0);
    char vb[16];
    CHECK_EQ((int)gfs_getxattr(&g, "/f", "user.x", vb, sizeof(vb)), 1);
    CHECK_EQ(vb[0], 'v');

    /* brak -> -ENODATA */
    CHECK_EQ((int)gfs_getxattr(&g, "/f", "user.none", vb, sizeof(vb)), -ENODATA);

    /* list zawiera nazwe */
    char lb[64];
    ssize_t ll = gfs_listxattr(&g, "/f", lb, sizeof(lb));
    CHECK_EQ((int)ll, 7);          /* "user.x\0" */
    CHECK_EQ(memcmp(lb, "user.x", 6), 0);

    /* remove -> -ENODATA po usunieciu */
    CHECK_EQ(gfs_removexattr(&g, "/f", "user.x"), 0);
    CHECK_EQ((int)gfs_getxattr(&g, "/f", "user.x", vb, sizeof(vb)), -ENODATA);
    CHECK_EQ(gfs_removexattr(&g, "/f", "user.x"), -ENODATA);

    gfs_unmount(&g);
    unlink(p2);
}

int main(void) {
    RUN_TEST(test_detect_errors);
    RUN_TEST(test_v1_facade);
    RUN_TEST(test_v2_facade);
    RUN_TEST(test_xattr_split);
    return TEST_SUMMARY();
}
