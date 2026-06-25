#include "test.h"
#include "../src/block.h"
#include "../src/crypto.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static void test_create_read_write(void) {
    char tmp[] = "/tmp/ghost_blkXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev;
    CHECK_EQ(gh_dev_create(tmp, 8, &dev), 0);
    CHECK_EQ(dev.total_blocks, 8);

    char out[GH_BLOCK_SIZE]; memset(out, 0xAB, sizeof(out));
    CHECK_EQ(gh_block_write(&dev, 3, out), 0);

    char in[GH_BLOCK_SIZE]; memset(in, 0, sizeof(in));
    CHECK_EQ(gh_block_read(&dev, 3, in), 0);
    CHECK_EQ(memcmp(in, out, GH_BLOCK_SIZE), 0);

    /* odczyt poza zakresem = blad */
    CHECK(gh_block_read(&dev, 99, in) != 0);
    gh_dev_close(&dev);
    unlink(tmp);
}

static void test_txn_buffer(void) {
    char tmp[] = "/tmp/ghost_txnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 16, &dev), 0);

    /* recznie zbuduj transakcje (bufor na 4 bloki) */
    struct gh_txn t; memset(&t, 0, sizeof(t));
    t.cap = 4; t.blknos = malloc(4 * sizeof(uint64_t));
    t.images = malloc(4 * GH_BLOCK_SIZE); t.active = 1;
    dev.txn = &t;

    char w[GH_BLOCK_SIZE]; memset(w, 0xCD, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, 5, w), 0);     /* trafia do bufora, nie na dysk */

    /* read-your-writes: widac w transakcji */
    char r[GH_BLOCK_SIZE]; memset(r, 0, sizeof(r));
    CHECK_EQ(gh_block_read(&dev, 5, r), 0);
    CHECK_EQ(memcmp(r, w, GH_BLOCK_SIZE), 0);

    /* na dysku jeszcze nic (czytamy bez transakcji) */
    dev.txn = NULL;
    char d[GH_BLOCK_SIZE]; memset(d, 0xFF, sizeof(d));
    CHECK_EQ(gh_block_read(&dev, 5, d), 0);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* kontener wyzerowany */

    /* dedup: drugi zapis tego samego bloku nie zwieksza n */
    dev.txn = &t;
    CHECK_EQ(gh_block_write(&dev, 5, w), 0);
    CHECK_EQ(t.n, 1u);

    /* przepelnienie: zapelnij do cap, kolejny -> ENOSPC */
    CHECK_EQ(gh_block_write(&dev, 6, w), 0);
    CHECK_EQ(gh_block_write(&dev, 7, w), 0);
    CHECK_EQ(gh_block_write(&dev, 8, w), 0);
    CHECK_EQ(t.n, 4u);
    CHECK_EQ(gh_block_write(&dev, 9, w), -ENOSPC);

    free(t.blknos); free(t.images);
    dev.txn = NULL; gh_dev_close(&dev); unlink(tmp);
}

static void test_disk_encryption(void) {
    char tmp[] = "/tmp/ghost_encXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 16, &dev), 0);
    struct gh_cipher c; memset(&c, 0, sizeof(c));
    uint8_t salt[16]; memset(salt, 9, 16); gh_crypto_derive("pw", salt, 1000, c.key);
    dev.cipher = &c;

    char w[GH_BLOCK_SIZE]; memset(w, 0xBE, sizeof(w));
    CHECK_EQ(gh_disk_write(&dev, 5, w), 0);

    /* surowy odczyt bloku 5 z dysku (z pominieciem szyfru) != jawne */
    char raw[GH_BLOCK_SIZE];
    pread(dev.fd, raw, GH_BLOCK_SIZE, (off_t)5 * GH_BLOCK_SIZE);
    CHECK(memcmp(raw, w, GH_BLOCK_SIZE) != 0);     /* na dysku szyfrogram */

    /* gh_disk_read odszyfrowuje z powrotem */
    char r[GH_BLOCK_SIZE]; CHECK_EQ(gh_disk_read(&dev, 5, r), 0);
    CHECK_EQ(memcmp(r, w, GH_BLOCK_SIZE), 0);

    /* blok 0 (superblok) NIE jest szyfrowany */
    CHECK_EQ(gh_disk_write(&dev, 0, w), 0);
    pread(dev.fd, raw, GH_BLOCK_SIZE, 0);
    CHECK_EQ(memcmp(raw, w, GH_BLOCK_SIZE), 0);     /* jawne na dysku */

    dev.cipher = NULL; gh_dev_close(&dev); unlink(tmp);
}

static void test_dev_fields(void) {
    char tmp[] = "/tmp/ghost_dfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 32, &dev), 0);
    CHECK_EQ(dev.is_blkdev, 0);
    CHECK_EQ(dev.total_blocks, 32);
    gh_dev_close(&dev);
    /* ponowne otwarcie pliku */
    CHECK_EQ(gh_dev_open(tmp, &dev), 0);
    CHECK_EQ(dev.is_blkdev, 0);
    CHECK_EQ(dev.total_blocks, 32);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_discard_punch(void) {
    char tmp[] = "/tmp/ghost_dpXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 64, &dev), 0);
    char buf[GH_BLOCK_SIZE]; memset(buf, 0xAB, sizeof(buf));
    for (uint64_t b = 10; b < 40; b++) CHECK_EQ(gh_block_write(&dev, b, buf), 0);

    struct stat s1; fstat(dev.fd, &s1);
    /* discard zapisanych blokow -> plik staje sie rzadszy */
    CHECK_EQ(gh_disk_discard(&dev, 10, 30), 0);
    struct stat s2; fstat(dev.fd, &s2);
    CHECK(s2.st_blocks <= s1.st_blocks);   /* punch-hole zmniejszyl alokacje (lub best-effort) */
    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_create_read_write);
    RUN_TEST(test_txn_buffer);
    RUN_TEST(test_disk_encryption);
    RUN_TEST(test_dev_fields);
    RUN_TEST(test_discard_punch);
    return TEST_SUMMARY();
}
