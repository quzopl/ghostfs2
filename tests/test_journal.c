#include "test.h"
#include "../src/block.h"
#include "../src/super.h"
#include "../src/journal.h"
#include "../src/ghostfs.h"
#include "../src/csum.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

/* niskopoziomowy odczyt bloku z pominieciem transakcji */
static void raw_read(struct gh_dev *dev, uint64_t b, void *buf) {
    pread(dev->fd, buf, GH_BLOCK_SIZE, (off_t)b * GH_BLOCK_SIZE);
}

static void test_commit_persists(void) {
    char tmp[] = "/tmp/ghost_jcXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    CHECK_EQ(gh_dev_open(tmp, &dev), 0); CHECK_EQ(gh_mount_sb(&dev, &sb), 0);

    /* wolny blok danych (data_start zajmuje korzen katalogu po formacie) */
    uint64_t tgt = sb.data_start + 5;
    CHECK_EQ(gh_jrnl_open(&dev, &sb), 0);
    CHECK(dev.txn != NULL);
    gh_jrnl_op_begin(&dev);
    char w[GH_BLOCK_SIZE]; memset(w, 0x5A, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, tgt, w), 0);
    gh_jrnl_op_commit(&dev);

    /* przed flush: na dysku zera (raw) */
    char d[GH_BLOCK_SIZE]; raw_read(&dev, tgt, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);

    CHECK_EQ(gh_jrnl_flush(&dev, &sb), 0);
    /* po flush: blok na dysku ma dane */
    raw_read(&dev, tgt, d);
    CHECK_EQ(memcmp(d, w, GH_BLOCK_SIZE), 0);
    /* naglowek dziennika wyczyszczony (committed=0) */
    char h[GH_BLOCK_SIZE]; raw_read(&dev, sb.journal_start, h);
    struct gh_jheader jh; memcpy(&jh, h, sizeof(jh));
    CHECK_EQ(jh.committed, 0u);

    gh_jrnl_close(&dev);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_abort_discards(void) {
    char tmp[] = "/tmp/ghost_jaXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t tgt = sb.data_start + 5;
    CHECK_EQ(gh_jrnl_open(&dev, &sb), 0);
    gh_jrnl_op_begin(&dev);
    char w[GH_BLOCK_SIZE]; memset(w, 0x11, sizeof(w));
    CHECK_EQ(gh_block_write(&dev, tgt, w), 0);
    gh_jrnl_op_rollback(&dev);
    CHECK(dev.txn != NULL);
    /* bufor wrocil: kolejny flush nic nie pisze (dirty==0) */
    CHECK_EQ(gh_jrnl_flush(&dev, &sb), 0);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, tgt, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* nic nie zapisane */
    gh_jrnl_close(&dev);
    gh_dev_close(&dev); unlink(tmp);
}

/* operacja A (write blok X, op_commit), operacja B (write blok X innym wzorcem +
   write nowy blok Y, op_rollback) -> po flush X ma wzorzec A, Y nieobecny (zera) */
