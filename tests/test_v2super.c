#include "test.h"
#include "v2/gh2_super.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* surowy zapis bloku (do symulacji rozdarcia/korupcji slotu) */
static void smash_slot(struct gh_dev *dev, int slot) {
    uint8_t junk[GH2_BLOCK_SIZE];
    memset(junk, 0xAB, sizeof(junk));
    pwrite(dev->fd, junk, GH2_BLOCK_SIZE, (off_t)slot * GH2_BLOCK_SIZE);
    fsync(dev->fd);
}
static void flip_bit_slot(struct gh_dev *dev, int slot) {
    uint8_t blk[GH2_BLOCK_SIZE];
    pread(dev->fd, blk, GH2_BLOCK_SIZE, (off_t)slot * GH2_BLOCK_SIZE);
    blk[64] ^= 0x01;   /* przekręć bit w środku superbloku */
    pwrite(dev->fd, blk, GH2_BLOCK_SIZE, (off_t)slot * GH2_BLOCK_SIZE);
    fsync(dev->fd);
}

static void test_v2_format_mount(void) {
    char tmp[] = "/tmp/ghost_v2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev;
    CHECK_EQ(gh_dev_create(tmp, 1024, &dev), 0);
    CHECK_EQ(gh2_format(&dev, 1024, 0), 0);
    struct gh2_superblock sb;
    CHECK_EQ(gh2_mount(&dev, &sb), 0);
    CHECK_EQ(memcmp(sb.magic, GH2_MAGIC, GH2_MAGIC_LEN), 0);
    CHECK_EQ(sb.generation, 1u);
    CHECK_EQ(sb.total_blocks, 1024u);
    CHECK_EQ(sb.block_size, GH2_BLOCK_SIZE);
    CHECK_EQ(sb.next_free, GH2_DATA_START);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_v2_commit_pingpong(void) {
    char tmp[] = "/tmp/ghost_v2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev;
    CHECK_EQ(gh_dev_create(tmp, 1024, &dev), 0);
    CHECK_EQ(gh2_format(&dev, 1024, 0), 0);
    struct gh2_superblock sb;
    CHECK_EQ(gh2_mount(&dev, &sb), 0);
    /* kilka commitow: generacja rosnie, remount widzi najnowsza */
    for (uint64_t g = 2; g <= 6; g++) {
        sb.next_free += 1;                 /* jakas zmiana stanu */
        CHECK_EQ(gh2_commit_super(&dev, &sb), 0);
        CHECK_EQ(sb.generation, g);
        struct gh2_superblock m;
        CHECK_EQ(gh2_mount(&dev, &m), 0);
        CHECK_EQ(m.generation, g);
        CHECK_EQ(m.next_free, sb.next_free);
    }
    gh_dev_close(&dev); unlink(tmp);
}

/* BRAMKA: awaria przy zapisie SB (slot gen N) -> mount wybiera gen N-1 z drugiego slotu */
static void test_v2_crash_fallback(void) {
    char tmp[] = "/tmp/ghost_v2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev;
    CHECK_EQ(gh_dev_create(tmp, 1024, &dev), 0);
    CHECK_EQ(gh2_format(&dev, 1024, 0), 0);
    struct gh2_superblock sb; CHECK_EQ(gh2_mount(&dev, &sb), 0);
    /* commit do gen 2 (slot 0) i gen 3 (slot 1) -> oba sloty wazne, rozne generacje */
    sb.next_free = 100; CHECK_EQ(gh2_commit_super(&dev, &sb), 0);  /* gen2 slot0 */
    sb.next_free = 200; CHECK_EQ(gh2_commit_super(&dev, &sb), 0);  /* gen3 slot1 */
    CHECK_EQ(sb.generation, 3u);
    /* symuluj rozdarty zapis NASTEPNEGO commitu: gen4 trafia w slot0, ale rozdarty */
    smash_slot(&dev, 0);                    /* slot0 (gen2) -> smieci */
    struct gh2_superblock m; CHECK_EQ(gh2_mount(&dev, &m), 0);
    CHECK_EQ(m.generation, 3u);             /* wybrano gen3 ze slotu1 (spojny) */
    CHECK_EQ(m.next_free, 200u);
    gh_dev_close(&dev); unlink(tmp);
}

/* csum: przekrecony bit -> slot odrzucony; oba zepsute -> -EINVAL */
static void test_v2_csum_reject(void) {
    char tmp[] = "/tmp/ghost_v2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev;
    CHECK_EQ(gh_dev_create(tmp, 1024, &dev), 0);
    CHECK_EQ(gh2_format(&dev, 1024, 0), 0);   /* oba sloty gen1 */
    flip_bit_slot(&dev, 0);                    /* slot0 zepsuty (csum != ) */
    struct gh2_superblock m; CHECK_EQ(gh2_mount(&dev, &m), 0);
    CHECK_EQ(m.generation, 1u);                /* slot1 nadal wazny */
    flip_bit_slot(&dev, 1);                     /* teraz oba zepsute */
    CHECK_EQ(gh2_mount(&dev, &m), -EINVAL);
    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_v2_format_mount);
    RUN_TEST(test_v2_commit_pingpong);
    RUN_TEST(test_v2_crash_fallback);
    RUN_TEST(test_v2_csum_reject);
    return TEST_SUMMARY();
}
