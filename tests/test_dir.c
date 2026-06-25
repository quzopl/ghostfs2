#include "test.h"
#include "../src/block.h"
#include "../src/super.h"
#include "../src/inode.h"
#include "../src/dir.h"
#include "../src/csum.h"
#include "../src/ghostfs.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void test_add_lookup_remove(void) {
    char tmp[] = "/tmp/ghost_dirXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 512, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t fino; gh_inode_alloc(&dev, &sb, GH_FILE, &fino);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "plik.txt", fino), 0);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "plik.txt", fino), -EEXIST);

    uint64_t got;
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "plik.txt", &got), 0);
    CHECK_EQ(got, fino);
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "nie ma", &got), -ENOENT);

    CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, "plik.txt"), 0);
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "plik.txt", &got), -ENOENT);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_nested_path(void) {
    char tmp[] = "/tmp/ghost_dir2XXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 512, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t a, b;
    gh_inode_alloc(&dev, &sb, GH_DIR, &a);
    gh_dir_add(&dev, &sb, GH_ROOT_INO, "a", a);
    gh_inode_alloc(&dev, &sb, GH_DIR, &b);
    gh_dir_add(&dev, &sb, a, "b", b);

    uint64_t got;
    CHECK_EQ(gh_path_resolve(&dev, &sb, "/a/b", &got), 0);
    CHECK_EQ(got, b);
    CHECK_EQ(gh_path_resolve(&dev, &sb, "/", &got), 0);
    CHECK_EQ(got, GH_ROOT_INO);

    char parent[1024], name[256];
    CHECK_EQ(gh_path_split("/a/b/c", parent, name), 0);
    CHECK_EQ(strcmp(parent, "/a/b"), 0);
    CHECK_EQ(strcmp(name, "c"), 0);
    gh_dev_close(&dev); unlink(tmp);
}

static void test_prefix_tombstone_isempty(void) {
    /* --- container 1: prefix non-match + tombstone reuse --- */
    char tmp[] = "/tmp/ghost_dir3XXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 512, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    /* 1. Prefix non-match */
    uint64_t fino; gh_inode_alloc(&dev, &sb, GH_FILE, &fino);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "ab", fino), 0);
    uint64_t got;
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "abc", &got), -ENOENT);
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "ab", &got), 0);
    CHECK_EQ(got, fino);

    /* 2. Tombstone reuse */
    uint64_t ix, iy, iz;
    gh_inode_alloc(&dev, &sb, GH_FILE, &ix);
    gh_inode_alloc(&dev, &sb, GH_FILE, &iy);
    gh_inode_alloc(&dev, &sb, GH_FILE, &iz);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "x", ix), 0);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "y", iy), 0);
    CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, "x"), 0);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "z", iz), 0);
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "z", &got), 0);
    CHECK_EQ(got, iz);
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "x", &got), -ENOENT);

    gh_dev_close(&dev); unlink(tmp);

    /* --- container 2: is_empty --- */
    char tmp2[] = "/tmp/ghost_dir4XXXXXX"; int fd2 = mkstemp(tmp2); close(fd2);
    CHECK_EQ(gh_format(tmp2, 512, 64), 0);
    struct gh_dev dev2; struct gh_superblock sb2;
    gh_dev_open(tmp2, &dev2); gh_mount_sb(&dev2, &sb2);

    /* 3. is_empty: fresh root (only . and ..) must be empty */
    int empty;
    CHECK_EQ(gh_dir_is_empty(&dev2, &sb2, GH_ROOT_INO, &empty), 0);
    CHECK_EQ(empty, 1);

    /* after adding a file it must be non-empty */
    uint64_t fno; gh_inode_alloc(&dev2, &sb2, GH_FILE, &fno);
    CHECK_EQ(gh_dir_add(&dev2, &sb2, GH_ROOT_INO, "f", fno), 0);
    CHECK_EQ(gh_dir_is_empty(&dev2, &sb2, GH_ROOT_INO, &empty), 0);
    CHECK_EQ(empty, 0);

    gh_dev_close(&dev2); unlink(tmp2);
}

