#include "test.h"
#include "../src/block.h"
#include "../src/alloc.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>

/* superblok pod test: mapa w bloku 1, dane od bloku 2 */
static struct gh_superblock mk_sb(uint64_t total) {
    struct gh_superblock sb = {0};
    sb.block_size = GH_BLOCK_SIZE; sb.total_blocks = total;
    sb.bitmap_start = 1; sb.inode_start = 2; sb.data_start = 4;
    sb.inode_count = GH_INODES_PER_BLK * 2; sb.root_inode = GH_ROOT_INO;
    return sb;
}

static void test_alloc_free(void) {
    char tmp[] = "/tmp/ghost_allocXXXXXX";
    int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; gh_dev_create(tmp, 16, &dev);
    struct gh_superblock sb = mk_sb(16);

    uint64_t a, b;
    CHECK_EQ(gh_alloc_block(&dev, &sb, &a), 0);
    CHECK_EQ(a, sb.data_start);          /* pierwszy wolny = pierwszy blok danych */
    CHECK_EQ(gh_alloc_block(&dev, &sb, &b), 0);
    CHECK_EQ(b, sb.data_start + 1);

    int set = 0;
    CHECK_EQ(gh_bitmap_test(&dev, &sb, a, &set), 0); CHECK_EQ(set, 1);
    CHECK_EQ(gh_free_block(&dev, &sb, a), 0);
    CHECK_EQ(gh_bitmap_test(&dev, &sb, a, &set), 0); CHECK_EQ(set, 0);

    /* po zwolnieniu a, kolejny alloc znow zwraca a */
    uint64_t c; CHECK_EQ(gh_alloc_block(&dev, &sb, &c), 0); CHECK_EQ(c, a);

    gh_dev_close(&dev); unlink(tmp);
}

static void test_enospc(void) {
    char tmp[] = "/tmp/ghost_alloc2XXXXXX";
    int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; gh_dev_create(tmp, 6, &dev);  /* tylko 2 bloki danych: 4,5 */
    struct gh_superblock sb = mk_sb(6);
    uint64_t x;
    CHECK_EQ(gh_alloc_block(&dev, &sb, &x), 0);
    CHECK_EQ(gh_alloc_block(&dev, &sb, &x), 0);
    CHECK_EQ(gh_alloc_block(&dev, &sb, &x), -ENOSPC);
    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_alloc_free);
    RUN_TEST(test_enospc);
    return TEST_SUMMARY();
}
