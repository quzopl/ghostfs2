#include "test.h"
#include "../src/fs.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <errno.h>

static void test_end_to_end(void) {
    char tmp[] = "/tmp/ghost_fsXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/dir/hello.txt", 0644), 0);

    const char *msg = "zawartosc pliku ghost";
    CHECK_EQ(gh_fs_write(&fs, "/dir/hello.txt", msg, strlen(msg), 0),
             (ssize_t)strlen(msg));
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/dir/hello.txt", buf, sizeof(buf), 0),
             (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/dir/hello.txt", &st, &ino), 0);
    CHECK_EQ(st.type, GH_FILE);
    CHECK_EQ(st.size, strlen(msg));

    /* rmdir niepustego = ENOTEMPTY */
    CHECK_EQ(gh_fs_rmdir(&fs, "/dir"), -ENOTEMPTY);
    CHECK_EQ(gh_fs_unlink(&fs, "/dir/hello.txt"), 0);
    CHECK_EQ(gh_fs_rmdir(&fs, "/dir"), 0);

    int issues = -1;
    CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs);
    unlink(tmp);
}

static void test_fsck_large_file(void) {
    char tmp[] = "/tmp/ghost_fs_largeXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 8192, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/big", 0644), 0);

    char *chunk = malloc(GH_BLOCK_SIZE);
    memset(chunk, 0xAB, GH_BLOCK_SIZE);
    for (int b = 0; b < 600; b++)
        CHECK_EQ(gh_fs_write(&fs, "/big", chunk, GH_BLOCK_SIZE, (uint64_t)b * GH_BLOCK_SIZE),
                 (ssize_t)GH_BLOCK_SIZE);

    int issues = -1;
    CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    CHECK_EQ(gh_fs_unlink(&fs, "/big"), 0);
    CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    free(chunk);
    gh_fs_unmount(&fs);
    unlink(tmp);
}

static void test_fsck_bad_pointer(void) {
    char tmp[] = "/tmp/ghost_fs_badptrXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 256, 64), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/victim", 0644), 0);
    CHECK_EQ(gh_fs_write(&fs, "/victim", "hi", 2, 0), (ssize_t)2);

    struct gh_inode n; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/victim", &n, &ino), 0);

    /* corrupt direct[0] to an out-of-range block number */
    n.direct[0] = fs.sb.total_blocks + 5;
    CHECK_EQ(gh_inode_write(&fs.dev, &fs.sb, ino, &n), 0);

    int issues = 0;
    CHECK_EQ(gh_fsck(&fs, 0, &issues), 0);
    CHECK(issues > 0);

    gh_fs_unmount(&fs);
    unlink(tmp);
}

