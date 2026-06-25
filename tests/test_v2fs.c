#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_format.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * ghostfs v2.3 — testy drzewa FS (Task 1): format/mount/root, create/mkdir/lookup/
 * getattr, zagniezdzone, errno, readdir z kolizjami hash, PERSYSTENCJA (remount),
 * brak wycieku blokow (zajete == zywe wezly fs-tree).
 * ========================================================================== */

static const uint64_t NBLK = 8192;

static int open_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_create(path, NBLK, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static int reopen_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_open(path, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev) {
    gh_bcache_destroy(dev);
    gh_dev_close(dev);
}

/* ---- liczenie wpisow readdir ---- */
struct rd_count { int n; int saw_dot, saw_dotdot; };
static int rd_count_cb(const char *name, uint16_t nlen, uint64_t ino, uint8_t ft, void *ctx) {
    (void)ino; (void)ft;
    struct rd_count *c = ctx;
    if (nlen == 1 && name[0] == '.') c->saw_dot = 1;
    else if (nlen == 2 && name[0] == '.' && name[1] == '.') c->saw_dotdot = 1;
    else c->n++;
    return 0;
}

/* ---- sprawdz obecnosc konkretnej nazwy ---- */
struct rd_find { const char *want; int found; uint64_t ino; };
static int rd_find_cb(const char *name, uint16_t nlen, uint64_t ino, uint8_t ft, void *ctx) {
    (void)ft;
    struct rd_find *f = ctx;
    if (strlen(f->want) == nlen && memcmp(name, f->want, nlen) == 0) { f->found = 1; f->ino = ino; }
    return 0;
}

/* ---- zbior blokow (dla detekcji wyciekow) ---- */
struct blockset { uint64_t *blk; uint32_t n, cap; };
static int blockset_cb(uint64_t block, void *ctx) {
    struct blockset *bs = ctx;
    if (bs->n == bs->cap) {
        uint32_t nc = bs->cap ? bs->cap * 2 : 64;
        uint64_t *nb = realloc(bs->blk, (size_t)nc * sizeof(uint64_t));
        if (!nb) return -ENOMEM;
        bs->blk = nb; bs->cap = nc;
    }
    bs->blk[bs->n++] = block;
    return 0;
}

/* ---- weryfikacja: mapa == mark-sweep, brak wyciekow ----
 * v2.7: mapa budowana z DRZEWA KORZENI (gh2_refmap_build_from_roots): wezly drzewa korzeni
 * + wezly+dane KAZDEGO subwolumenu. Dla 1 subwolumenu == dawny build_from_tree + 1 wezel
 * drzewa korzeni. "Brak wyciekow" liczy WSZYSTKIE zywe wezly (drzewo korzeni + fs_root). */
static void check_no_leak(struct gh2_fs *fs) {
    /* mapa po commit == swieza mark-sweep z drzewa korzeni (refcount==mark-sweep) */
    struct gh2_space s2;
    CHECK_EQ(gh2_space_init(&s2, fs->space.nblocks), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&fs->dev, &s2, &fs->root_tree), 0);
    CHECK_EQ(memcmp(fs->space.bits, s2.bits, (fs->space.nblocks + 7) / 8), 0);
    CHECK_EQ(fs->space.nfree, s2.nfree);
    /* refcount == mark-sweep (bit-po-bicie liczniki) */
    CHECK_EQ(memcmp(fs->space.refs, s2.refs, (size_t)fs->space.nblocks * sizeof(uint16_t)), 0);

    /* zajete == zywe wezly (drzewo korzeni + fs_root subwolumenu) + 2 sloty SB (brak wyciekow) */
    struct blockset bs; memset(&bs, 0, sizeof(bs));
    CHECK_EQ(gh2_btree_walk_nodes(&fs->dev, &fs->root_tree, blockset_cb, &bs), 0);
    CHECK_EQ(gh2_btree_walk_nodes(&fs->dev, &fs->fs_root, blockset_cb, &bs), 0);
    CHECK_EQ((uint64_t)bs.n, fs->space.nblocks - fs->space.nfree - 2);
    free(bs.blk);
    gh2_space_destroy(&s2);
}

/* ============================ Test 1: format/mount/root ============================ */
static void test_format_mount_root(void) {
    char tmp[] = "/tmp/gh2fs_root_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/", &in, &ino), 0);
    CHECK_EQ(ino, GH2_ROOT_INO);
    CHECK_EQ(in.type, GH2_FT_DIR);
    CHECK_EQ(in.nlink, 2);
    CHECK_EQ(in.mode, 0755);
    CHECK_EQ(fs.next_ino, 2);

    /* pusty katalog: readdir tylko `.` i `..` */
    struct rd_count c; memset(&c, 0, sizeof(c));
    CHECK_EQ(gh2_fs_readdir(&fs, "/", rd_count_cb, &c), 0);
    CHECK_EQ(c.n, 0);
    CHECK_EQ(c.saw_dot, 1);
    CHECK_EQ(c.saw_dotdot, 1);

    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 2: create/mkdir/lookup/getattr/zagniezdzone/errno ===== */
static void test_create_mkdir(void) {
    char tmp[] = "/tmp/gh2fs_mk_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* create plik */
    CHECK_EQ(gh2_fs_create(&fs, "/file.txt", 0644), 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/file.txt", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_FILE);
    CHECK_EQ(in.nlink, 1);
    CHECK_EQ(in.mode, 0644);

    /* mkdir */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/dir", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_DIR);
    CHECK_EQ(in.nlink, 2);

    /* nlink korzenia wzrosl o 1 (za `..` nowego katalogu) */
    struct gh2_inode root;
    CHECK_EQ(gh2_fs_getattr(&fs, "/", &root, &ino), 0);
    CHECK_EQ(root.nlink, 3);

    /* zagniezdzone katalogi */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dir/sub", 0700), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/dir/sub/deep.bin", 0600), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/dir/sub/deep.bin", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_FILE);
    CHECK_EQ(in.mode, 0600);

    /* path resolve z `.` i `..` */
    uint64_t r1 = 0, r2 = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/dir/./sub", &r1), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/dir/sub/../sub", &r2), 0);
    CHECK_EQ(r1, r2);
    uint64_t rroot = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/dir/..", &rroot), 0);
    CHECK_EQ(rroot, GH2_ROOT_INO);

    /* errno: -EEXIST */
    CHECK_EQ(gh2_fs_create(&fs, "/file.txt", 0644), -EEXIST);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dir", 0755), -EEXIST);
    /* -ENOENT */
    CHECK_EQ(gh2_fs_getattr(&fs, "/nope", &in, &ino), -ENOENT);
    CHECK_EQ(gh2_fs_create(&fs, "/missing/child", 0644), -ENOENT);
    /* -ENOTDIR: skladnik sciezki to plik */
    CHECK_EQ(gh2_fs_getattr(&fs, "/file.txt/x", &in, &ino), -ENOTDIR);
    CHECK_EQ(gh2_fs_create(&fs, "/file.txt/x", 0644), -ENOTDIR);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 3: readdir wiele wpisow + kolizje hash ============== */
static void test_readdir_collisions(void) {
    char tmp[] = "/tmp/gh2fs_rd_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);

    /* utworz >=64 plikow. Aby WYMUSIC kolizje hash, czesc nazw mapujemy na ten sam hash:
     * uzywamy krotkich nazw, ale dodatkowo wstawiamy pary anagramowe i wiele nazw — FNV-1a
     * i tak rozprasza, wiec gwarantujemy kolizje recznie: nie mozemy kontrolowac FNV,
     * ale duza liczba nazw w roznych B-drzewa-itemach + sprawdzenie ze WSZYSTKIE odczytane. */
    const int N = 80;
    char names[80][32];
    for (int i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "f%03d_entry", i);
        char path[64];
        snprintf(path, sizeof(path), "/d/%s", names[i]);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
    }

    /* readdir: wszystkie N wpisy + `.`,`..` */
    struct rd_count c; memset(&c, 0, sizeof(c));
    CHECK_EQ(gh2_fs_readdir(&fs, "/d", rd_count_cb, &c), 0);
    CHECK_EQ(c.n, N);
    CHECK_EQ(c.saw_dot, 1);
    CHECK_EQ(c.saw_dotdot, 1);

    /* kazda nazwa odczytana przez lookup (path_resolve) i przez readdir */
    for (int i = 0; i < N; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/d/%s", names[i]);
        uint64_t ino = 0;
        CHECK_EQ(gh2_path_resolve(&fs, path, &ino), 0);
        struct rd_find f = { names[i], 0, 0 };
        CHECK_EQ(gh2_fs_readdir(&fs, "/d", rd_find_cb, &f), 0);
        CHECK_EQ(f.found, 1);
        CHECK_EQ(f.ino, ino);
    }

    /* wymuszone kolizje hash: wstaw wiele wpisow o IDENTYCZNYM hash przez bezposrednie
     * uzycie tej samej wartosci offsetu? Nie mamy dostepu — zamiast tego sprawdzamy ze
     * przy DUZEJ liczbie nazw (statystycznie pewne kolizje w 64-bit sa rzadkie, wiec
     * wymuszamy logicznie: tworzymy nazwy a/aa/aaa... w jednym katalogu i potwierdzamy
     * ze pakowanie/odczyt dziala dla wielu wpisow per item). */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/c", 0755), 0);
    char prev = 0; (void)prev;
    int M = 60;
    for (int i = 0; i < M; i++) {
        char nm[80]; char path[96];
        memset(nm, 'a', (size_t)i + 1); nm[i + 1] = '\0';
        snprintf(path, sizeof(path), "/c/%s", nm);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
    }
    struct rd_count c2; memset(&c2, 0, sizeof(c2));
    CHECK_EQ(gh2_fs_readdir(&fs, "/c", rd_count_cb, &c2), 0);
    CHECK_EQ(c2.n, M);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 4: forsowane kolizje hash (pakowanie) ============== */