static void test_path_split_long_parent(void) {
    char parent[1024], name[256];
    char path[2048];
    memset(path, 'a', sizeof(path));
    path[0] = '/';
    /* czesc-rodzic ~2045 znakow (>1023 B), potem '/x' */
    path[2045] = '/'; path[2046] = 'x'; path[2047] = '\0';
    CHECK_EQ(gh_path_split(path, parent, name), -ENAMETOOLONG);
}

/* (a) kolizje: wiele nazw o tym samym crc%INIT_SLOTS -> wszystkie znajdowane */
static void test_hash_collisions(void) {
    char tmp[] = "/tmp/ghost_dircXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 512, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    /* zbierz nazwy ktorych crc32 % GH_DIR_INIT_SLOTS == ten sam bucket */
    char names[6][16]; int n = 0; uint64_t inos[6];
    for (int i = 0; i < 100000 && n < 6; i++) {
        char cand[16]; snprintf(cand, sizeof(cand), "c%d", i);
        if (gh_crc32(cand, strlen(cand)) % GH_DIR_INIT_SLOTS == 3) {
            strcpy(names[n], cand); n++;
        }
    }
    CHECK_EQ(n, 6);   /* na pewno znajdziemy 6 kolidujacych */
    for (int i = 0; i < n; i++) {
        gh_inode_alloc(&dev, &sb, GH_FILE, &inos[i]);
        CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, names[i], inos[i]), 0);
    }
    for (int i = 0; i < n; i++) {
        uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, names[i], &got), 0);
        CHECK_EQ(got, inos[i]);
    }
    /* usun srodkowy (tombston w lancuchu sondowania) -> reszta nadal znajdowana */
    CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, names[2]), 0);
    uint64_t got;
    CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, names[2], &got), -ENOENT);
    for (int i = 0; i < n; i++) {
        if (i == 2) continue;
        CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, names[i], &got), 0);
        CHECK_EQ(got, inos[i]);
    }
    gh_dev_close(&dev); unlink(tmp);
}

/* (b) tombston: add, remove, ponowne add tej samej nazwy -> OK i lookup zwraca nowe ino */
static void test_hash_tombstone_reuse(void) {
    char tmp[] = "/tmp/ghost_dirtXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 512, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t a, b; gh_inode_alloc(&dev, &sb, GH_FILE, &a); gh_inode_alloc(&dev, &sb, GH_FILE, &b);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "reuse", a), 0);
    CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, "reuse"), 0);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "reuse", b), 0);
    uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "reuse", &got), 0);
    CHECK_EQ(got, b);
    gh_dev_close(&dev); unlink(tmp);
}

/* iterate licznik */
static int count_cb(const struct gh_dirent *de, void *ctx) {
    (void)de; (*(int*)ctx)++; return 0;
}

/* (c) wzrost: dodaj 20 wpisow (>16*3/4=12) -> rehash, wszystkie nadal lookup-owalne */
static void test_hash_grow(void) {
    char tmp[] = "/tmp/ghost_dirgXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    char nm[16]; uint64_t inos[20];
    for (int i = 0; i < 20; i++) {
        snprintf(nm, sizeof(nm), "g%d", i);
        gh_inode_alloc(&dev, &sb, GH_FILE, &inos[i]);
        CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, nm, inos[i]), 0);
    }
    for (int i = 0; i < 20; i++) {
        snprintf(nm, sizeof(nm), "g%d", i);
        uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, nm, &got), 0);
        CHECK_EQ(got, inos[i]);
    }
    int empty; CHECK_EQ(gh_dir_is_empty(&dev, &sb, GH_ROOT_INO, &empty), 0);
    CHECK_EQ(empty, 0);
    /* iterate widzi . , .. oraz 20 plikow = 22 */
    int cnt = 0; CHECK_EQ(gh_dir_iterate(&dev, &sb, GH_ROOT_INO, count_cb, &cnt), 0);
    CHECK_EQ(cnt, 22);
    gh_dev_close(&dev); unlink(tmp);
}