static void test_op_rollback_in_batch(void) {
    char tmp[] = "/tmp/ghost_jbXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t X = sb.data_start + 5;
    uint64_t Y = sb.data_start + 6;
    CHECK_EQ(gh_jrnl_open(&dev, &sb), 0);

    /* operacja A: zapisz wzorzec A do X, zatwierdz operacje */
    char a[GH_BLOCK_SIZE]; memset(a, 0xAA, sizeof(a));
    gh_jrnl_op_begin(&dev);
    CHECK_EQ(gh_block_write(&dev, X, a), 0);
    gh_jrnl_op_commit(&dev);

    /* operacja B: nadpisz X wzorcem B + zapisz nowy blok Y, wycofaj operacje */
    char bb[GH_BLOCK_SIZE]; memset(bb, 0xBB, sizeof(bb));
    gh_jrnl_op_begin(&dev);
    CHECK_EQ(gh_block_write(&dev, X, bb), 0);
    CHECK_EQ(gh_block_write(&dev, Y, bb), 0);
    gh_jrnl_op_rollback(&dev);

    /* read-your-writes w buforze: X==A, Y nieobecny -> z dysku (zera) */
    char rb[GH_BLOCK_SIZE];
    CHECK_EQ(gh_block_read(&dev, X, rb), 0);
    CHECK_EQ(memcmp(rb, a, GH_BLOCK_SIZE), 0);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(gh_block_read(&dev, Y, rb), 0);
    CHECK_EQ(memcmp(rb, zero, GH_BLOCK_SIZE), 0);

    CHECK_EQ(gh_jrnl_flush(&dev, &sb), 0);
    /* po flush na dysku: X ma wzorzec A, Y zera */
    raw_read(&dev, X, rb);
    CHECK_EQ(memcmp(rb, a, GH_BLOCK_SIZE), 0);
    raw_read(&dev, Y, rb);
    CHECK_EQ(memcmp(rb, zero, GH_BLOCK_SIZE), 0);

    gh_jrnl_close(&dev);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_recover_redo(void) {
    char tmp[] = "/tmp/ghost_jrXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    /* spreparuj zatwierdzona transakcje recznie: 1 blok docelowy = data_start */
    uint64_t js = sb.journal_start;
    uint64_t target = sb.data_start + 3;
    char img[GH_BLOCK_SIZE]; memset(img, 0xA7, sizeof(img));
    /* deskryptor: blok js+1, pierwszy uint64 = target */
    char db[GH_BLOCK_SIZE]; memset(db, 0, sizeof(db)); memcpy(db, &target, 8);
    pwrite(dev.fd, db, GH_BLOCK_SIZE, (off_t)(js + 1) * GH_BLOCK_SIZE);
    /* obraz: blok js+2 */
    pwrite(dev.fd, img, GH_BLOCK_SIZE, (off_t)(js + 2) * GH_BLOCK_SIZE);
    /* naglowek committed=1 */
    struct gh_jheader jh; memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.seq = 1; jh.committed = 1;
    jh.n_blocks = 1; jh.descriptor_blocks = 1;
    { uint64_t bn[1] = { target };
      uint32_t c = 0xFFFFFFFFu;
      c = gh_crc32_update(c, bn, sizeof(bn));
      c = gh_crc32_update(c, img, GH_BLOCK_SIZE);
      jh.csum = c ^ 0xFFFFFFFFu; }
    char hb[GH_BLOCK_SIZE]; memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    /* recover -> target ma obraz, naglowek wyczyszczony */
    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, target, d);
    CHECK_EQ(memcmp(d, img, GH_BLOCK_SIZE), 0);
    raw_read(&dev, js, hb); memcpy(&jh, hb, sizeof(jh));
    CHECK_EQ(jh.committed, 0u);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_recover_uncommitted_noop(void) {
    char tmp[] = "/tmp/ghost_juXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);
    uint64_t js = sb.journal_start, target = sb.data_start + 1;
    char img[GH_BLOCK_SIZE]; memset(img, 0x44, sizeof(img));
    char db[GH_BLOCK_SIZE]; memset(db, 0, sizeof(db)); memcpy(db, &target, 8);
    pwrite(dev.fd, db, GH_BLOCK_SIZE, (off_t)(js + 1) * GH_BLOCK_SIZE);
    pwrite(dev.fd, img, GH_BLOCK_SIZE, (off_t)(js + 2) * GH_BLOCK_SIZE);
    struct gh_jheader jh; memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.committed = 0;   /* NIE zatwierdzone */
    jh.n_blocks = 1; jh.descriptor_blocks = 1;
    char hb[GH_BLOCK_SIZE]; memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, target, d);
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* nic nie odtworzone */
    gh_dev_close(&dev); unlink(tmp);
}

static void test_recover_inconsistent_noop(void) {
    char tmp[] = "/tmp/ghost_jiXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);
    uint64_t js = sb.journal_start;

    /* zapisz znana zawartosc do kilku blokow danych (data_start+1..+5) */
    char known[5][GH_BLOCK_SIZE];
    for (int i = 0; i < 5; i++) {
        memset(known[i], 0x70 + i, GH_BLOCK_SIZE);
        pwrite(dev.fd, known[i], GH_BLOCK_SIZE,
               (off_t)(sb.data_start + 1 + i) * GH_BLOCK_SIZE);
    }
    /* migawka calego urzadzenia (poza naglowkiem dziennika, ktory zmieniamy) */
    char *before = malloc((size_t)sb.total_blocks * GH_BLOCK_SIZE);
    CHECK(before != NULL);
    for (uint64_t b = 0; b < sb.total_blocks; b++)
        raw_read(&dev, b, before + b * GH_BLOCK_SIZE);

    /* spreparuj NIESPOJNY naglowek: committed=1, n_blocks=20, descriptor_blocks=0
       -> deskryptor nie pokrywa zadnych obrazow, targets[] niezainicjowane;
       przed fixem recover pisze obrazy pod losowe (garbage) numery blokow */
    struct gh_jheader jh; memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.seq = 1; jh.committed = 1;
    jh.n_blocks = 20; jh.descriptor_blocks = 0;
    char hb[GH_BLOCK_SIZE]; memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    /* recover musi potraktowac niespojny dziennik jak brak transakcji */
    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);

    /* znane bloki nienaruszone */
    char d[GH_BLOCK_SIZE];
    for (int i = 0; i < 5; i++) {
        raw_read(&dev, sb.data_start + 1 + i, d);
        CHECK_EQ(memcmp(d, known[i], GH_BLOCK_SIZE), 0);
    }
    /* zaden blok poza naglowkiem dziennika nie zostal zmieniony */
    for (uint64_t b = 0; b < sb.total_blocks; b++) {
        if (b == js) continue;   /* naglowek dziennika moze byc wyczyszczony */
        raw_read(&dev, b, d);
        CHECK_EQ(memcmp(d, before + b * GH_BLOCK_SIZE, GH_BLOCK_SIZE), 0);
    }
    free(before);
    gh_dev_close(&dev); unlink(tmp);
}