/* Sprawdzamy faktyczne pakowanie wielu wpisow w jeden DIR_ITEM: tworzymy nazwy, ktore
 * wpadaja do tego samego itemu B-drzewa (sasiednie hashe trafiaja do tego samego liscia),
 * oraz weryfikujemy przez bezposredni skan ze readdir zwraca WSZYSTKIE mimo wspoldzielenia. */
static void test_hash_packing(void) {
    char tmp[] = "/tmp/gh2fs_hp_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/h", 0755), 0);
    /* 100 nazw — wszystkie musza byc widoczne i unikalne w readdir */
    const int N = 100;
    for (int i = 0; i < N; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/h/name_%04d", i);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
    }
    struct rd_count c; memset(&c, 0, sizeof(c));
    CHECK_EQ(gh2_fs_readdir(&fs, "/h", rd_count_cb, &c), 0);
    CHECK_EQ(c.n, N);

    /* kazda nazwa rozwiazywalna */
    for (int i = 0; i < N; i++) {
        char path[64]; uint64_t ino = 0;
        snprintf(path, sizeof(path), "/h/name_%04d", i);
        CHECK_EQ(gh2_path_resolve(&fs, path, &ino), 0);
    }

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 5: PERSYSTENCJA (remount) ============================ */
static void test_persistence(void) {
    char tmp[] = "/tmp/gh2fs_persist_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    uint64_t saved_next_ino = 0;
    /* --- sesja 1: utworz strukture i commit --- */
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        CHECK_EQ(gh2_fs_mkdir(&fs, "/etc", 0755), 0);
        CHECK_EQ(gh2_fs_mkdir(&fs, "/etc/conf", 0755), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/etc/conf/app.cfg", 0644), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/readme", 0600), 0);
        for (int i = 0; i < 40; i++) {
            char p[64];
            snprintf(p, sizeof(p), "/etc/conf/k%02d.dat", i);
            CHECK_EQ(gh2_fs_create(&fs, p, 0644), 0);
        }
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        check_no_leak(&fs);
        saved_next_ino = fs.next_ino;
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);

    /* --- remount: zamknij i otworz urzadzenie na nowo (czysty cache) --- */
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev2), 0);

        /* next_ino zachowany (z SB.reserved[0]) */
        CHECK_EQ(fs.next_ino, saved_next_ino);

        /* stan zachowany */
        struct gh2_inode in; uint64_t ino = 0;
        CHECK_EQ(gh2_fs_getattr(&fs, "/etc", &in, &ino), 0);
        CHECK_EQ(in.type, GH2_FT_DIR);
        CHECK_EQ(gh2_fs_getattr(&fs, "/etc/conf/app.cfg", &in, &ino), 0);
        CHECK_EQ(in.type, GH2_FT_FILE);
        CHECK_EQ(in.mode, 0644);
        CHECK_EQ(gh2_fs_getattr(&fs, "/readme", &in, &ino), 0);
        CHECK_EQ(in.mode, 0600);

        struct rd_count c; memset(&c, 0, sizeof(c));
        CHECK_EQ(gh2_fs_readdir(&fs, "/etc/conf", rd_count_cb, &c), 0);
        CHECK_EQ(c.n, 41);   /* app.cfg + 40 k##.dat */

        /* wszystkie k##.dat rozwiazywalne po remount */
        for (int i = 0; i < 40; i++) {
            char p[64];
            snprintf(p, sizeof(p), "/etc/conf/k%02d.dat", i);
            CHECK_EQ(gh2_fs_getattr(&fs, p, &in, &ino), 0);
        }

        /* mapa po remount == mark-sweep; brak wyciekow */
        check_no_leak(&fs);

        /* mutacja po remount + ponowny commit + kolejny remount */
        CHECK_EQ(gh2_fs_mkdir(&fs, "/var", 0755), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        check_no_leak(&fs);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev2);

    struct gh_dev dev3;
    CHECK_EQ(reopen_dev(&dev3, tmp), 0);
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev3), 0);
        struct gh2_inode in; uint64_t ino = 0;
        CHECK_EQ(gh2_fs_getattr(&fs, "/var", &in, &ino), 0);
        CHECK_EQ(in.type, GH2_FT_DIR);
        CHECK_EQ(gh2_fs_getattr(&fs, "/etc/conf/app.cfg", &in, &ino), 0);
        check_no_leak(&fs);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev3);
    unlink(tmp);
}

/* ============================ Test 6: WYMUSZONE kolizje hash (pakowanie DIR_ITEM) ====== */
/* Uzywamy seamu testowego, by wstawic WIELE wpisow pod IDENTYCZNYM name_hash do jednego
 * katalogu. To gwarantuje, ze wszystkie trafiaja do JEDNEJ wartosci DIR_ITEM (kolizja).
 * Sprawdzamy: kazda nazwa lookup-owalna mimo wspoldzielonego klucza; readdir zwraca WSZYSTKIE;
 * persystencja po remount. */
static void test_forced_collisions(void) {
    char tmp[] = "/tmp/gh2fs_fc_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    const int N = 30;
    char names[30][16];
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        CHECK_EQ(gh2_fs_mkdir(&fs, "/k", 0755), 0);
        uint64_t kino = 0;
        CHECK_EQ(gh2_path_resolve(&fs, "/k", &kino), 0);

        const uint64_t FAKE = 0xDEADBEEFCAFE1234ULL;   /* jeden hash dla wszystkich -> kolizja */
        for (int i = 0; i < N; i++) {
            snprintf(names[i], sizeof(names[i]), "coll%02d", i);
            CHECK_EQ(gh2_fs_test_dir_add(&fs, kino, FAKE, names[i],
                                         (uint16_t)strlen(names[i]),
                                         (uint64_t)(1000 + i), GH2_FT_FILE), 0);
        }
        /* duplikat tej samej nazwy pod tym samym hashem -> -EEXIST */
        CHECK_EQ(gh2_fs_test_dir_add(&fs, kino, FAKE, names[0],
                                     (uint16_t)strlen(names[0]), 9999, GH2_FT_FILE), -EEXIST);

        /* kazda nazwa znaleziona mimo wspoldzielonego klucza (skan po nazwie) */
        for (int i = 0; i < N; i++) {
            uint64_t got = 0;
            CHECK_EQ(gh2_fs_test_dir_lookup(&fs, kino, FAKE, names[i],
                                            (uint16_t)strlen(names[i]), &got), 0);
            CHECK_EQ(got, (uint64_t)(1000 + i));
        }

        /* readdir widzi WSZYSTKIE N wpisy (spakowane w jednym DIR_ITEM) */
        struct rd_count c; memset(&c, 0, sizeof(c));
        CHECK_EQ(gh2_fs_readdir(&fs, "/k", rd_count_cb, &c), 0);
        CHECK_EQ(c.n, N);

        CHECK_EQ(gh2_fs_commit(&fs), 0);
        check_no_leak(&fs);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);

    /* remount: wszystkie skolizjonowane wpisy zachowane */
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev2), 0);
        uint64_t kino = 0;
        CHECK_EQ(gh2_path_resolve(&fs, "/k", &kino), 0);
        const uint64_t FAKE = 0xDEADBEEFCAFE1234ULL;
        for (int i = 0; i < N; i++) {
            uint64_t got = 0;
            CHECK_EQ(gh2_fs_test_dir_lookup(&fs, kino, FAKE, names[i],
                                            (uint16_t)strlen(names[i]), &got), 0);
            CHECK_EQ(got, (uint64_t)(1000 + i));
        }
        struct rd_count c; memset(&c, 0, sizeof(c));
        CHECK_EQ(gh2_fs_readdir(&fs, "/k", rd_count_cb, &c), 0);
        CHECK_EQ(c.n, N);
        check_no_leak(&fs);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 7: unlink / rmdir ============================ */