/* (d) gh_dir_set_ino zmienia ino i lookup zwraca nowe */
static void test_hash_set_ino(void) {
    char tmp[] = "/tmp/ghost_dirsXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 512, 64), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t a, b; gh_inode_alloc(&dev, &sb, GH_FILE, &a); gh_inode_alloc(&dev, &sb, GH_FILE, &b);
    CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, "ent", a), 0);
    CHECK_EQ(gh_dir_set_ino(&dev, &sb, GH_ROOT_INO, "ent", b), 0);
    uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, "ent", &got), 0);
    CHECK_EQ(got, b);
    CHECK_EQ(gh_dir_set_ino(&dev, &sb, GH_ROOT_INO, "brak", b), -ENOENT);
    gh_dev_close(&dev); unlink(tmp);
}

/* (e) churn: 2000 cykli add f{i} / remove f{i-1} (max ~2 zywe) -> katalog NIE puchnie.
 * Na koncu nslots musi byc MALE (tombstony czyszczone przez rehash-in-place). */
static void test_dir_churn(void) {
    char tmp[] = "/tmp/ghost_dirchurnXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 1024, 128), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);

    uint64_t fino; gh_inode_alloc(&dev, &sb, GH_FILE, &fino);
    char nm[32], prev[32];
    for (int i = 0; i < 2000; i++) {
        snprintf(nm, sizeof(nm), "f%d", i);
        CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, nm, fino), 0);
        if (i > 0) {
            snprintf(prev, sizeof(prev), "f%d", i - 1);
            CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, prev), 0);
        }
    }
    /* zywe naraz: '.' , '..' oraz ostatnia f{1999} = 3 -> lookup ostatniej OK */
    snprintf(nm, sizeof(nm), "f%d", 1999);
    uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, nm, &got), 0);
    CHECK_EQ(got, fino);

    /* dowod: nslots katalogu pozostalo MALE (nie spuchlo do tysiecy) */
    struct gh_inode dir; CHECK_EQ(gh_inode_read(&dev, &sb, GH_ROOT_INO, &dir), 0);
    struct gh_dirhdr h;
    CHECK_EQ(gh_inode_pread(&dev, &sb, &dir, &h, sizeof(h), 0), (ssize_t)sizeof(h));
    CHECK_EQ(h.magic == GH_DIRHDR_MAGIC, 1);
    CHECK_EQ(h.nslots <= 64, 1);

    gh_dev_close(&dev); unlink(tmp);
}

/* (f) skala: 1000 wpisow, wszystkie lookup; usun parzyste; nieparzyste znajdowane,
 * parzyste -ENOENT. Szybki = dowod braku O(n^2). */
static void test_dir_scale(void) {
    char tmp[] = "/tmp/ghost_dscXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format(tmp, 8192, 2048), 0);
    struct gh_dev dev; struct gh_superblock sb;
    gh_dev_open(tmp, &dev); gh_mount_sb(&dev, &sb);
    /* 1000 wpisow */
    char nm[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "plik%d", i);
        uint64_t fino; CHECK_EQ(gh_inode_alloc(&dev, &sb, GH_FILE, &fino), 0);
        CHECK_EQ(gh_dir_add(&dev, &sb, GH_ROOT_INO, nm, fino), 0);
    }
    /* wszystkie znajdowane */
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "plik%d", i);
        uint64_t got; CHECK_EQ(gh_dir_lookup(&dev, &sb, GH_ROOT_INO, nm, &got), 0);
    }
    /* usun parzyste; nieparzyste nadal znajdowane, parzyste -ENOENT */
    for (int i = 0; i < 1000; i += 2) { snprintf(nm, sizeof(nm), "plik%d", i); CHECK_EQ(gh_dir_remove(&dev, &sb, GH_ROOT_INO, nm), 0); }
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "plik%d", i);
        uint64_t got; int r = gh_dir_lookup(&dev, &sb, GH_ROOT_INO, nm, &got);
        CHECK_EQ(r, (i % 2 == 0) ? -ENOENT : 0);
    }
    gh_dev_close(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_add_lookup_remove);
    RUN_TEST(test_nested_path);
    RUN_TEST(test_prefix_tombstone_isempty);
    RUN_TEST(test_path_split_long_parent);
    RUN_TEST(test_hash_collisions);
    RUN_TEST(test_hash_tombstone_reuse);
    RUN_TEST(test_hash_grow);
    RUN_TEST(test_hash_set_ino);
    RUN_TEST(test_dir_churn);
    RUN_TEST(test_dir_scale);
    return TEST_SUMMARY();
}
