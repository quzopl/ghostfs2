#include "test.h"
#include "../src/fs.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void test_symlink(void) {
    char tmp[] = "/tmp/ghost_symXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    const char *target = "/a/b/c/cel.txt";
    CHECK_EQ(gh_fs_symlink(&fs, target, "/link"), 0);

    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/link", &st, &ino), 0);
    CHECK_EQ(st.type, GH_SYMLINK);

    char buf[256] = {0};
    ssize_t r = gh_fs_readlink(&fs, "/link", buf, sizeof(buf));
    CHECK_EQ(r, (ssize_t)strlen(target));
    CHECK_EQ(memcmp(buf, target, strlen(target)), 0);

    /* readlink na nie-symlinku = EINVAL */
    CHECK_EQ(gh_fs_create(&fs, "/plik", 0644), 0);
    CHECK_EQ(gh_fs_readlink(&fs, "/plik", buf, sizeof(buf)), -EINVAL);

    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_xattr(void) {
    char tmp[] = "/tmp/ghost_xaXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);

    /* set + get */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.kolor", "zielony", 7, 0), 0);
    char val[64] = {0};
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, sizeof(val)), 7);
    CHECK_EQ(memcmp(val, "zielony", 7), 0);

    /* get size-only */
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", NULL, 0), 7);
    /* ERANGE gdy za maly bufor */
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, 3), -ERANGE);
    /* ENODATA gdy brak */
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.brak", val, sizeof(val)), -ENODATA);

    /* drugi atrybut + list */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.rozmiar", "duzy", 4, 0), 0);
    char list[128] = {0};
    ssize_t ln = gh_fs_listxattr(&fs, "/f", list, sizeof(list));
    CHECK_EQ(ln, (ssize_t)(strlen("user.kolor") + 1 + strlen("user.rozmiar") + 1));

    /* XATTR_CREATE na istniejacym = EEXIST; XATTR_REPLACE na braku = ENODATA */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.kolor", "x", 1, GH_XATTR_CREATE), -EEXIST);
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.nowy", "x", 1, GH_XATTR_REPLACE), -ENODATA);

    /* nadpisanie wartosci */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.kolor", "niebieski", 9, 0), 0);
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, sizeof(val)), 9);
    CHECK_EQ(memcmp(val, "niebieski", 9), 0);

    /* remove */
    CHECK_EQ(gh_fs_removexattr(&fs, "/f", "user.kolor"), 0);
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.kolor", val, sizeof(val)), -ENODATA);
    CHECK_EQ(gh_fs_removexattr(&fs, "/f", "user.brak"), -ENODATA);

    /* usun ostatni -> blok xattr zwolniony, fsck czyste */
    CHECK_EQ(gh_fs_removexattr(&fs, "/f", "user.rozmiar"), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_xattr_multiblock(void) {
    char tmp[] = "/tmp/ghost_xmXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);

    /* ustaw 10 atrybutow po 600 B wartosci => > 4 KB lacznie => wymusza lancuch */
    char val[600]; char nm[32];
    for (int i = 0; i < 10; i++) {
        memset(val, 'A' + i, sizeof(val));
        snprintf(nm, sizeof(nm), "user.attr%d", i);
        CHECK_EQ(gh_fs_setxattr(&fs, "/f", nm, val, sizeof(val), 0), 0);
    }
    /* odczytaj wszystkie poprawnie */
    char out[700];
    for (int i = 0; i < 10; i++) {
        snprintf(nm, sizeof(nm), "user.attr%d", i);
        CHECK_EQ(gh_fs_getxattr(&fs, "/f", nm, out, sizeof(out)), 600);
        char exp[600]; memset(exp, 'A' + i, sizeof(exp));
        CHECK_EQ(memcmp(out, exp, 600), 0);
    }
    /* lista zawiera 10 nazw */
    char list[512] = {0};
    ssize_t ln = gh_fs_listxattr(&fs, "/f", list, sizeof(list));
    CHECK(ln > 0);
    int cnt = 0; for (ssize_t k = 0; k < ln; k++) if (list[k] == '\0') cnt++;
    CHECK_EQ(cnt, 10);
    /* fsck czysty (lancuch osiagalny) */
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* usun wszystkie -> lancuch zwolniony */
    for (int i = 0; i < 10; i++) {
        snprintf(nm, sizeof(nm), "user.attr%d", i);
        CHECK_EQ(gh_fs_removexattr(&fs, "/f", nm), 0);
    }
    CHECK_EQ(gh_fs_getxattr(&fs, "/f", "user.attr0", out, sizeof(out)), -ENODATA);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* usun plik -> brak wyciekow (fsck czysty) */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.x", "y", 1, 0), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/f"), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}

/* uszkodzony lancuch xattr (next -> sam siebie) nie moze zawiesic sciezki danych */
static void test_xattr_cycle_guard(void) {
    char tmp[] = "/tmp/ghost_xcXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);
    /* jeden atrybut -> powstaje 1 blok xattr (glowa) */
    CHECK_EQ(gh_fs_setxattr(&fs, "/f", "user.k", "v", 1, 0), 0);

    /* odczytaj numer glowy lancucha z i-wezla */
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/f", &st, &ino), 0);
    uint64_t head = st.xattr_block;
    CHECK(head != 0);

    /* spreparuj cykl: next bloku glowy wskazuje na samego siebie */
    uint8_t blk[GH_BLOCK_SIZE];
    CHECK_EQ(gh_block_read(&fs.dev, head, blk), 0);
    memcpy(blk, &head, 8);
    CHECK_EQ(gh_block_write(&fs.dev, head, blk), 0);
    gh_fs_unmount(&fs);

    /* zamontuj swiezo na uszkodzonym obrazie */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* alarm jako zabezpieczenie: gdyby fix byl bledny, sygnal zabije proces */
    alarm(10);
    char val[64] = {0};
    /* getxattr nieistniejacej nazwy: musi sie zakonczyc (nie zawiesic), zwracajac blad */
    CHECK(gh_fs_getxattr(&fs, "/f", "user.brak", val, sizeof(val)) < 0);
    /* listxattr: musi sie zakonczyc */
    char list[128] = {0};
    (void)gh_fs_listxattr(&fs, "/f", list, sizeof(list));
    /* unlink: musi sie zakonczyc (nie zawiesic) */
    (void)gh_fs_unlink(&fs, "/f");
    alarm(0);

    gh_fs_unmount(&fs); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_symlink);
    RUN_TEST(test_xattr);
    RUN_TEST(test_xattr_multiblock);
    RUN_TEST(test_xattr_cycle_guard);
    return TEST_SUMMARY();
}