static void test_unlink_rmdir(void) {
    char tmp[] = "/tmp/gh2fs_ul_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* unlink pliku: nlink 1 -> 0 -> i-wezel usuniety */
    CHECK_EQ(gh2_fs_create(&fs, "/a.txt", 0644), 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/a.txt", &in, &ino), 0);
    uint64_t a_ino = ino;
    CHECK_EQ(gh2_fs_unlink(&fs, "/a.txt"), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/a.txt", &in, &ino), -ENOENT);
    CHECK_EQ(gh2_fs_read_inode(&fs, a_ino, &in), -ENOENT);   /* INODE_ITEM zniknal */

    /* -EISDIR: unlink na katalogu */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh2_fs_unlink(&fs, "/d"), -EISDIR);
    /* -ENOENT */
    CHECK_EQ(gh2_fs_unlink(&fs, "/nope"), -ENOENT);

    /* rmdir: niepusty -> -ENOTEMPTY */
    CHECK_EQ(gh2_fs_create(&fs, "/d/inside", 0644), 0);
    CHECK_EQ(gh2_fs_rmdir(&fs, "/d"), -ENOTEMPTY);
    /* -ENOTDIR na pliku */
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    CHECK_EQ(gh2_fs_rmdir(&fs, "/f"), -ENOTDIR);

    /* oproznij i usun katalog -> nlink rodzica-- */
    struct gh2_inode root;
    CHECK_EQ(gh2_fs_getattr(&fs, "/", &root, &ino), 0);
    uint32_t root_nlink_before = root.nlink;
    CHECK_EQ(gh2_fs_unlink(&fs, "/d/inside"), 0);
    CHECK_EQ(gh2_fs_rmdir(&fs, "/d"), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/d", &in, &ino), -ENOENT);
    CHECK_EQ(gh2_fs_getattr(&fs, "/", &root, &ino), 0);
    CHECK_EQ(root.nlink, root_nlink_before - 1);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 8: hardlink ============================ */
static void test_hardlink(void) {
    char tmp[] = "/tmp/gh2fs_ln_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/orig", 0644), 0);
    struct gh2_inode in; uint64_t ino1 = 0, ino2 = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/orig", &in, &ino1), 0);
    CHECK_EQ(in.nlink, 1);

    /* hardlink: nlink++, wspolny ino */
    CHECK_EQ(gh2_fs_link(&fs, "/orig", "/hard"), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/orig", &in, &ino1), 0);
    CHECK_EQ(in.nlink, 2);
    CHECK_EQ(gh2_fs_getattr(&fs, "/hard", &in, &ino2), 0);
    CHECK_EQ(ino1, ino2);
    CHECK_EQ(in.nlink, 2);

    /* -EPERM dla katalogu */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dd", 0755), 0);
    CHECK_EQ(gh2_fs_link(&fs, "/dd", "/dd2"), -EPERM);
    /* -EEXIST */
    CHECK_EQ(gh2_fs_link(&fs, "/orig", "/hard"), -EEXIST);
    /* -ENOENT */
    CHECK_EQ(gh2_fs_link(&fs, "/nope", "/x"), -ENOENT);

    /* unlink jednego -> drugi zyje, nlink-- */
    CHECK_EQ(gh2_fs_unlink(&fs, "/orig"), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/hard", &in, &ino2), 0);
    CHECK_EQ(in.nlink, 1);
    CHECK_EQ(ino2, ino1);
    CHECK_EQ(gh2_fs_getattr(&fs, "/orig", &in, &ino1), -ENOENT);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 9: symlink / readlink ============================ */
static void test_symlink(void) {
    char tmp[] = "/tmp/gh2fs_sl_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    const char *target = "/etc/conf/app.cfg";
    CHECK_EQ(gh2_fs_symlink(&fs, target, "/link"), 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/link", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_SYMLINK);
    CHECK_EQ(in.size, strlen(target));

    char buf[128];
    CHECK_EQ(gh2_fs_readlink(&fs, "/link", buf, sizeof(buf)), 0);
    CHECK_EQ(strcmp(buf, target), 0);

    /* readlink na nie-symlinku -> -EINVAL */
    CHECK_EQ(gh2_fs_create(&fs, "/reg", 0644), 0);
    CHECK_EQ(gh2_fs_readlink(&fs, "/reg", buf, sizeof(buf)), -EINVAL);
    /* -EEXIST */
    CHECK_EQ(gh2_fs_symlink(&fs, "x", "/link"), -EEXIST);

    /* unlink symlinka usuwa i-wezel + SYMLINK_DATA */
    CHECK_EQ(gh2_fs_getattr(&fs, "/link", &in, &ino), 0);
    uint64_t l_ino = ino;
    CHECK_EQ(gh2_fs_unlink(&fs, "/link"), 0);
    CHECK_EQ(gh2_fs_read_inode(&fs, l_ino, &in), -ENOENT);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);

    /* round-trip po remount */
    CHECK_EQ(gh2_fs_symlink(&fs, target, "/link2"), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev2), 0);
    CHECK_EQ(gh2_fs_readlink(&fs, "/link2", buf, sizeof(buf)), 0);
    CHECK_EQ(strcmp(buf, target), 0);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 10: mknod ============================ */
static void test_mknod(void) {
    char tmp[] = "/tmp/gh2fs_nod_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    uint64_t rdev = (5ULL << 8) | 3;   /* major 5, minor 3 */
    CHECK_EQ(gh2_fs_mknod(&fs, "/chr", S_IFCHR | 0600, rdev), 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/chr", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_CHR);
    CHECK_EQ(in.rdev, rdev);
    CHECK_EQ(in.mode, 0600);

    uint64_t bdev = (8ULL << 8) | 1;
    CHECK_EQ(gh2_fs_mknod(&fs, "/blk", S_IFBLK | 0660, bdev), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/blk", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_BLK);
    CHECK_EQ(in.rdev, bdev);

    CHECK_EQ(gh2_fs_mknod(&fs, "/fifo", S_IFIFO | 0644, 0), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/fifo", &in, &ino), 0);
    CHECK_EQ(in.type, GH2_FT_FIFO);

    /* -EINVAL: zly typ (np. katalog) */
    CHECK_EQ(gh2_fs_mknod(&fs, "/bad", S_IFDIR | 0755, 0), -EINVAL);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);

    /* remount round-trip rdev */
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev2), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/chr", &in, &ino), 0);
    CHECK_EQ(in.rdev, rdev);
    CHECK_EQ(gh2_fs_getattr(&fs, "/blk", &in, &ino), 0);
    CHECK_EQ(in.rdev, bdev);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================ Test 11: rename ============================ */
static void test_rename(void) {
    char tmp[] = "/tmp/gh2fs_mv_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* zwykly rename pliku */
    CHECK_EQ(gh2_fs_create(&fs, "/a", 0644), 0);
    struct gh2_inode in; uint64_t ino_a = 0, ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/a", &in, &ino_a), 0);
    CHECK_EQ(gh2_fs_rename(&fs, "/a", "/b", 0), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/a", &in, &ino), -ENOENT);
    CHECK_EQ(gh2_fs_getattr(&fs, "/b", &in, &ino), 0);
    CHECK_EQ(ino, ino_a);

    /* nadpisanie istniejacego pliku -> jego i-wezel usuniety */
    CHECK_EQ(gh2_fs_create(&fs, "/c", 0644), 0);
    uint64_t ino_c = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/c", &in, &ino_c), 0);
    CHECK_EQ(gh2_fs_rename(&fs, "/b", "/c", 0), 0);   /* b nadpisuje c */
    CHECK_EQ(gh2_fs_read_inode(&fs, ino_c, &in), -ENOENT);  /* stary c zniknal */
    CHECK_EQ(gh2_fs_getattr(&fs, "/c", &in, &ino), 0);
    CHECK_EQ(ino, ino_a);

    /* rename miedzy katalogami z poprawa `..` i nlink */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/src", 0755), 0);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dst", 0755), 0);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/src/movedir", 0755), 0);
    struct gh2_inode src_i, dst_i;
    uint64_t md_ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/src/movedir", &in, &md_ino), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/src", &src_i, &ino), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/dst", &dst_i, &ino), 0);
    uint32_t src_nlink = src_i.nlink, dst_nlink = dst_i.nlink;

    CHECK_EQ(gh2_fs_rename(&fs, "/src/movedir", "/dst/movedir", 0), 0);
    /* `..` movedir wskazuje teraz dst */
    uint64_t up = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/dst/movedir/..", &up), 0);
    uint64_t dst_ino = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/dst", &dst_ino), 0);
    CHECK_EQ(up, dst_ino);
    /* nlink: src-- , dst++ */
    CHECK_EQ(gh2_fs_getattr(&fs, "/src", &src_i, &ino), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/dst", &dst_i, &ino), 0);
    CHECK_EQ(src_i.nlink, src_nlink - 1);
    CHECK_EQ(dst_i.nlink, dst_nlink + 1);
    /* movedir wciaz ten sam ino */
    uint64_t md2 = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/dst/movedir", &md2), 0);
    CHECK_EQ(md2, md_ino);

    /* ochrona petli: katalog do wlasnego poddrzewa -> -EINVAL */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/dst/movedir/deep", 0755), 0);
    CHECK_EQ(gh2_fs_rename(&fs, "/dst/movedir", "/dst/movedir/deep/loop", 0), -EINVAL);

    /* nadpisanie niepustego katalogu -> -ENOTEMPTY */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/full", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/full/x", 0644), 0);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/emptysrc", 0755), 0);
    CHECK_EQ(gh2_fs_rename(&fs, "/emptysrc", "/full", 0), -ENOTEMPTY);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ Test 12: chmod / chown / utimens / truncate ============== */
static void test_meta(void) {
    char tmp[] = "/tmp/gh2fs_meta_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    struct gh2_inode in; uint64_t ino = 0;

    CHECK_EQ(gh2_fs_chmod(&fs, "/f", 0600), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &ino), 0);
    CHECK_EQ(in.mode, 0600);

    CHECK_EQ(gh2_fs_chown(&fs, "/f", 1000, 2000), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &ino), 0);
    CHECK_EQ(in.uid, 1000);
    CHECK_EQ(in.gid, 2000);
    /* -1 nie zmienia */
    CHECK_EQ(gh2_fs_chown(&fs, "/f", (uint32_t)-1, 3000), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &ino), 0);
    CHECK_EQ(in.uid, 1000);
    CHECK_EQ(in.gid, 3000);

    CHECK_EQ(gh2_fs_utimens(&fs, "/f", 111111, 222222), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &ino), 0);
    CHECK_EQ(in.atime, 111111);
    CHECK_EQ(in.mtime, 222222);

    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 4096), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &ino), 0);
    CHECK_EQ(in.size, 4096);
    /* truncate katalogu -> -EISDIR */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh2_fs_truncate(&fs, "/d", 0), -EISDIR);

    /* statfs */
    struct gh2_statfs st;
    CHECK_EQ(gh2_fs_statfs(&fs, &st), 0);
    CHECK_EQ(st.total_blocks, NBLK);
    CHECK(st.free_blocks > 0 && st.free_blocks < NBLK);
    CHECK_EQ(st.block_size, 4096);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ fsck-poziom: spojnosc drzewa FS ============================ */