static void test_meta_ops(void) {
    char tmp[] = "/tmp/ghost_metaXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/f.txt", 0644), 0);
    const char *msg = "0123456789ABCDEF";
    CHECK_EQ(gh_fs_write(&fs, "/f.txt", msg, 16, 0), 16);

    /* truncate w dol */
    CHECK_EQ(gh_fs_truncate(&fs, "/f.txt", 8), 0);
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.size, 8);
    char buf[32] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/f.txt", buf, sizeof(buf), 0), 8);
    CHECK_EQ(memcmp(buf, "01234567", 8), 0);

    /* truncate na katalogu = EISDIR */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_truncate(&fs, "/d", 0), -EISDIR);

    /* chmod */
    CHECK_EQ(gh_fs_chmod(&fs, "/f.txt", 0600), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.mode, 0600);

    /* chown (uid=1000, gid bez zmiany) */
    CHECK_EQ(gh_fs_chown(&fs, "/f.txt", 1000, (uint32_t)-1), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.uid, 1000);

    /* utimens */
    CHECK_EQ(gh_fs_utimens(&fs, "/f.txt", 111, 222), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/f.txt", &st, &ino), 0);
    CHECK_EQ(st.atime, 111);
    CHECK_EQ(st.mtime, 222);

    /* nieistniejaca sciezka */
    CHECK_EQ(gh_fs_chmod(&fs, "/brak", 0600), -ENOENT);

    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_statfs_sync(void) {
    char tmp[] = "/tmp/ghost_stfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    struct gh_statfs a;
    CHECK_EQ(gh_fs_statfs(&fs, &a), 0);
    CHECK_EQ(a.block_size, GH_BLOCK_SIZE);
    CHECK_EQ(a.total_blocks, 1024);
    CHECK(a.free_blocks > 0);
    uint64_t free_inodes_before = a.free_inodes;

    /* utworz plik z jednym blokiem -> mniej wolnych blokow i i-wezlow */
    CHECK_EQ(gh_fs_create(&fs, "/x", 0644), 0);
    char data[100]; memset(data, 'a', sizeof(data));
    CHECK_EQ(gh_fs_write(&fs, "/x", data, sizeof(data), 0), (ssize_t)sizeof(data));

    struct gh_statfs b;
    CHECK_EQ(gh_fs_statfs(&fs, &b), 0);
    CHECK(b.free_blocks < a.free_blocks);          /* zajeto blok danych */
    CHECK_EQ(b.free_inodes, free_inodes_before - 1); /* zajeto i-wezel */

    CHECK_EQ(gh_fs_sync(&fs), 0);

    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_hardlink(void) {
    char tmp[] = "/tmp/ghost_lnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    const char *msg = "wspolna tresc";
    CHECK_EQ(gh_fs_write(&fs, "/a", msg, strlen(msg), 0), (ssize_t)strlen(msg));

    /* twardy link /b -> ten sam i-wezel */
    CHECK_EQ(gh_fs_link(&fs, "/a", "/b"), 0);
    struct gh_inode sa, sbb; uint64_t ia, ib;
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &sa, &ia), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &sbb, &ib), 0);
    CHECK_EQ(ia, ib);              /* ten sam i-wezel */
    CHECK_EQ(sbb.nlink, 2);

    /* odczyt przez /b daje te sama tresc */
    char buf[32] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/b", buf, sizeof(buf), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    /* unlink /a: i-wezel zyje (nlink 2->1), /b dalej czytelny */
    CHECK_EQ(gh_fs_unlink(&fs, "/a"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &sa, &ia), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &sbb, &ib), 0);
    CHECK_EQ(sbb.nlink, 1);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(gh_fs_read(&fs, "/b", buf, sizeof(buf), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    /* link na katalog = EPERM */
    CHECK_EQ(gh_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh_fs_link(&fs, "/dir", "/dir2"), -EPERM);

    /* po unlink /b mapa spojna (i-wezel zwolniony) */
    CHECK_EQ(gh_fs_unlink(&fs, "/b"), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_rename(void) {
    char tmp[] = "/tmp/ghost_rnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* rename pliku w tym samym katalogu */
    CHECK_EQ(gh_fs_create(&fs, "/a.txt", 0644), 0);
    const char *msg = "tresc";
    CHECK_EQ(gh_fs_write(&fs, "/a.txt", msg, strlen(msg), 0), (ssize_t)strlen(msg));
    CHECK_EQ(gh_fs_rename(&fs, "/a.txt", "/b.txt"), 0);
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/a.txt", &st, &ino), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/b.txt", &st, &ino), 0);
    char buf[16] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/b.txt", buf, sizeof(buf), 0), (ssize_t)strlen(msg));
    CHECK_EQ(memcmp(buf, msg, strlen(msg)), 0);

    /* rename nadpisujacy istniejacy plik */
    CHECK_EQ(gh_fs_create(&fs, "/c.txt", 0644), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/b.txt", "/c.txt"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b.txt", &st, &ino), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/c.txt", &st, &ino), 0);

    /* rename katalogu ze zmiana rodzica: aktualizacja ".." */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d2", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1/sub", 0755), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/d1/sub", "/d2/sub"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/d2/sub", &st, &ino), 0);
    uint64_t dotdot;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2/sub/..", &dotdot), 0);
    uint64_t d2ino;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2", &d2ino), 0);
    CHECK_EQ(dotdot, d2ino);    /* ".." pokazuje nowego rodzica */

    /* nadpisanie niepustego katalogu = ENOTEMPTY */
    CHECK_EQ(gh_fs_mkdir(&fs, "/full", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/full/x", 0644), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/mover", 0755), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/mover", "/full"), -ENOTEMPTY);

    /* przeniesienie katalogu w jego poddrzewo = EINVAL */
    CHECK_EQ(gh_fs_rename(&fs, "/d2", "/d2/sub/x"), -EINVAL);

    /* nadpisanie pustego katalogu-celu ze zmiana rodzica nie zawyza nlink rodzica */
    CHECK_EQ(gh_fs_mkdir(&fs, "/pa", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/pb", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/pa/src", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/pb/dst", 0755), 0);   /* pusty cel */
    struct gh_inode pbn; uint64_t pbino;
    CHECK_EQ(gh_fs_getattr(&fs, "/pb", &pbn, &pbino), 0);
    uint32_t nlink_before = pbn.nlink;
    CHECK_EQ(gh_fs_rename(&fs, "/pa/src", "/pb/dst"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/pb", &pbn, &pbino), 0);
    CHECK_EQ(pbn.nlink, nlink_before);   /* bez zawyzenia */
    /* false-positive ochrony poddrzewa: /d20 obok /d2 nie jest blokowane */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d20", 0755), 0);
    CHECK_EQ(gh_fs_rename(&fs, "/d20", "/d2/d20moved"), 0);

    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_journaled_atomic(void) {
    char tmp[] = "/tmp/ghost_jatXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* operacja przez transakcje dziala normalnie i jest trwala po remount */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f.txt", 0644), 0);
    const char *m = "dane przez dziennik";
    CHECK_EQ(gh_fs_write(&fs, "/d/f.txt", m, strlen(m), 0), (ssize_t)strlen(m));
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);

    /* remount (uruchamia recover; brak zaleglej transakcji) i sprawdz trwalosc */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0), (ssize_t)strlen(m));
    CHECK_EQ(memcmp(buf, m, strlen(m)), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_fsck_tree(void) {
    char tmp[] = "/tmp/ghost_ftXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/a.txt", 0644), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    /* zepsuj nlink pliku recznie */
    uint64_t ino; struct gh_inode n;
    CHECK_EQ(gh_fs_getattr(&fs, "/a.txt", &n, &ino), 0);
    n.nlink = 5;
    CHECK_EQ(gh_inode_write(&fs.dev, &fs.sb, ino, &n), 0);
    issues = 0; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);  /* wykryte */
    issues = 0; CHECK_EQ(gh_fsck(&fs, 1, &issues), 0);                     /* napraw */
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0); /* czyste */
    CHECK_EQ(gh_fs_getattr(&fs, "/a.txt", &n, &ino), 0);
    CHECK_EQ(n.nlink, 1);   /* naprawione do liczby referencji */

    /* osierocony i-wezel: zaalokuj bez wpisu katalogowego */
    uint64_t orphan;
    CHECK_EQ(gh_inode_alloc(&fs.dev, &fs.sb, GH_FILE, &orphan), 0);
    issues = 0; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);  /* sierota wykryta */
    issues = 0; CHECK_EQ(gh_fsck(&fs, 1, &issues), 0);                     /* napraw: zwolnij */
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    struct gh_inode on; CHECK_EQ(gh_inode_read(&fs.dev, &fs.sb, orphan, &on), 0);
    CHECK_EQ(on.type, GH_FREE);   /* zwolniony */

    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_alloc_hint_mass(void) {
    char tmp[] = "/tmp/ghost_ahXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 8192, 1024), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    char name[64];
    /* utworz 400 plikow */
    for (int i = 0; i < 400; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_create(&fs, name, 0644), 0);
        char d[50]; memset(d, 'a', sizeof(d));
        CHECK_EQ(gh_fs_write(&fs, name, d, sizeof(d), 0), (ssize_t)sizeof(d));
    }
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* usun co drugi */
    for (int i = 0; i < 400; i += 2) {
        snprintf(name, sizeof(name), "/f%d", i);
        CHECK_EQ(gh_fs_unlink(&fs, name), 0);
    }
    /* utworz 200 nowych — musza trafic w zwolnione zasoby (hint nie pomija) */
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "/g%d", i);
        CHECK_EQ(gh_fs_create(&fs, name, 0644), 0);
    }
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* odczyt zachowanych plikow spojny */
    char buf[64];
    snprintf(name, sizeof(name), "/f1");   /* nieparzysty -> zachowany */
    CHECK_EQ(gh_fs_read(&fs, name, buf, 50, 0), 50);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_cache_transparent(void) {
    char tmp[] = "/tmp/ghost_caXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK(fs.dev.cache != NULL);              /* cache wlaczony przez mount */

    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f", 0644), 0);
    const char *m = "tresc-przez-cache-1234567890";
    CHECK_EQ(gh_fs_write(&fs, "/d/f", m, strlen(m), 0), (ssize_t)strlen(m));
    /* wielokrotny odczyt (trafienia cache) spojny */
    for (int i = 0; i < 5; i++) {
        char b[64] = {0};
        CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m));
        CHECK_EQ(memcmp(b, m, strlen(m)), 0);
    }
    /* nadpisanie -> odczyt widzi nowa tresc (write-through) */
    const char *m2 = "NOWA";
    CHECK_EQ(gh_fs_truncate(&fs, "/d/f", strlen(m2)), 0);
    CHECK_EQ(gh_fs_write(&fs, "/d/f", m2, strlen(m2), 0), (ssize_t)strlen(m2));
    char b[64] = {0}; CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m2));
    CHECK_EQ(memcmp(b, m2, strlen(m2)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);

    /* remount (swiezy cache) -> dane trwale i spojne */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    memset(b, 0, sizeof(b));
    CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m2));
    CHECK_EQ(memcmp(b, m2, strlen(m2)), 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_rename_flags(void) {
    char tmp[] = "/tmp/ghost_rfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    const char *ma = "AAAA"; CHECK_EQ(gh_fs_write(&fs, "/a", ma, 4, 0), 4);
    CHECK_EQ(gh_fs_create(&fs, "/b", 0644), 0);
    const char *mb = "BBBBBB"; CHECK_EQ(gh_fs_write(&fs, "/b", mb, 6, 0), 6);

    /* NOREPLACE: cel istnieje -> EEXIST */
    CHECK_EQ(gh_fs_rename2(&fs, "/a", "/b", GH_RENAME_NOREPLACE), -EEXIST);
    /* NOREPLACE: cel nie istnieje -> przenosi */
    CHECK_EQ(gh_fs_rename2(&fs, "/a", "/c", GH_RENAME_NOREPLACE), 0);
    struct gh_inode st; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &st, &ino), -ENOENT);
    CHECK_EQ(gh_fs_getattr(&fs, "/c", &st, &ino), 0);

    /* EXCHANGE: zamiana /c (AAAA) i /b (BBBBBB) */
    uint64_t ic, ib;
    CHECK_EQ(gh_fs_getattr(&fs, "/c", &st, &ic), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &st, &ib), 0);
    CHECK_EQ(gh_fs_rename2(&fs, "/c", "/b", GH_RENAME_EXCHANGE), 0);
    uint64_t ic2, ib2;
    CHECK_EQ(gh_fs_getattr(&fs, "/c", &st, &ic2), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/b", &st, &ib2), 0);
    CHECK_EQ(ic2, ib);   /* zamienione */
    CHECK_EQ(ib2, ic);
    char buf[8] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/c", buf, sizeof(buf), 0), 6);   /* /c ma teraz BBBBBB */
    CHECK_EQ(memcmp(buf, mb, 6), 0);

    /* EXCHANGE: brak celu -> ENOENT */
    CHECK_EQ(gh_fs_rename2(&fs, "/c", "/niema", GH_RENAME_EXCHANGE), -ENOENT);
    /* obie flagi -> EINVAL */
    CHECK_EQ(gh_fs_rename2(&fs, "/c", "/b", GH_RENAME_NOREPLACE | GH_RENAME_EXCHANGE), -EINVAL);

    /* EXCHANGE katalog<->plik ze zmiana rodzica: ".." aktualizowane */
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d2", 0755), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d1/sub", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d2/file", 0644), 0);
    CHECK_EQ(gh_fs_rename2(&fs, "/d1/sub", "/d2/file", GH_RENAME_EXCHANGE), 0);
    uint64_t dd;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2/file/..", &dd), 0);  /* /d2/file to teraz katalog */
    uint64_t d2;
    CHECK_EQ(gh_path_resolve(&fs.dev, &fs.sb, "/d2", &d2), 0);
    CHECK_EQ(dd, d2);

    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_discard_on_free(void) {
    char tmp[] = "/tmp/ghost_doXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* zapisz duzy plik (wiele blokow) */
    CHECK_EQ(gh_fs_create(&fs, "/big", 0644), 0);
    char *d = malloc(200000); memset(d, 'z', 200000);
    CHECK_EQ(gh_fs_write(&fs, "/big", d, 200000, 0), 200000);
    struct stat sb1; stat(tmp, &sb1);
    /* usun -> odroczone discardy wykonane przy flush -> plik rzadszy */
    CHECK_EQ(gh_fs_unlink(&fs, "/big"), 0);
    CHECK_EQ(gh_fs_sync(&fs), 0);            /* discardy wykonywane przy flush */
    struct stat sb2; stat(tmp, &sb2);
    CHECK(sb2.st_blocks <= sb1.st_blocks);   /* discard zwolnil miejsce (best-effort) */
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    /* integralnosc: po realokacji blok nie jest discardowany (zapis-odczyt spojny) */
    CHECK_EQ(gh_fs_create(&fs, "/re", 0644), 0);
    const char *m = "po-realokacji-spojne";
    CHECK_EQ(gh_fs_write(&fs, "/re", m, strlen(m), 0), (ssize_t)strlen(m));
    char rb[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/re", rb, sizeof(rb), 0), (ssize_t)strlen(m));
    CHECK_EQ(memcmp(rb, m, strlen(m)), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    free(d); gh_fs_unmount(&fs); unlink(tmp);
}

static void test_batch_atomicity(void) {
    char tmp[] = "/tmp/ghost_baXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 256, 64), 0);   /* maly dziennik -> latwy ENOSPC w paczce */
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* operacja A: udana */
    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    const char *ma = "tresc-A";
    CHECK_EQ(gh_fs_write(&fs, "/a", ma, strlen(ma), 0), (ssize_t)strlen(ma));
    /* operacja B: zapis wiekszy niz pojemnosc dziennika -> ENOSPC, pelne wycofanie */
    CHECK_EQ(gh_fs_create(&fs, "/b", 0644), 0);
    char *big = malloc(2 * 1024 * 1024); memset(big, 'b', 2*1024*1024);
    ssize_t w = gh_fs_write(&fs, "/b", big, 2*1024*1024, 0);
    CHECK(w < 0);   /* nie zmiescilo sie -> blad, operacja wycofana */
    free(big);
    /* A nadal spojna (read-your-writes), fsck czysty */
    char buf[16] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/a", buf, sizeof(buf), 0), (ssize_t)strlen(ma));
    CHECK_EQ(memcmp(buf, ma, strlen(ma)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_durability_flush(void) {
    char tmp[] = "/tmp/ghost_duXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f", 0644), 0);
    const char *m = "trwale-po-flush";
    CHECK_EQ(gh_fs_write(&fs, "/d/f", m, strlen(m), 0), (ssize_t)strlen(m));
    /* bez jawnego sync: unmount flushuje */
    gh_fs_unmount(&fs);
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char b[32] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/d/f", b, sizeof(b), 0), (ssize_t)strlen(m));
    CHECK_EQ(memcmp(b, m, strlen(m)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_csum_detect(void) {
    char tmp[] = "/tmp/ghost_csXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/f", 0644), 0);
    const char *m = "dane-do-ochrony-checksumem-1234567890";
    CHECK_EQ(gh_fs_write(&fs, "/f", m, strlen(m), 0), (ssize_t)strlen(m));
    /* znajdz fizyczny blok danych: pierwszy blok pliku */
    struct gh_inode n; uint64_t ino; CHECK_EQ(gh_fs_getattr(&fs, "/f", &n, &ino), 0);
    uint64_t phys = n.direct[0];
    gh_fs_sync(&fs);          /* utrwal na dysk */
    gh_fs_unmount(&fs);

    /* zdrowy odczyt po remount */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/f", buf, sizeof(buf), 0), (ssize_t)strlen(m));
    gh_fs_unmount(&fs);

    /* uszkodz surowo blok danych (1 bajt) */
    int rf = open(tmp, O_RDWR);
    uint8_t b; CHECK_EQ(pread(rf, &b, 1, (off_t)phys * GH_BLOCK_SIZE + 10), 1);
    b ^= 0xFF; CHECK_EQ(pwrite(rf, &b, 1, (off_t)phys * GH_BLOCK_SIZE + 10), 1);
    close(rf);

    /* odczyt uszkodzonego -> EIO (korupcja wykryta) */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(gh_fs_read(&fs, "/f", buf, sizeof(buf), 0), -EIO);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_mknod(void) {
    char tmp[] = "/tmp/ghost_mkXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);

    /* FIFO */
    CHECK_EQ(gh_fs_mknod(&fs, "/p", S_IFIFO | 0644, 0), 0);
    struct gh_inode n; uint64_t ino;
    CHECK_EQ(gh_fs_getattr(&fs, "/p", &n, &ino), 0); CHECK_EQ(n.type, GH_FIFO);
    /* gniazdo */
    CHECK_EQ(gh_fs_mknod(&fs, "/s", S_IFSOCK | 0644, 0), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/s", &n, &ino), 0); CHECK_EQ(n.type, GH_SOCK);
    /* urzadzenie znakowe 1,3 (/dev/null) */
    uint64_t rdev = makedev(1, 3);
    CHECK_EQ(gh_fs_mknod(&fs, "/cdev", S_IFCHR | 0666, rdev), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/cdev", &n, &ino), 0);
    CHECK_EQ(n.type, GH_CHR); CHECK_EQ(n.direct[0], rdev);     /* rdev w direct[0] */
    /* urzadzenie blokowe 8,0 */
    CHECK_EQ(gh_fs_mknod(&fs, "/bdev", S_IFBLK | 0660, makedev(8, 0)), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/bdev", &n, &ino), 0); CHECK_EQ(n.type, GH_BLK);

    /* fsck czysty: rdev w direct[0] NIE jest fałszywie osiagalnym blokiem */
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    /* truncate wezla specjalnego -> EINVAL (chroni rdev) */
    CHECK_EQ(gh_fs_truncate(&fs, "/cdev", 0), -EINVAL);
    /* mknod katalogu -> EINVAL */
    CHECK_EQ(gh_fs_mknod(&fs, "/d", S_IFDIR | 0755, 0), -EINVAL);
    /* hardlink urzadzenia: nlink rośnie */
    CHECK_EQ(gh_fs_link(&fs, "/cdev", "/cdev2"), 0);
    CHECK_EQ(gh_fs_getattr(&fs, "/cdev2", &n, &ino), 0); CHECK_EQ(n.nlink, 2);

    /* unlink wezla -> fsck czysty, brak falszywego zwolnienia bloku */
    CHECK_EQ(gh_fs_unlink(&fs, "/cdev"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/cdev2"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/bdev"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/p"), 0);
    CHECK_EQ(gh_fs_unlink(&fs, "/s"), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);

    gh_fs_unmount(&fs); unlink(tmp);
}

/* REGRESJA: naprawy fsck --repair MUSZA byc trwale (przetrwac remount).
   Wczesniej naprawy buforowaly sie w running txn bez dirty -> ginely przy unmount. */
static void test_fsck_repair_persists(void) {
    char tmp[] = "/tmp/ghost_frpXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 2048, 128), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_create(&fs, "/a", 0644), 0);
    gh_fs_unmount(&fs);   /* utrwal na dysk */

    /* uszkodz na dysku przez surowy dev (brak txn -> zapis bezposredni, sumy spojne) */
    struct gh_dev dev; struct gh_superblock sb;
    CHECK_EQ(gh_dev_open(tmp, &dev), 0); CHECK_EQ(gh_mount_sb(&dev, &sb), 0);
    struct gh_inode n; CHECK_EQ(gh_inode_read(&dev, &sb, GH_ROOT_INO + 1, &n), 0);
    n.nlink = 9; CHECK_EQ(gh_inode_write(&dev, &sb, GH_ROOT_INO + 1, &n), 0);  /* zly nlink */
    uint64_t orphan; CHECK_EQ(gh_inode_alloc(&dev, &sb, GH_FILE, &orphan), 0); /* sierota */
    uint64_t leak; CHECK_EQ(gh_alloc_block(&dev, &sb, &leak), 0);              /* wyciek w mapie */
    gh_dev_close(&dev);

    /* fsck wykrywa, naprawia, unmount utrwala */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    int issues = 0; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK(issues >= 3);
    issues = 0; CHECK_EQ(gh_fsck(&fs, 1, &issues), 0);
    gh_fs_unmount(&fs);

    /* SWIEZY montaz: naprawy TRWALE (to lapie blad) */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    struct gh_inode an; uint64_t aino;
    CHECK_EQ(gh_fs_getattr(&fs, "/a", &an, &aino), 0); CHECK_EQ(an.nlink, 1);  /* nlink naprawiony */
    struct gh_inode on; CHECK_EQ(gh_inode_read(&fs.dev, &fs.sb, orphan, &on), 0);
    CHECK_EQ(on.type, GH_FREE);   /* sierota zwolniona */
    gh_fs_unmount(&fs); unlink(tmp);
}

/* statfs szybki: liczby identyczne jak brute-force (czytanie kazdego elementu osobno) */
static void test_statfs_fast(void) {
    char tmp[] = "/tmp/ghost_sfXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 256), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    for (int i = 0; i < 10; i++) {
        char nm[16]; snprintf(nm, sizeof(nm), "/f%d", i);
        CHECK_EQ(gh_fs_create(&fs, nm, 0644), 0);
        char data[5000]; memset(data, 'x', sizeof(data));
        CHECK_EQ(gh_fs_write(&fs, nm, data, sizeof(data), 0), (ssize_t)sizeof(data));
    }
    CHECK_EQ(gh_fs_sync(&fs), 0);

    struct gh_statfs s; CHECK_EQ(gh_fs_statfs(&fs, &s), 0);
    uint64_t bf = 0;
    for (uint64_t b = fs.sb.data_start; b < fs.sb.total_blocks; b++) {
        int set = 0; gh_bitmap_test(&fs.dev, &fs.sb, b, &set); if (!set) bf++;
    }
    CHECK_EQ(s.free_blocks, bf);                 /* optymalizowany == brute-force */
    uint64_t fi = 0;
    for (uint64_t i = 0; i < fs.sb.inode_count; i++) {
        struct gh_inode n;
        if (gh_inode_read(&fs.dev, &fs.sb, i, &n) == 0 && n.type == GH_FREE) fi++;
    }
    CHECK_EQ(s.free_inodes, fi);
    CHECK_EQ(s.total_blocks, fs.sb.total_blocks);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_ordered_data_write(void) {
    char tmp[] = "/tmp/ghost_odXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 64), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    /* a) wielo-blokowy + czesciowy ostatni blok */
    CHECK_EQ(gh_fs_create(&fs, "/big", 0644), 0);
    size_t N = 4096*3 + 1234; char *w = malloc(N); for (size_t i=0;i<N;i++) w[i]=(char)(i*7+1);
    CHECK_EQ(gh_fs_write(&fs, "/big", w, N, 0), (ssize_t)N);
    char *rd = malloc(N); CHECK_EQ(gh_fs_read(&fs, "/big", rd, N, 0), (ssize_t)N);
    CHECK_EQ(memcmp(w, rd, N), 0);                    /* read-your-writes (direct) */
    /* b) dziura: zapis na offsecie 8192, [0,8192) to dziura -> zera */
    CHECK_EQ(gh_fs_create(&fs, "/sparse", 0644), 0);
    CHECK_EQ(gh_fs_write(&fs, "/sparse", "XYZ", 3, 8192), 3);
    char hole[8195]; CHECK_EQ(gh_fs_read(&fs, "/sparse", hole, sizeof(hole), 0), (ssize_t)sizeof(hole));
    for (int i=0;i<8192;i++) CHECK_EQ(hole[i], 0);    /* dziura = zera (nie smieci z nz) */
    CHECK_EQ(memcmp(hole+8192, "XYZ", 3), 0);
    /* c) nadpisanie istniejacego bloku (journalowana sciezka) */
    CHECK_EQ(gh_fs_write(&fs, "/big", "OVER", 4, 0), 4);
    CHECK_EQ(gh_fs_read(&fs, "/big", rd, 4, 0), 4); CHECK_EQ(memcmp(rd, "OVER", 4), 0);
    /* d) fsck czysty */
    int issues=-1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    free(w); free(rd);
    gh_fs_unmount(&fs); unlink(tmp);
}

/* REGRESJA: blok zapisany direct, ktory byl wczesniej w buforze txn (zwolniony +
   realokowany w tej samej niezflushowanej paczce) — nieaktualny obraz w txn NIE moze
   zacieniac swiezego zapisu (zaden odczyt nie zwraca EIO). Lapie blad z integracji. */
static void test_ordered_data_realloc_no_eio(void) {
    char tmp[] = "/tmp/ghost_orXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 4096, 64), 0);
    struct gh_fs fs; CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    /* BEZ sync: wszystko zostaje w jednej, niezflushowanej paczce txn */
    for (int round = 0; round < 8; round++) {
        char na[32], nb[32];
        snprintf(na, sizeof(na), "/a%d", round);
        snprintf(nb, sizeof(nb), "/b%d", round);
        CHECK_EQ(gh_fs_create(&fs, na, 0644), 0);
        char da[200]; memset(da, 'A' + round, sizeof(da));
        CHECK_EQ(gh_fs_write(&fs, na, da, sizeof(da), 0), (ssize_t)sizeof(da));  /* nowy blok: direct */
        /* nadpisanie IN-PLACE (sciezka journalowana) -> obraz bloku DANYCH trafia do bufora txn */
        CHECK_EQ(gh_fs_write(&fs, na, "ZZZZ", 4, 0), 4);
        CHECK_EQ(gh_fs_unlink(&fs, na), 0);                                       /* zwolnij blok */
        CHECK_EQ(gh_fs_create(&fs, nb, 0644), 0);
        char db[200]; memset(db, 'a' + round, sizeof(db));
        CHECK_EQ(gh_fs_write(&fs, nb, db, sizeof(db), 0), (ssize_t)sizeof(db));   /* realokacja + direct */
        char rb[200]; memset(rb, 0, sizeof(rb));
        ssize_t r = gh_fs_read(&fs, nb, rb, sizeof(rb), 0);
        CHECK_EQ(r, (ssize_t)sizeof(db));                                         /* NIE EIO */
        CHECK_EQ(memcmp(rb, db, sizeof(db)), 0);                                  /* swieza tresc */
    }
    int issues=-1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);
    /* po remount dane trwale i bez EIO */
    CHECK_EQ(gh_fs_mount(&fs, tmp), 0);
    char rb[200]; memset(rb, 0, sizeof(rb));
    CHECK_EQ(gh_fs_read(&fs, "/b7", rb, sizeof(rb), 0), 200);
    char exp[200]; memset(exp, 'a' + 7, sizeof(exp));
    CHECK_EQ(memcmp(rb, exp, 200), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

int main(void) { RUN_TEST(test_ordered_data_write); RUN_TEST(test_ordered_data_realloc_no_eio); RUN_TEST(test_statfs_fast); RUN_TEST(test_end_to_end); RUN_TEST(test_fsck_large_file); RUN_TEST(test_fsck_bad_pointer); RUN_TEST(test_meta_ops); RUN_TEST(test_statfs_sync); RUN_TEST(test_hardlink); RUN_TEST(test_rename); RUN_TEST(test_rename_flags); RUN_TEST(test_journaled_atomic); RUN_TEST(test_fsck_tree); RUN_TEST(test_fsck_repair_persists); RUN_TEST(test_alloc_hint_mass); RUN_TEST(test_cache_transparent); RUN_TEST(test_discard_on_free); RUN_TEST(test_batch_atomicity); RUN_TEST(test_durability_flush); RUN_TEST(test_csum_detect); RUN_TEST(test_mknod); return TEST_SUMMARY(); }
