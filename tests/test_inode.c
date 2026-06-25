#include "test.h"
#include "../src/block.h"
#include "../src/super.h"
#include "../src/inode.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void mount(const char *p, struct gh_dev *dev, struct gh_superblock *sb) {
    CHECK_EQ(gh_dev_open(p, dev), 0);
    CHECK_EQ(gh_mount_sb(dev, sb), 0);
}

static void test_small_file(void) {
    char tmp[] = "/tmp/ghost_inoXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 256, 64), 0);
    struct gh_dev dev; struct gh_superblock sb; mount(tmp, &dev, &sb);

    uint64_t ino; CHECK_EQ(gh_inode_alloc(&dev, &sb, GH_FILE, &ino), 0);
    struct gh_inode node; CHECK_EQ(gh_inode_read(&dev, &sb, ino, &node), 0);

    const char *msg = "hello ghost";
    ssize_t w = gh_inode_pwrite(&dev, &sb, ino, &node, msg, strlen(msg), 0);
    CHECK_EQ(w, (ssize_t)strlen(msg));
    CHECK_EQ(node.size, strlen(msg));

    char buf[64] = {0};
    ssize_t r = gh_inode_pread(&dev, &sb, &node, buf, sizeof(buf), 0);
    CHECK_EQ(r, (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_large_file_indirect(void) {
    char tmp[] = "/tmp/ghost_ino2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 64), 0);   /* duzo blokow danych */
    struct gh_dev dev; struct gh_superblock sb; mount(tmp, &dev, &sb);

    uint64_t ino; gh_inode_alloc(&dev, &sb, GH_FILE, &ino);
    struct gh_inode node; gh_inode_read(&dev, &sb, ino, &node);

    /* 20 blokow => przekracza NDIRECT(12), wchodzi w indirect */
    size_t sz = 20 * GH_BLOCK_SIZE;
    char *src = malloc(sz);
    for (size_t i = 0; i < sz; i++) src[i] = (char)(i * 7 + 3);
    CHECK_EQ(gh_inode_pwrite(&dev, &sb, ino, &node, src, sz, 0), (ssize_t)sz);

    char *dst = malloc(sz); memset(dst, 0, sz);
    CHECK_EQ(gh_inode_pread(&dev, &sb, &node, dst, sz, 0), (ssize_t)sz);
    CHECK_EQ(memcmp(src, dst, sz), 0);

    /* edycja w miejscu: nadpisz 4 bajty w srodku */
    char patch[4] = {1,2,3,4};
    CHECK_EQ(gh_inode_pwrite(&dev, &sb, ino, &node, patch, 4, 10000), 4);
    char check[4];
    CHECK_EQ(gh_inode_pread(&dev, &sb, &node, check, 4, 10000), 4);
    CHECK_EQ(memcmp(patch, check, 4), 0);

    /* weryfikacja otoczenia patcha: 16 bajtow od 9996 */
    char surr[16];
    CHECK_EQ(gh_inode_pread(&dev, &sb, &node, surr, 16, 9996), 16);
    /* 4 bajty przed patchem (9996..9999) nie zmienione */
    CHECK_EQ(memcmp(surr, src + 9996, 4), 0);
    /* 4 bajty po patchu (10004..10007) nie zmienione */
    CHECK_EQ(memcmp(surr + 8, src + 10004, 4), 0);

    free(src); free(dst);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_double_indirect(void) {
    char tmp[] = "/tmp/ghost_ino3XXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 8192, 64), 0);
    struct gh_dev dev; struct gh_superblock sb; mount(tmp, &dev, &sb);

    uint64_t ino; CHECK_EQ(gh_inode_alloc(&dev, &sb, GH_FILE, &ino), 0);
    struct gh_inode node; CHECK_EQ(gh_inode_read(&dev, &sb, ino, &node), 0);

    /* 530 blokow > 12 + 512 = 524, wchodzi w double-indirect */
    size_t sz = 530 * GH_BLOCK_SIZE;
    char *src = malloc(sz);
    for (size_t i = 0; i < sz; i++) src[i] = (char)(i * 13 + 5);
    CHECK_EQ(gh_inode_pwrite(&dev, &sb, ino, &node, src, sz, 0), (ssize_t)sz);

    char *dst = malloc(sz); memset(dst, 0, sz);
    CHECK_EQ(gh_inode_pread(&dev, &sb, &node, dst, sz, 0), (ssize_t)sz);
    CHECK_EQ(memcmp(src, dst, sz), 0);

    CHECK_EQ(gh_inode_free(&dev, &sb, ino), 0);

    free(src); free(dst);
    gh_dev_close(&dev); unlink(tmp);
}

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

int main(void) {
    RUN_TEST(test_small_file);
    RUN_TEST(test_large_file_indirect);
    RUN_TEST(test_double_indirect);
    RUN_TEST(test_truncate);
    return TEST_SUMMARY();
}