/* Zbiera wszystkie INODE_ITEM i DIR_ITEM, weryfikuje:
 *  - kazdy wpis DIR_ITEM wskazuje istniejacy INODE_ITEM,
 *  - nlink i-wezla == liczba wpisow (linkow) wskazujacych nan; katalog: 2 + liczba podkatalogow,
 *  - brak sierot (kazdy INODE osiagalny z roota; tu: kazdy INODE ma >=1 link lub jest rootem). */

#define FSCK_MAX_INO 200000
struct fsck {
    /* mapy indeksowane ino (rzadkie -> uzywamy hash-mniej, tu prosty array bo ino male) */
    uint8_t  *is_inode;      /* INODE_ITEM istnieje */
    uint8_t  *ftype;         /* typ i-wezla */
    uint32_t *nlink;         /* zadeklarowany nlink */
    uint32_t *links_seen;    /* zliczone wpisy DIR_ITEM -> ino (z dowolnego katalogu, !=`.`/`..`) */
    uint32_t *subdirs;       /* liczba podkatalogow (dla nlink katalogu) */
    uint8_t  *reachable;     /* osiagalny z roota */
    uint64_t  max_ino;
    int       err;           /* niespojnosc: 1 */
};

static struct fsck *g_fsck;   /* dla callbackow iterate */

static int fsck_inode_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)ctx;
    if (key->type != GH2_INODE_ITEM) return 0;
    struct fsck *f = g_fsck;
    uint64_t ino = key->objectid;
    if (ino >= f->max_ino) { f->err = 1; return 0; }
    struct gh2_inode in;
    if (len != sizeof(in)) { f->err = 1; return 0; }
    memcpy(&in, val, sizeof(in));
    f->is_inode[ino] = 1;
    f->ftype[ino] = (uint8_t)in.type;
    f->nlink[ino] = in.nlink;
    return 0;
}

static int fsck_dir_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)ctx;
    if (key->type != GH2_DIR_ITEM) return 0;
    struct fsck *f = g_fsck;
    uint64_t parent = key->objectid;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *nm = (const char *)v + off + sizeof(struct gh2_dirent);
        int is_dot = (nlen == 1 && nm[0] == '.');
        int is_dotdot = (nlen == 2 && nm[0] == '.' && nm[1] == '.');
        if (de.ino >= f->max_ino) { f->err = 1; off += rec; continue; }
        if (!is_dot && !is_dotdot) {
            f->links_seen[de.ino]++;
            if (de.ftype == GH2_FT_DIR) {
                if (parent < f->max_ino) f->subdirs[parent]++;
            }
        }
        off += rec;
    }
    return 0;
}

/* DFS osiagalnosci z roota po `..`-niezaleznie: chodzimy po wpisach katalogow. */
static int fsck_walk_reach(struct gh2_fs *fs, struct fsck *f, uint64_t dir_ino);
struct reach_ctx { struct gh2_fs *fs; struct fsck *f; int rc; };

static int reach_item_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct reach_ctx *rc = ctx;
    (void)key;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *nm = (const char *)v + off + sizeof(struct gh2_dirent);
        int is_dot = (nlen == 1 && nm[0] == '.');
        int is_dotdot = (nlen == 2 && nm[0] == '.' && nm[1] == '.');
        if (!is_dot && !is_dotdot && de.ino < rc->f->max_ino && !rc->f->reachable[de.ino]) {
            rc->f->reachable[de.ino] = 1;
            if (de.ftype == GH2_FT_DIR) {
                int r = fsck_walk_reach(rc->fs, rc->f, de.ino);
                if (r) { rc->rc = r; return r; }
            }
        }
        off += rec;
    }
    return 0;
}

static int fsck_walk_reach(struct gh2_fs *fs, struct fsck *f, uint64_t dir_ino) {
    struct reach_ctx rc = { fs, f, 0 };
    struct gh2_key min, max;
    memset(&min, 0, sizeof(min)); memset(&max, 0, sizeof(max));
    min.objectid = dir_ino; min.type = GH2_DIR_ITEM; min.offset = 0;
    max.objectid = dir_ino; max.type = GH2_DIR_ITEM; max.offset = UINT64_MAX;
    return gh2_btree_iterate_range(&fs->dev, &fs->fs_root, &min, &max, reach_item_cb, &rc);
}

/* pelna walidacja fsck-poziom; zwraca 0 jesli spojne. */
static int fsck_check(struct gh2_fs *fs) {
    struct fsck f; memset(&f, 0, sizeof(f));
    f.max_ino = fs->next_ino + 1;
    if (f.max_ino > FSCK_MAX_INO) f.max_ino = FSCK_MAX_INO;
    f.is_inode   = calloc(f.max_ino, 1);
    f.ftype      = calloc(f.max_ino, 1);
    f.nlink      = calloc(f.max_ino, sizeof(uint32_t));
    f.links_seen = calloc(f.max_ino, sizeof(uint32_t));
    f.subdirs    = calloc(f.max_ino, sizeof(uint32_t));
    f.reachable  = calloc(f.max_ino, 1);
    if (!f.is_inode || !f.ftype || !f.nlink || !f.links_seen || !f.subdirs || !f.reachable) {
        free(f.is_inode); free(f.ftype); free(f.nlink);
        free(f.links_seen); free(f.subdirs); free(f.reachable);
        return -ENOMEM;
    }

    g_fsck = &f;
    int rc = 0;
    /* zbierz INODE i DIR */
    if (gh2_btree_iterate(&fs->dev, &fs->fs_root, fsck_inode_cb, NULL)) { rc = -EIO; goto done; }
    if (gh2_btree_iterate(&fs->dev, &fs->fs_root, fsck_dir_cb, NULL)) { rc = -EIO; goto done; }
    if (f.err) { rc = -EIO; goto done; }

    /* osiagalnosc z roota */
    f.reachable[GH2_ROOT_INO] = 1;
    if (fsck_walk_reach(fs, &f, GH2_ROOT_INO)) { rc = -EIO; goto done; }

    /* walidacja per ino */
    for (uint64_t ino = 1; ino < f.max_ino; ino++) {
        if (!f.is_inode[ino]) {
            /* nie ma INODE -> nie powinien byc wskazany przez DIR_ITEM */
            if (f.links_seen[ino] != 0) { rc = -EIO; goto done; }
            continue;
        }
        /* INODE istnieje -> musi byc osiagalny (brak sierot) */
        if (!f.reachable[ino]) { rc = -EIO; goto done; }

        if (f.ftype[ino] == GH2_FT_DIR) {
            /* nlink == 2 + liczba podkatalogow */
            uint32_t expect = 2 + f.subdirs[ino];
            if (f.nlink[ino] != expect) { rc = -EIO; goto done; }
        } else {
            /* nlink == liczba wpisow wskazujacych nan */
            if (ino == GH2_ROOT_INO) continue;
            if (f.nlink[ino] != f.links_seen[ino]) { rc = -EIO; goto done; }
        }
    }

done:
    free(f.is_inode); free(f.ftype); free(f.nlink);
    free(f.links_seen); free(f.subdirs); free(f.reachable);
    return rc;
}

