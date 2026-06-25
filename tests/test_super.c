#include "test.h"
#include "../src/block.h"
#include "../src/super.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void test_format_then_mount(void) {
    char tmp[] = "/tmp/ghost_sbXXXXXX";
    int fd = mkstemp(tmp); close(fd);

    CHECK_EQ(gh_format(tmp, 64, 32), 0);

    struct gh_dev dev; CHECK_EQ(gh_dev_open(tmp, &dev), 0);
    struct gh_superblock sb;
    CHECK_EQ(gh_mount_sb(&dev, &sb), 0);
    CHECK_EQ(memcmp(sb.magic, GH_MAGIC, GH_MAGIC_LEN), 0);
    CHECK_EQ(sb.block_size, GH_BLOCK_SIZE);
    CHECK_EQ(sb.total_blocks, 64);
    CHECK_EQ(sb.root_inode, GH_ROOT_INO);
    CHECK(sb.data_start > sb.inode_start);
    CHECK(sb.inode_start > sb.bitmap_start);
    CHECK_EQ(sb.bitmap_start, 1);
    CHECK_EQ(sb.inode_start, 2);
    /* dziennik istnieje i jest miedzy i-wezlami a danymi */
    CHECK(sb.journal_blocks > 0);
    CHECK_EQ(sb.journal_start, sb.inode_start + ((sb.inode_count / GH_INODES_PER_BLK)));
    /* region sum miedzy dziennikiem a danymi */
    CHECK(sb.csum_blocks > 0);
    CHECK_EQ(sb.csum_start, sb.journal_start + sb.journal_blocks);
    CHECK_EQ(sb.data_start, sb.csum_start + sb.csum_blocks);
    gh_dev_close(&dev);
    unlink(tmp);
}

static void test_bad_magic(void) {
    char tmp[] = "/tmp/ghost_sb2XXXXXX";
    int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 8, &dev), 0);    /* same zera, brak magic */
    struct gh_superblock sb;
    CHECK_EQ(gh_mount_sb(&dev, &sb), -EINVAL);
    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_format_then_mount);
    RUN_TEST(test_bad_magic);
    return TEST_SUMMARY();
}