/* CRC dziennika: poprawny csum -> recover odtwarza; bledny csum (uszkodzony obraz) ->
   recover NIE odtwarza (rozdarty zapis) */
static void test_journal_csum(void) {
    char tmp[] = "/tmp/ghost_jzXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);
    uint64_t js = sb.journal_start;
    uint64_t target = sb.data_start + 7;

    /* 1-blokowa transakcja: deskryptor (js+1) z target, obraz (js+2) */
    char img[GH_BLOCK_SIZE]; memset(img, 0x3C, sizeof(img));
    char db[GH_BLOCK_SIZE]; memset(db, 0, sizeof(db)); memcpy(db, &target, 8);
    pwrite(dev.fd, db, GH_BLOCK_SIZE, (off_t)(js + 1) * GH_BLOCK_SIZE);
    pwrite(dev.fd, img, GH_BLOCK_SIZE, (off_t)(js + 2) * GH_BLOCK_SIZE);

    /* poprawny csum policzony jak we flush: uporzadkowany strumien blknos+image */
    uint64_t blknos[1] = { target };
    uint32_t good;
    { uint32_t c = 0xFFFFFFFFu;
      c = gh_crc32_update(c, blknos, sizeof(blknos));
      c = gh_crc32_update(c, img, GH_BLOCK_SIZE);
      good = c ^ 0xFFFFFFFFu; }

    struct gh_jheader jh; memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.seq = 1; jh.committed = 1;
    jh.n_blocks = 1; jh.descriptor_blocks = 1; jh.csum = good;
    char hb[GH_BLOCK_SIZE]; memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    /* poprawny csum -> recover odtwarza */
    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);
    char d[GH_BLOCK_SIZE]; raw_read(&dev, target, d);
    CHECK_EQ(memcmp(d, img, GH_BLOCK_SIZE), 0);

    /* teraz rozdarty zapis: uszkodz obraz w regionie dziennika, ALE zostaw stary csum,
       committed=1 -> recover wykrywa niezgodnosc i NIE odtwarza */
    char target2 = (char)0x9E;
    char img2[GH_BLOCK_SIZE]; memset(img2, target2, sizeof(img2));
    uint64_t tgt2 = sb.data_start + 8;
    char db2[GH_BLOCK_SIZE]; memset(db2, 0, sizeof(db2)); memcpy(db2, &tgt2, 8);
    pwrite(dev.fd, db2, GH_BLOCK_SIZE, (off_t)(js + 1) * GH_BLOCK_SIZE);
    pwrite(dev.fd, img2, GH_BLOCK_SIZE, (off_t)(js + 2) * GH_BLOCK_SIZE);
    /* csum w naglowku celowo zly (z poprzedniej transakcji - good) */
    memset(&jh, 0, sizeof(jh));
    memcpy(jh.magic, GH_JMAGIC, 8); jh.seq = 1; jh.committed = 1;
    jh.n_blocks = 1; jh.descriptor_blocks = 1; jh.csum = good;  /* nie pasuje do img2/tgt2 */
    memset(hb, 0, sizeof(hb)); memcpy(hb, &jh, sizeof(jh));
    pwrite(dev.fd, hb, GH_BLOCK_SIZE, (off_t)js * GH_BLOCK_SIZE);

    /* cel tgt2 powinien byc zerowy przed recoverem (nigdy nie zapisany) */
    char zero[GH_BLOCK_SIZE]; memset(zero, 0, sizeof(zero));
    raw_read(&dev, tgt2, d);
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);

    CHECK_EQ(gh_jrnl_recover(&dev, &sb), 0);
    raw_read(&dev, tgt2, d);
    CHECK_EQ(memcmp(d, zero, GH_BLOCK_SIZE), 0);   /* rozdarcie wykryte -> NIE odtworzone */

    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_commit_persists);
    RUN_TEST(test_abort_discards);
    RUN_TEST(test_op_rollback_in_batch);
    RUN_TEST(test_recover_redo);
    RUN_TEST(test_recover_uncommitted_noop);
    RUN_TEST(test_recover_inconsistent_noop);
    RUN_TEST(test_journal_csum);
    return TEST_SUMMARY();
}