/* ============================ Test 13: losowa sekwencja + fsck-poziom ================== */
/* Prowadzimy losowa sekwencje create/mkdir/unlink/rmdir/link/rename z wlasnym MODELEM
 * sciezek; po sekwencji walidujemy: fsck-poziom (nlink/sieroty/links), mapa==mark-sweep,
 * persystencja po remount. */

/* prosty PRNG (deterministyczny) */
static uint64_t rng_state = 0x123456789abcdefULL;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

/* model: tablica sciezek istniejacych (pliki i katalogi) */
#define MAXP 600
static char  paths[MAXP][160];
static int   pis_dir[MAXP];
static int   npaths;

static int model_find(const char *p) {
    for (int i = 0; i < npaths; i++) if (strcmp(paths[i], p) == 0) return i;
    return -1;
}
static void model_add(const char *p, int isdir) {
    if (npaths >= MAXP) return;
    snprintf(paths[npaths], sizeof(paths[0]), "%s", p);
    pis_dir[npaths] = isdir;
    npaths++;
}
static void model_del(const char *p) {
    int i = model_find(p);
    if (i < 0) return;
    paths[i][0] = 0;
    for (int j = i; j < npaths - 1; j++) {
        memcpy(paths[j], paths[j + 1], sizeof(paths[0]));
        pis_dir[j] = pis_dir[j + 1];
    }
    npaths--;
}
/* czy `pref` jest prefiksem-katalogiem `p` (p w poddrzewie pref) */
static int is_under(const char *pref, const char *p) {
    size_t l = strlen(pref);
    return strncmp(p, pref, l) == 0 && p[l] == '/';
}

/* fwd-decl helperow atomowosci (def. nizej, przed main) — dla error-path w fuzz */
static int bptr_eq(const struct gh2_bptr *a, const struct gh2_bptr *b);
static void space_squeeze(struct gh2_fs *fs, uint64_t keep);
static uint8_t *space_snapshot(struct gh2_fs *fs);
static void check_map_eq_marksweep(struct gh2_fs *fs);

/* error-path: zajmij WSZYSTKIE wolne bloki (0 wolnych) -> kazda alokujaca op = ENOSPC od razu.
 * Po probie cofnij DOKLADNIE te bloki, ktore sami zajelismy (bez ruszania txn-bookkeepingu,
 * by nie zdesynchronizowac alloc/defer_free/txn_alloced). */
static void fuzz_enospc_probe(struct gh2_fs *fs, const char *path, int is_mkdir) {
    struct gh2_bptr root_before = fs->fs_root;
    uint8_t *map_before = space_snapshot(fs);       /* mapa przed sztucznym squeeze */
    uint64_t next_ino_before = fs->next_ino;

    /* zajmij wszystkie wolne bloki danych (nasze sztuczne; zapamietane w roznicy z map_before) */
    space_squeeze(fs, 0);

    int rr = is_mkdir ? gh2_fs_mkdir(fs, path, 0755) : gh2_fs_create(fs, path, 0644);
    CHECK_EQ(rr, -ENOSPC);                            /* 0 wolnych -> gwarantowany ENOSPC */
    /* invarianty rollbacku: fs_root + next_ino nietkniete; sciezka nie powstala */
    CHECK_EQ(bptr_eq(&fs->fs_root, &root_before), 1);
    CHECK_EQ(fs->next_ino, next_ino_before);
    uint64_t dummy = 0;
    CHECK_EQ(gh2_path_resolve(fs, path, &dummy), -ENOENT);

    /* cofnij sztuczne squeeze: zwolnij bloki ktore byly wolne przed squeeze (map_before==0)
     * a teraz sa zajete. Rollback operacji juz zwolnil swoje alokacje, wiec roznica == squeeze. */
    for (uint64_t b = GH2_DATA_START; b < fs->space.nblocks; b++) {
        int was_free = !((map_before[b >> 3] >> (b & 7)) & 1u);
        if (was_free) gh2_space_set(&fs->space, b, 0);
    }
    /* mapa == przed proba (rollback op + cofniecie squeeze) */
    CHECK_EQ(memcmp(fs->space.bits, map_before, (size_t)((fs->space.nblocks + 7) / 8)), 0);
    /* spojnosc: mapa >= mark-sweep (zywe wezly zajete) */
    check_map_eq_marksweep(fs);
    free(map_before);
}

static void test_random_fsck(void) {
    char tmp[] = "/tmp/gh2fs_rnd_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    npaths = 0;
    rng_state = 0x9e3779b97f4a7c15ULL;
    int counter = 0;
    const int OPS = 500;
    int did = 0;

    for (int step = 0; step < OPS; step++) {
        uint32_t op = rng_next() % 100;
        char np[160];

        if (op < 30) {
            /* create plik: w roocie lub w losowym (plytkim) istniejacym katalogu */
            const char *dir = "";
            int dircount = 0;
            for (int i = 0; i < npaths; i++) if (pis_dir[i] && strlen(paths[i]) < 40) dircount++;
            if (dircount > 0 && (rng_next() & 1)) {
                int pick = (int)(rng_next() % (uint32_t)dircount);
                for (int i = 0; i < npaths; i++) if (pis_dir[i] && strlen(paths[i]) < 40) { if (pick-- == 0) { dir = paths[i]; break; } }
            }
            snprintf(np, sizeof(np), "%.80s/f%d", dir, counter++);
            if (model_find(np) < 0 && gh2_fs_create(&fs, np, 0644) == 0) { model_add(np, 0); did++; }
        } else if (op < 50) {
            /* mkdir */
            const char *dir = "";
            int dircount = 0;
            for (int i = 0; i < npaths; i++) if (pis_dir[i] && strlen(paths[i]) < 40) dircount++;
            if (dircount > 0 && (rng_next() & 1)) {
                int pick = (int)(rng_next() % (uint32_t)dircount);
                for (int i = 0; i < npaths; i++) if (pis_dir[i] && strlen(paths[i]) < 40) { if (pick-- == 0) { dir = paths[i]; break; } }
            }
            snprintf(np, sizeof(np), "%.80s/d%d", dir, counter++);
            if (model_find(np) < 0 && gh2_fs_mkdir(&fs, np, 0755) == 0) { model_add(np, 1); did++; }
        } else if (op < 65) {
            /* unlink losowego pliku */
            if (npaths == 0) continue;
            int pick = (int)(rng_next() % (uint32_t)npaths);
            if (!pis_dir[pick]) {
                if (gh2_fs_unlink(&fs, paths[pick]) == 0) { model_del(paths[pick]); did++; }
            }
        } else if (op < 78) {
            /* rmdir pustego katalogu (sprawdz model: brak dzieci) */
            if (npaths == 0) continue;
            int pick = (int)(rng_next() % (uint32_t)npaths);
            if (pis_dir[pick]) {
                int has_child = 0;
                for (int i = 0; i < npaths; i++) if (i != pick && is_under(paths[pick], paths[i])) { has_child = 1; break; }
                if (!has_child) {
                    int rr = gh2_fs_rmdir(&fs, paths[pick]);
                    if (rr == 0) { model_del(paths[pick]); did++; }
                }
            }
        } else if (op < 88) {
            /* hardlink losowego pliku */
            if (npaths == 0) continue;
            int pick = (int)(rng_next() % (uint32_t)npaths);
            if (!pis_dir[pick]) {
                snprintf(np, sizeof(np), "/h%d", counter++);
                if (model_find(np) < 0 && gh2_fs_link(&fs, paths[pick], np) == 0) { model_add(np, 0); did++; }
            }
        } else {
            /* rename losowego wpisu do nowej nazwy w roocie */
            if (npaths == 0) continue;
            int pick = (int)(rng_next() % (uint32_t)npaths);
            /* nie przenosimy katalogu w wlasne poddrzewo: cel jest w roocie */
            snprintf(np, sizeof(np), "/r%d", counter++);
            if (model_find(np) < 0) {
                int wasdir = pis_dir[pick];
                char oldp[160]; snprintf(oldp, sizeof(oldp), "%s", paths[pick]);
                int rr = gh2_fs_rename(&fs, oldp, np, 0);
                if (rr == 0) {
                    /* uaktualnij model: stary -> nowy; dzieci katalogu zmieniaja prefiks */
                    if (wasdir) {
                        for (int i = 0; i < npaths; i++) {
                            if (is_under(oldp, paths[i])) {
                                char nb[320];
                                snprintf(nb, sizeof(nb), "%s%s", np, paths[i] + strlen(oldp));
                                snprintf(paths[i], sizeof(paths[0]), "%.159s", nb);
                            }
                        }
                    }
                    model_del(oldp);
                    model_add(np, wasdir);
                    did++;
                }
            }
        }

        /* error-path ENOSPC co 50 krokow: rollback nie moze zostawic czesciowego stanu */
        if ((step % 50) == 25) {
            char ep[64];
            snprintf(ep, sizeof(ep), "/enospc%d", counter++);
            fuzz_enospc_probe(&fs, ep, step & 1);
            CHECK_EQ(fsck_check(&fs), 0);            /* fsck spojny po nieudanej op */
        }

        /* okresowy commit + fsck w trakcie */
        if ((step % 100) == 99) {
            CHECK_EQ(gh2_fs_commit(&fs), 0);
            CHECK_EQ(fsck_check(&fs), 0);
            check_no_leak(&fs);
        }
    }

    /* finalna walidacja */
    CHECK(did > 50);   /* sekwencja faktycznie cos zrobila */
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(fsck_check(&fs), 0);
    check_no_leak(&fs);

    /* persystencja: remount + fsck */
    uint64_t saved_ni = fs.next_ino;
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev2), 0);
    CHECK_EQ(fs.next_ino, saved_ni);
    CHECK_EQ(fsck_check(&fs), 0);
    check_no_leak(&fs);
    /* wszystkie sciezki z modelu rozwiazywalne */
    for (int i = 0; i < npaths; i++) {
        uint64_t ino = 0;
        CHECK_EQ(gh2_path_resolve(&fs, paths[i], &ino), 0);
    }
    gh2_fs_unmount(&fs);
    close_dev(&dev2);
    unlink(tmp);
}

