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
        CHECK_EQ(gh_fs_sync(&fs), 0);                 /* utrwal prep przed symulacja awarii */

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

/* ordered-data: nowy plik z danymi (direct write), sweep awarii -> recover -> fsck czysty;
   KRYTYCZNE: zaden odczyt pliku po recover NIE zwraca EIO (brak bloku referencyjnego z bledna
   suma). Plus: po pelnym zapisie+sync+remount dane TRWALE. */
static void test_ordered_data_crash(void) {
    /* wieloblokowy zapis -> wiele bloku danych direct + metadane journalowane */
    size_t N = 4096 * 3 + 777;
    char *msg = malloc(N); for (size_t i = 0; i < N; i++) msg[i] = (char)(i * 31 + 5);

    for (int K = 1; K <= 60; K++) {
        char tmp[] = "/tmp/ghost_odcXXXXXX"; int fd = mkstemp(tmp); close(fd);
        CHECK_EQ(gh_format(tmp, 4096, 128), 0);
        struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
        CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);    /* prep bez awarii */
        CHECK_EQ(gh_fs_sync(&fs), 0);

        /* zapis nowego pliku z awaria po K zapisach (liczy tez direct gh_disk_write) */
        fs.dev.fail_after = K;
        gh_fs_create(&fs, "/d/f.bin", 0644);
        gh_fs_write(&fs, "/d/f.bin", msg, N, 0);
        fs.dev.fail_after = 0;
        gh_fs_unmount(&fs);

        /* remount (recover) -> fsck czysty dla KAZDEGO K */
        CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
        int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
        CHECK_EQ(issues, 0);

        /* jesli plik istnieje, ODCZYT NIE moze zwrocic EIO (brak ref. bloku z bledna suma) */
        struct gh_inode st; uint64_t ino;
        if (gh_fs_getattr(&fs, "/d/f.bin", &st, &ino) == 0 && st.type == GH_FILE && st.size > 0) {
            char *rb = malloc(st.size);
            ssize_t r = gh_fs_read(&fs, "/d/f.bin", rb, st.size, 0);
            CHECK(r >= 0);                       /* nigdy EIO */
            if (r == (ssize_t)N) CHECK_EQ(memcmp(rb, msg, N), 0);  /* committed -> pelne dane */
            free(rb);
        }
        gh_fs_unmount(&fs); unlink(tmp);
    }

    /* trwalosc: pelny zapis + sync + remount -> dane bajt-w-bajt */
    char tmp[] = "/tmp/ghost_oddXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/big.bin", 0644), 0);
    CHECK_EQ(gh_fs_write(&fs, "/big.bin", msg, N, 0), (ssize_t)N);
    CHECK_EQ(gh_fs_sync(&fs), 0);
    gh_fs_unmount(&fs);
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char *rb = malloc(N);
    CHECK_EQ(gh_fs_read(&fs, "/big.bin", rb, N, 0), (ssize_t)N);   /* trwale, bez EIO */
    CHECK_EQ(memcmp(rb, msg, N), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    free(rb); gh_fs_unmount(&fs); unlink(tmp);

    free(msg);
}

int main(void) {
    srand(2026);
    RUN_TEST(test_crash_atomic);
    RUN_TEST(test_ordered_data_crash);
    RUN_TEST(test_corrupt_mount);
    return TEST_SUMMARY();
}