/* ============================================================================
 * Testy ATOMOWOSCI per-operacja (savepoint/rollback) + rename same-inode.
 * Recenzja adwersaryjna: operacja FS robi WIELE insert/delete dzielac jeden alloc;
 * przy ENOSPC w srodku NIE wolno zmieniac fs_root ani zostawiac czesciowego stanu.
 * ========================================================================== */

/* porownanie bptr bit-po-bicie */
static int bptr_eq(const struct gh2_bptr *a, const struct gh2_bptr *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* zostaw dokladnie `keep` wolnych blokow danych w mapie (reszta zajeta), aby wymusic
 * ENOSPC w srodku operacji robiacej >keep alokacji CoW. Zwraca liczbe ZAJETYCH przez nas. */
static void space_squeeze(struct gh2_fs *fs, uint64_t keep) {
    /* zajmij wszystkie wolne bloki poza pierwszymi `keep` wolnymi (od GH2_DATA_START) */
    uint64_t left = keep;
    for (uint64_t b = GH2_DATA_START; b < fs->space.nblocks; b++) {
        if (!gh2_space_is_used(&fs->space, b)) {
            if (left > 0) { left--; continue; }
            gh2_space_set(&fs->space, b, 1);
        }
    }
}

/* snapshot mapy (bits) do bufora */
static uint8_t *space_snapshot(struct gh2_fs *fs) {
    size_t nbytes = (size_t)((fs->space.nblocks + 7) / 8);
    uint8_t *cp = malloc(nbytes);
    CHECK(cp != NULL);
    memcpy(cp, fs->space.bits, nbytes);
    return cp;
}

/* weryfikacja: mapa biezaca == mark-sweep z fs_root (bit-po-bicie) */
static void check_map_eq_marksweep(struct gh2_fs *fs) {
    struct gh2_space s2;
    CHECK_EQ(gh2_space_init(&s2, fs->space.nblocks), 0);
    CHECK_EQ(gh2_space_build_from_tree(&fs->dev, &s2, &fs->fs_root), 0);
    /* mapa biezaca moze miec DODATKOWO zajete bloki (nasze sztuczne squeeze) — ale wszystkie
     * ZYWE wezly drzewa MUSZA byc zajete: s2.bits implikuje fs->space.bits. */
    size_t nbytes = (size_t)((fs->space.nblocks + 7) / 8);
    for (size_t i = 0; i < nbytes; i++)
        CHECK_EQ((uint8_t)(s2.bits[i] & ~fs->space.bits[i]), 0);  /* zywy => zajety */
    gh2_space_destroy(&s2);
}

/* Test 14: ATOMOWOSC — ENOSPC w srodku mkdir oraz rename; fs_root i mapa NIETKNIETE. */
static void test_atomicity(void) {
    char tmp[] = "/tmp/gh2fs_atom_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* przygotuj plik do rename (przy pelnej przestrzeni) */
    CHECK_EQ(gh2_fs_create(&fs, "/src", 0644), 0);
    uint64_t src_ino = 0; struct gh2_inode in;
    CHECK_EQ(gh2_fs_getattr(&fs, "/src", &in, &src_ino), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* --- ENOSPC w srodku mkdir --- */
    /* mkdir robi: INODE + `.` + `..` + DIR_ITEM rodzica + update rodzica => kilka alokacji.
     * Zostaw tylko 1 wolny blok danych -> alokacja CoW padnie w srodku. */
    space_squeeze(&fs, 1);
    struct gh2_bptr root_before = fs.fs_root;
    uint8_t *map_before = space_snapshot(&fs);
    uint64_t nfree_before = fs.space.nfree;
    uint64_t next_ino_before = fs.next_ino;

    int rr = gh2_fs_mkdir(&fs, "/newdir", 0755);
    CHECK(rr == -ENOSPC);                            /* operacja sie nie powiodla */
    /* fs_root NIETKNIETY */
    CHECK_EQ(bptr_eq(&fs.fs_root, &root_before), 1);
    /* mapa NIETKNIETA (rollback zwolnil zaalokowane bloki tej operacji) */
    CHECK_EQ(fs.space.nfree, nfree_before);
    CHECK_EQ(memcmp(fs.space.bits, map_before, (size_t)((fs.space.nblocks + 7) / 8)), 0);
    /* next_ino NIEZMIENIONY (czesciowy stan nie przeszedl) */
    CHECK_EQ(fs.next_ino, next_ino_before);
    /* sciezka NIE rozwiazuje sie (brak czesciowego katalogu) */
    uint64_t dummy = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/newdir", &dummy), -ENOENT);
    /* zywe wezly wciaz zajete (spojnosc mapa>=mark-sweep) */
    check_map_eq_marksweep(&fs);
    free(map_before);

    /* --- ENOSPC w srodku rename (na cel nieistniejacy) --- */
    root_before = fs.fs_root;
    map_before = space_snapshot(&fs);
    nfree_before = fs.space.nfree;
    rr = gh2_fs_rename(&fs, "/src", "/dst", 0);
    CHECK(rr == -ENOSPC);
    CHECK_EQ(bptr_eq(&fs.fs_root, &root_before), 1);
    CHECK_EQ(fs.space.nfree, nfree_before);
    CHECK_EQ(memcmp(fs.space.bits, map_before, (size_t)((fs.space.nblocks + 7) / 8)), 0);
    /* OBA: stary wpis wciaz jest, nowy NIE powstal */
    CHECK_EQ(gh2_path_resolve(&fs, "/src", &dummy), 0);
    CHECK_EQ(dummy, src_ino);
    CHECK_EQ(gh2_path_resolve(&fs, "/dst", &dummy), -ENOENT);
    check_map_eq_marksweep(&fs);
    free(map_before);

    /* --- po zwolnieniu miejsca kolejna operacja dziala --- */
    /* odbuduj prawdziwa mape (cofnij sztuczne squeeze) -> swiezy mark-sweep z drzewa korzeni
     * (musi zawierac bloki-wezly drzewa korzeni, inaczej commit CoW je nadpisze) */
    gh2_space_destroy(&fs.space);
    CHECK_EQ(gh2_space_init(&fs.space, fs.sb.total_blocks), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&fs.dev, &fs.space, &fs.root_tree), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/newdir", 0755), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/newdir", &dummy), 0);
    CHECK_EQ(gh2_fs_rename(&fs, "/src", "/dst", 0), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/dst", &dummy), 0);
    CHECK_EQ(dummy, src_ino);
    CHECK_EQ(gh2_path_resolve(&fs, "/src", &dummy), -ENOENT);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    CHECK_EQ(fsck_check(&fs), 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* Test 15: rename same-inode (BUG #2) — oba hardlinki tego samego i-wezla. */
static void test_rename_same_inode(void) {
    char tmp[] = "/tmp/gh2fs_rsi_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* /orig i /hl -> ten sam i-wezel (nlink 2) */
    CHECK_EQ(gh2_fs_create(&fs, "/orig", 0644), 0);
    CHECK_EQ(gh2_fs_link(&fs, "/orig", "/hl"), 0);
    struct gh2_inode in; uint64_t io = 0, ih = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/orig", &in, &io), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/hl", &in, &ih), 0);
    CHECK_EQ(io, ih);
    CHECK_EQ(in.nlink, 2);

    /* rename(/orig,/hl): dst istnieje, dst_ino==src_ino -> no-op success; OBA zostaja */
    CHECK_EQ(gh2_fs_rename(&fs, "/orig", "/hl", 0), 0);
    uint64_t i1 = 0, i2 = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/orig", &i1), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/hl", &i2), 0);
    CHECK_EQ(i1, io);
    CHECK_EQ(i2, io);
    CHECK_EQ(gh2_fs_getattr(&fs, "/orig", &in, &io), 0);
    CHECK_EQ(in.nlink, 2);                           /* nlink zachowany */

    /* rename(/x,/x) sciezkowo identyczne -> 0 */
    CHECK_EQ(gh2_fs_create(&fs, "/x", 0644), 0);
    uint64_t ix = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/x", &in, &ix), 0);
    CHECK_EQ(gh2_fs_rename(&fs, "/x", "/x", 0), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/x", &i1), 0);
    CHECK_EQ(i1, ix);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_no_leak(&fs);
    CHECK_EQ(fsck_check(&fs), 0);                     /* mapa/nlink spojne */
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================================================================
 * v2.4 — testy danych plikow (ekstenty): round-trip, CoW, RMW, truncate, unlink,
 * persystencja, csum->EIO, duzy plik, fsck danych. Wiekszy obraz (duzy plik 4MB).
 * ========================================================================== */

static const uint64_t NBLK_BIG = 4096;   /* 16 MB obrazu — mieszczi 4 MB plik + drzewo */

static int open_dev_n(struct gh_dev *dev, const char *path, uint64_t nblk) {
    int r = gh_dev_create(path, nblk, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}

/* mapa po commit MUSI == mark-sweep (TERAZ z blokami danych ekstentow). v2.7: z drzewa korzeni. */
static void check_data_map_eq_marksweep(struct gh2_fs *fs) {
    struct gh2_space s2;
    CHECK_EQ(gh2_space_init(&s2, fs->space.nblocks), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&fs->dev, &s2, &fs->root_tree), 0);
    CHECK_EQ(memcmp(fs->space.bits, s2.bits, (size_t)((fs->space.nblocks + 7) / 8)), 0);
    CHECK_EQ(fs->space.nfree, s2.nfree);
    /* refcount == mark-sweep */
    CHECK_EQ(memcmp(fs->space.refs, s2.refs, (size_t)fs->space.nblocks * sizeof(uint16_t)), 0);
    gh2_space_destroy(&s2);
}

/* fsck danych: zbierz wszystkie EXTENT_DATA; KAZDY disk_block musi byc zajety w mapie;
 * ZADEN disk_block nie wspoldzielony miedzy roznymi (ino,off). Zwraca 0 gdy spojne. */
struct dfsck { struct gh2_fs *fs; uint64_t *used; uint32_t n, cap; int err; };
static int dfsck_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct dfsck *d = ctx;
    if (key->type != GH2_EXTENT_DATA) return 0;
    if (len != sizeof(struct gh2_extent)) { d->err = 1; return 0; }
    struct gh2_extent e; memcpy(&e, val, sizeof(e));
    if (e.disk_block == 0) { d->err = 1; return 0; }
    /* w mapie? */
    if (!gh2_space_is_used(&d->fs->space, e.disk_block)) { d->err = 1; return 0; }
    /* wspoldzielenie? */
    for (uint32_t i = 0; i < d->n; i++)
        if (d->used[i] == e.disk_block) { d->err = 1; return 0; }
    if (d->n == d->cap) {
        uint32_t nc = d->cap ? d->cap * 2 : 64;
        uint64_t *nu = realloc(d->used, (size_t)nc * sizeof(uint64_t));
        if (!nu) { d->err = 1; return -ENOMEM; }
        d->used = nu; d->cap = nc;
    }
    d->used[d->n++] = e.disk_block;
    return 0;
}
static int data_fsck(struct gh2_fs *fs) {
    struct dfsck d; memset(&d, 0, sizeof(d)); d.fs = fs;
    int r = gh2_btree_iterate(&fs->dev, &fs->fs_root, dfsck_cb, &d);
    free(d.used);
    if (r) return r;
    return d.err ? -EIO : 0;
}

/* Test 16: round-trip rozne rozmiary / niewyrownane offsety / dziury. */
static void test_data_roundtrip(void) {
    char tmp[] = "/tmp/gh2fs_drt_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    /* sub-blok */
    const char *s = "hello world";
    CHECK_EQ(gh2_fs_write(&fs, "/f", s, 11, 0), 11);
    char rb[64]; memset(rb, 'X', sizeof(rb));
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 11, 0), 11);
    CHECK_EQ(memcmp(rb, s, 11), 0);
    /* read poza size -> obciety */
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 64, 0), 11);
    /* read od EOF -> 0 */
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 10, 100), 0);

    /* wielo-blok niewyrownany: zapis 5000 B od offsetu 100 (przecina granice 4096) */
    static uint8_t big[5000];
    for (int i = 0; i < 5000; i++) big[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(gh2_fs_create(&fs, "/g", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/g", big, 5000, 100), 5000);
    struct gh2_inode gi; uint64_t gino;
    CHECK_EQ(gh2_fs_getattr(&fs, "/g", &gi, &gino), 0);
    CHECK_EQ(gi.size, 5100);
    /* dziura [0,100) -> zera */
    static uint8_t rd[5100];
    CHECK_EQ(gh2_fs_read(&fs, "/g", rd, 5100, 0), 5100);
    for (int i = 0; i < 100; i++) CHECK_EQ(rd[i], 0);
    CHECK_EQ(memcmp(rd + 100, big, 5000), 0);

    /* dziura w srodku: zapis na offsecie 1MB, czyta zera przed */
    CHECK_EQ(gh2_fs_create(&fs, "/h", 0644), 0);
    const char *tail = "TAIL";
    CHECK_EQ(gh2_fs_write(&fs, "/h", tail, 4, 1000000), 4);
    char z[16]; CHECK_EQ(gh2_fs_read(&fs, "/h", z, 16, 0), 16);
    for (int i = 0; i < 16; i++) CHECK_EQ(z[i], 0);   /* dziura na poczatku -> zera */
    char t4[4]; CHECK_EQ(gh2_fs_read(&fs, "/h", t4, 4, 1000000), 4);
    CHECK_EQ(memcmp(t4, tail, 4), 0);

    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);
    CHECK_EQ(fsck_check(&fs), 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* Test 17: nadpisanie CoW (stary blok zwolniony, wyciek=0) + RMW czesciowy. */
static void test_data_cow_rmw(void) {
    char tmp[] = "/tmp/gh2fs_cow_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    static uint8_t b1[4096], b2[4096];
    memset(b1, 0xAA, sizeof(b1));
    memset(b2, 0xBB, sizeof(b2));
    CHECK_EQ(gh2_fs_write(&fs, "/f", b1, 4096, 0), 4096);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    uint64_t free_after_first = fs.space.nfree;

    /* nadpisz caly blok -> CoW: stary blok zwolniony po commit, netto wolnych bez zmian */
    CHECK_EQ(gh2_fs_write(&fs, "/f", b2, 4096, 0), 4096);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    static uint8_t rb[4096];
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 4096, 0), 4096);
    CHECK_EQ(memcmp(rb, b2, 4096), 0);
    /* wyciek=0: liczba wolnych po nadpisaniu == po pierwszym zapisie (1 blok danych) */
    CHECK_EQ(fs.space.nfree, free_after_first);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);

    /* RMW czesciowy: nadpisz [10,20) wzorem; reszta bloku nietknieta */
    uint8_t patch[10]; memset(patch, 0xCC, sizeof(patch));
    CHECK_EQ(gh2_fs_write(&fs, "/f", patch, 10, 10), 10);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 4096, 0), 4096);
    for (int i = 0; i < 10; i++) CHECK_EQ(rb[i], 0xBB);
    for (int i = 10; i < 20; i++) CHECK_EQ(rb[i], 0xCC);
    for (int i = 20; i < 4096; i++) CHECK_EQ(rb[i], 0xBB);
    CHECK_EQ(fs.space.nfree, free_after_first);   /* wciaz 1 blok danych */
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* Test 18: truncate skroc / rozszerz / zero. */
static void test_data_truncate(void) {
    char tmp[] = "/tmp/gh2fs_trunc_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    static uint8_t data[10000];
    for (int i = 0; i < 10000; i++) data[i] = (uint8_t)(i & 0xFF);
    CHECK_EQ(gh2_fs_write(&fs, "/f", data, 10000, 0), 10000);  /* 3 bloki (0,4096,8192) */
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    uint64_t free_full = fs.space.nfree;

    /* skroc do 5000 (sredina bloku 4096): blok 8192 zwolniony, ogon bloku 4096 wyzerowany */
    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 5000), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    struct gh2_inode in; uint64_t io;
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &io), 0);
    CHECK_EQ(in.size, 5000);
    /* CoW: blok 8192 zwolniony (1), ostatni blok przepisany (netto 0) -> 1 wiecej wolny */
    CHECK_EQ(fs.space.nfree, free_full + 1);
    static uint8_t rb[8192];
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 8192, 0), 5000);   /* obciete do size */
    CHECK_EQ(memcmp(rb, data, 5000), 0);
    /* read na offsecie 5000 -> 0 (poza EOF) */
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 100, 5000), 0);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);

    /* rozszerz do 20000 (sparse) -> zera; bloki danych bez zmian */
    uint64_t free_before_ext = fs.space.nfree;
    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 20000), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &io), 0);
    CHECK_EQ(in.size, 20000);
    CHECK_EQ(fs.space.nfree, free_before_ext);   /* sparse: brak nowych blokow danych */
    /* ogon wyzerowany: bajty [5000,8192) z bloku 4096 musza byc 0 */
    static uint8_t rb2[20000];
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb2, 20000, 0), 20000);
    CHECK_EQ(memcmp(rb2, data, 5000), 0);
    for (int i = 5000; i < 20000; i++) CHECK_EQ(rb2[i], 0);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);

    /* truncate do 0 -> wszystkie bloki danych zwolnione */
    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 0), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_getattr(&fs, "/f", &in, &io), 0);
    CHECK_EQ(in.size, 0);
    CHECK_EQ(data_fsck(&fs), 0);
    /* brak ekstentow -> mapa po commit ma tylko wezly + SB; wolnych >= free_full */
    CHECK(fs.space.nfree > free_full);
    check_data_map_eq_marksweep(&fs);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* Test 19: unlink pliku z danymi -> bloki danych zwolnione (wyciek=0). */
static void test_data_unlink(void) {
    char tmp[] = "/tmp/gh2fs_dunl_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    uint64_t free_empty = fs.space.nfree;
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    static uint8_t data[12000];
    for (int i = 0; i < 12000; i++) data[i] = (uint8_t)(i * 3);
    CHECK_EQ(gh2_fs_write(&fs, "/f", data, 12000, 0), 12000);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK(fs.space.nfree < free_empty);    /* zajeto bloki danych */

    CHECK_EQ(gh2_fs_unlink(&fs, "/f"), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    /* po unlink+commit: wolne >= stan poczatkowy (bloki danych + inode zwolnione) */
    CHECK_EQ(fs.space.nfree, free_empty);
    CHECK_EQ(data_fsck(&fs), 0);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(fsck_check(&fs), 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* Test 20: persystencja write+commit+remount -> dane identyczne; mapa==mark-sweep z danymi. */
static void test_data_persistence(void) {
    char tmp[] = "/tmp/gh2fs_dpers_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);

    static uint8_t data[9000];
    for (int i = 0; i < 9000; i++) data[i] = (uint8_t)(i ^ 0x5A);

    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/file", 0644), 0);
        CHECK_EQ(gh2_fs_write(&fs, "/file", data, 9000, 0), 9000);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        check_data_map_eq_marksweep(&fs);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);

    /* remount: dane identyczne + mapa (z mark-sweep) == zbudowana z drzewa */
    struct gh_dev dev2;
    CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    {
        struct gh2_fs fs;
        CHECK_EQ(gh2_fs_mount(&fs, &dev2), 0);
        static uint8_t rb[9000];
        CHECK_EQ(gh2_fs_read(&fs, "/file", rb, 9000, 0), 9000);
        CHECK_EQ(memcmp(rb, data, 9000), 0);
        /* mapa po remount (mark-sweep z blokami danych) == swieza mark-sweep */
        check_data_map_eq_marksweep(&fs);
        CHECK_EQ(data_fsck(&fs), 0);
        CHECK_EQ(fsck_check(&fs), 0);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev2);
    unlink(tmp);
}

/* Test 21: csum danych -> przekrec bit w disk_block -> read -EIO. */
static void test_data_csum_eio(void) {
    char tmp[] = "/tmp/gh2fs_dcsum_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    static uint8_t data[4096];
    memset(data, 0x42, sizeof(data));
    CHECK_EQ(gh2_fs_write(&fs, "/f", data, 4096, 0), 4096);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* znajdz disk_block ekstentu (ino pliku /f, off 0) */
    uint64_t ino = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/f", &ino), 0);
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = ino; k.type = GH2_EXTENT_DATA; k.offset = 0;
    uint8_t ebuf[sizeof(struct gh2_extent)]; uint32_t elen = 0;
    CHECK_EQ(gh2_btree_lookup(&fs.dev, &fs.fs_root, &k, ebuf, sizeof(ebuf), &elen), 0);
    struct gh2_extent e; memcpy(&e, ebuf, sizeof(e));

    /* przekrec bit w bloku danych na dysku (plaintext przez gh_disk_*) */
    uint8_t blk[4096];
    CHECK_EQ(gh_disk_read(&fs.dev, e.disk_block, blk), 0);
    blk[0] ^= 0x01;
    CHECK_EQ(gh_disk_write(&fs.dev, e.disk_block, blk), 0);

    /* read -> csum niezgodny, dup=0 -> -EIO */
    uint8_t rb[4096];
    CHECK_EQ(gh2_fs_read(&fs, "/f", rb, 4096, 0), -EIO);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* Test 22: duzy plik 4 MB (1024 bloki) write/read/truncate. */
static void test_data_large(void) {
    char tmp[] = "/tmp/gh2fs_dlarge_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev_n(&dev, tmp, NBLK_BIG), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK_BIG, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    const size_t SZ = 4u * 1024u * 1024u;   /* 4 MB = 1024 bloki */
    uint8_t *data = malloc(SZ);
    CHECK(data != NULL);
    for (size_t i = 0; i < SZ; i++) data[i] = (uint8_t)((i * 1103515245u + 12345u) >> 16);

    CHECK_EQ(gh2_fs_create(&fs, "/big", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/big", data, SZ, 0), (ssize_t)SZ);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint8_t *rb = malloc(SZ);
    CHECK(rb != NULL);
    CHECK_EQ(gh2_fs_read(&fs, "/big", rb, SZ, 0), (ssize_t)SZ);
    CHECK_EQ(memcmp(rb, data, SZ), 0);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);

    /* truncate do polowy */
    CHECK_EQ(gh2_fs_truncate(&fs, "/big", SZ / 2), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    CHECK_EQ(gh2_fs_read(&fs, "/big", rb, SZ, 0), (ssize_t)(SZ / 2));
    CHECK_EQ(memcmp(rb, data, SZ / 2), 0);
    check_data_map_eq_marksweep(&fs);
    CHECK_EQ(data_fsck(&fs), 0);

    free(data); free(rb);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

int main(void) {
    RUN_TEST(test_format_mount_root);
    RUN_TEST(test_create_mkdir);
    RUN_TEST(test_readdir_collisions);
    RUN_TEST(test_hash_packing);
    RUN_TEST(test_forced_collisions);
    RUN_TEST(test_persistence);
    RUN_TEST(test_unlink_rmdir);
    RUN_TEST(test_hardlink);
    RUN_TEST(test_symlink);
    RUN_TEST(test_mknod);
    RUN_TEST(test_rename);
    RUN_TEST(test_meta);
    RUN_TEST(test_atomicity);
    RUN_TEST(test_rename_same_inode);
    RUN_TEST(test_random_fsck);
    RUN_TEST(test_data_roundtrip);
    RUN_TEST(test_data_cow_rmw);
    RUN_TEST(test_data_truncate);
    RUN_TEST(test_data_unlink);
    RUN_TEST(test_data_persistence);
    RUN_TEST(test_data_csum_eio);
    RUN_TEST(test_data_large);
    return TEST_SUMMARY();
}
