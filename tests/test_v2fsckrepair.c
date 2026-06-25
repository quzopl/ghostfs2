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
 * ghostfs v2 — fsck --repair (analog v1 sub-projekt N, dla drzewa CoW v2).
 *
 * Dowodzimy: gh2_fsck(repair=1) naprawia problemy POZIOMU DRZEWA (wiszace DIR_ITEM,
 * sieroty, zly nlink) TRWALE (po commit+remount fsck==0), atomowo (ENOSPC -> nietkniete),
 * idempotentnie (czysty FS -> 0), z odzyskiem blokow sieroty (wyciek=0) i bez naruszenia
 * zdrowych danych (bajt-exact).
 * ========================================================================== */

static const uint64_t NBLK = 8192;

static int open_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_create(path, NBLK, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static int open_dev_n(struct gh_dev *dev, const char *path, uint64_t nblk) {
    int r = gh_dev_create(path, nblk, dev);
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

/* wzorzec danych zalezny od ino-seeda (rozpoznawalny, bajt-exact) */
static void fill_pattern(uint8_t *buf, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 131u + seed * 7u) ^ (i >> 5));
}

/* ====================================================================
 * Test 1: wiszacy DIR_ITEM (wpis -> ino bez INODE_ITEM) -> repair usuwa.
 * ==================================================================== */
static void test_dangling_entry(void) {
    char tmp[] = "/tmp/gh2fsckrep_dangle_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev; CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/dir", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/dir/real", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    /* wstrzyknij wiszacy wpis -> ino bez INODE_ITEM */
    uint64_t dino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/dir", &dino), 0);
    CHECK_EQ(gh2_fs_test_dir_add(&fs, dino, 0x1122334455667788ULL, "ghost", 5, 999999ULL, GH2_FT_FILE), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* read-only fsck wykrywa */
    issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);

    /* repair + commit */
    int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0); CHECK(detected > 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* TRWALOSC: remount swiezy -> fsck==0 */
    gh2_fs_unmount(&fs); close_dev(&dev);
    struct gh_dev dev2; CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2; CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    /* zdrowy plik wciaz osiagalny */
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs2, "/dir/real", &in, &ino), 0);

    gh2_fs_unmount(&fs2); close_dev(&dev2); unlink(tmp);
}

/* ====================================================================
 * Test 2: sierota (INODE_ITEM nieosiagalny) -> repair zwalnia i-wezel
 * + WSZYSTKIE jego bloki danych (wyciek=0); zdrowe dane nienaruszone.
 * ==================================================================== */
#define DLEN (12 * 4096)   /* ~12 blokow danych */

/* zbuduj zdrowy FS: /keep (dane), /victim (dane). Zwraca free_blocks po commit+remount. */
static void build_two_files(const char *path, uint8_t *keep_data, uint8_t *victim_data) {
    struct gh_dev dev; CHECK_EQ(open_dev(&dev, path), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/keep", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/keep", keep_data, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(gh2_fs_create(&fs, "/victim", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/victim", victim_data, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    gh2_fs_unmount(&fs); close_dev(&dev);
}

static void test_orphan_freed(void) {
    char tmp[]  = "/tmp/gh2fsckrep_orph_XXXXXX";
    char ref[]  = "/tmp/gh2fsckrep_ref_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    fd = mkstemp(ref); CHECK(fd >= 0); close(fd);

    uint8_t *keep   = malloc(DLEN), *victim = malloc(DLEN);
    uint8_t *rd     = malloc(DLEN);
    CHECK(keep && victim && rd);
    fill_pattern(keep, DLEN, 11);
    fill_pattern(victim, DLEN, 99);

    /* REFERENCJA: FS z TYLKO /keep (jak po pelnym usunieciu /victim) -> free_blocks wzorcowe */
    {
        struct gh_dev dev; CHECK_EQ(open_dev(&dev, ref), 0);
        CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
        struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/keep", 0644), 0);
        CHECK_EQ(gh2_fs_write(&fs, "/keep", keep, DLEN, 0), (ssize_t)DLEN);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        gh2_fs_unmount(&fs); close_dev(&dev);
    }
    uint64_t ref_free = 0;
    {
        struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, ref), 0);
        struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        struct gh2_statfs st; CHECK_EQ(gh2_fs_statfs(&fs, &st), 0);
        ref_free = st.free_blocks;
        gh2_fs_unmount(&fs); close_dev(&dev);
    }

    /* CEL: /keep + /victim, potem osierocenie /victim (usun wpis, zostaw INODE+ekstenty) */
    build_two_files(tmp, keep, victim);
    {
        struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, tmp), 0);
        struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        uint64_t vino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/victim", &vino), 0);
        /* usun wpis /victim z roota -> INODE_ITEM staje sie sierota (dane wciaz w drzewie) */
        CHECK_EQ(gh2_fs_test_dir_remove(&fs, GH2_ROOT_INO, "victim", 6), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        gh2_fs_unmount(&fs); close_dev(&dev);
    }

    /* remount: fsck wykrywa sierote; repair + commit */
    {
        struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, tmp), 0);
        struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);
        int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0); CHECK(detected > 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        gh2_fs_unmount(&fs); close_dev(&dev);
    }

    /* TRWALOSC + WYCIEK=0 + zdrowe dane bajt-exact */
    {
        struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, tmp), 0);
        struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
        int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
        /* sierota zniknela */
        struct gh2_inode in; uint64_t ino = 0;
        CHECK_EQ(gh2_fs_getattr(&fs, "/victim", &in, &ino), -ENOENT);
        /* zdrowy /keep bajt-exact */
        memset(rd, 0, DLEN);
        CHECK_EQ(gh2_fs_read(&fs, "/keep", rd, DLEN, 0), (ssize_t)DLEN);
        CHECK_EQ(memcmp(rd, keep, DLEN), 0);
        /* WYCIEK=0: free_blocks == referencja (bloki sieroty odzyskane) */
        struct gh2_statfs st; CHECK_EQ(gh2_fs_statfs(&fs, &st), 0);
        CHECK_EQ(st.free_blocks, ref_free);
        gh2_fs_unmount(&fs); close_dev(&dev);
    }

    free(keep); free(victim); free(rd);
    unlink(tmp); unlink(ref);
}

/* ====================================================================
 * Test 3: zly nlink (plik i katalog) -> repair ustawia poprawny.
 * ==================================================================== */
static void test_bad_nlink(void) {
    char tmp[] = "/tmp/gh2fsckrep_nlink_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev; CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d/sub", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/d/f", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t dino = 0, fino = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/d", &dino), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/d/f", &fino), 0);

    /* wymus zle nlink: katalog /d ma 1 podkatalog -> oczekiwane 3; ustaw 7. plik /f -> 1; ustaw 5. */
    CHECK_EQ(gh2_fs_test_set_nlink(&fs, dino, 7), 0);
    CHECK_EQ(gh2_fs_test_set_nlink(&fs, fino, 5), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues >= 2);
    int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0); CHECK(detected >= 2);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* TRWALOSC: remount -> fsck==0, nlink poprawne */
    gh2_fs_unmount(&fs); close_dev(&dev);
    struct gh_dev dev2; CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2; CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs2, "/d", &in, &ino), 0);
    CHECK_EQ(in.nlink, 3);   /* 2 + 1 podkatalog */
    CHECK_EQ(gh2_fs_getattr(&fs2, "/d/f", &in, &ino), 0);
    CHECK_EQ(in.nlink, 1);
    gh2_fs_unmount(&fs2); close_dev(&dev2); unlink(tmp);
}

/* ====================================================================
 * Test 4: repair na CZYSTYM FS -> issues==0, struktura nietknieta (idempotentne).
 * ==================================================================== */
static void test_repair_clean_noop(void) {
    char tmp[] = "/tmp/gh2fsckrep_clean_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev; CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/a", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/a/x", 0644), 0);
    uint8_t buf[2048]; fill_pattern(buf, sizeof(buf), 3);
    CHECK_EQ(gh2_fs_write(&fs, "/a/x", buf, sizeof(buf), 0), (ssize_t)sizeof(buf));
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    struct gh2_bptr root_before = fs.fs_root;
    int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0);
    CHECK_EQ(detected, 0);
    /* repair na czystym nie dotyka fs_root (f.issues==0 -> pass pominiety) */
    CHECK_EQ(memcmp(&root_before, &fs.fs_root, sizeof(root_before)), 0);

    /* dane wciaz czytelne */
    uint8_t rd[2048];
    CHECK_EQ(gh2_fs_read(&fs, "/a/x", rd, sizeof(rd), 0), (ssize_t)sizeof(rd));
    CHECK_EQ(memcmp(rd, buf, sizeof(buf)), 0);

    gh2_fs_unmount(&fs); close_dev(&dev); unlink(tmp);
}

/* ====================================================================
 * Test 5: atomowosc — ENOSPC w trakcie repair (maly kontener) -> fs_root nietkniety.
 * Maly FS z wieloma sierotami; repair alokuje wezly CoW; brak miejsca -> rollback.
 * ==================================================================== */
static void test_repair_atomic_enospc(void) {
    char tmp[] = "/tmp/gh2fsckrep_atom_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    /* sredni kontener: stworz kilka sierot, potem zapchaj wolna przestrzen blokami danych,
     * by pass naprawczy nie mial miejsca na wezly CoW -> ENOSPC -> rollback (nietkniete). */
    const uint64_t SMALL = 256;
    struct gh_dev dev; CHECK_EQ(open_dev_n(&dev, tmp, SMALL), 0);
    CHECK_EQ(gh2_fs_format(&dev, SMALL, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* kilka plikow -> osieroc (usun wpisy bez zwalniania i-wezlow) */
    int made = 0;
    for (int i = 0; i < 6; i++) {
        char p[32]; snprintf(p, sizeof(p), "/f%d", i);
        int r = gh2_fs_create(&fs, p, 0644);
        if (r) break;
        if (gh2_fs_commit(&fs) != 0) break;
        made++;
    }
    CHECK(made > 0);
    for (int i = 0; i < made; i++) {
        char nm[32]; snprintf(nm, sizeof(nm), "f%d", i);
        CHECK_EQ(gh2_fs_test_dir_remove(&fs, GH2_ROOT_INO, nm, (uint16_t)strlen(nm)), 0);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
    }

    /* zapchaj wolna przestrzen blokami danych (zostaw za malo na wezly CoW pass naprawczego) */
    CHECK_EQ(gh2_fs_create(&fs, "/big", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    uint8_t blk[4096]; memset(blk, 0xAB, sizeof(blk));
    uint64_t off = 0;
    for (;;) {
        ssize_t w = gh2_fs_write(&fs, "/big", blk, sizeof(blk), off);
        if (w < 0) break;
        if (gh2_fs_commit(&fs) != 0) break;
        off += sizeof(blk);
        if (off > (uint64_t)SMALL * 4096) break;   /* zabezpieczenie */
    }

    /* stan PRZED repair */
    struct gh2_bptr root_before = fs.fs_root;
    struct gh2_statfs st_before; CHECK_EQ(gh2_fs_statfs(&fs, &st_before), 0);

    int detected = -1;
    int r = gh2_fsck(&fs, 1, &detected);
    /* repair MUSI zglosic blad (ENOSPC/ENOMEM) — brak miejsca na wezly CoW */
    CHECK(r == -ENOSPC || r == -ENOMEM);
    /* ATOMOWOSC: fs_root NIETKNIETY; mapa nietknieta */
    CHECK_EQ(memcmp(&root_before, &fs.fs_root, sizeof(root_before)), 0);
    struct gh2_statfs st_after; CHECK_EQ(gh2_fs_statfs(&fs, &st_after), 0);
    CHECK_EQ(st_after.free_blocks, st_before.free_blocks);

    /* FS wciaz spojny dla read-only fsck (te same wykryte niespojnosci, brak korupcji) */
    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);

    gh2_fs_unmount(&fs); close_dev(&dev); unlink(tmp);
}

/* ====================================================================
 * Test 6: realistyczny mix typow niespojnosci -> repair -> fsck==0,
 * zdrowe dane bajt-exact po remount.
 * ==================================================================== */
static void test_realistic_mix(void) {
    char tmp[] = "/tmp/gh2fsckrep_mix_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    uint8_t *d1 = malloc(DLEN), *d2 = malloc(DLEN), *rd = malloc(DLEN);
    CHECK(d1 && d2 && rd);
    fill_pattern(d1, DLEN, 21); fill_pattern(d2, DLEN, 42);

    struct gh_dev dev; CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* zdrowa struktura: /good1 (dane), /sub/good2 (dane), /sub2 (katalog) */
    CHECK_EQ(gh2_fs_create(&fs, "/good1", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/good1", d1, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/sub", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/sub/good2", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/sub/good2", d2, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/sub2", 0755), 0);
    /* sierota-plik z danymi */
    CHECK_EQ(gh2_fs_create(&fs, "/orphan", 0644), 0);
    CHECK_EQ(gh2_fs_write(&fs, "/orphan", d1, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t oino = 0, subino = 0;
    CHECK_EQ(gh2_path_resolve(&fs, "/orphan", &oino), 0);
    CHECK_EQ(gh2_path_resolve(&fs, "/sub", &subino), 0);

    /* wstrzyknij mix: (1) sierota, (2) wiszacy DIR_ITEM, (3) zly nlink */
    CHECK_EQ(gh2_fs_test_dir_remove(&fs, GH2_ROOT_INO, "orphan", 6), 0);
    CHECK_EQ(gh2_fs_test_dir_add(&fs, subino, 0xDEADBEEFCAFEULL, "phantom", 7, 888888ULL, GH2_FT_FILE), 0);
    CHECK_EQ(gh2_fs_test_set_nlink(&fs, subino, 9), 0);   /* /sub nlink zle */
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues >= 3);
    int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0); CHECK(detected >= 3);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* remount: TRWALOSC + zdrowe dane bajt-exact */
    gh2_fs_unmount(&fs); close_dev(&dev);
    struct gh_dev dev2; CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2; CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);

    memset(rd, 0, DLEN);
    CHECK_EQ(gh2_fs_read(&fs2, "/good1", rd, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(memcmp(rd, d1, DLEN), 0);
    memset(rd, 0, DLEN);
    CHECK_EQ(gh2_fs_read(&fs2, "/sub/good2", rd, DLEN, 0), (ssize_t)DLEN);
    CHECK_EQ(memcmp(rd, d2, DLEN), 0);
    /* sierota i phantom zniknely */
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs2, "/orphan", &in, &ino), -ENOENT);
    CHECK_EQ(gh2_fs_getattr(&fs2, "/sub2", &in, &ino), 0);   /* zdrowy katalog zostaje */

    gh2_fs_unmount(&fs2); close_dev(&dev2);
    free(d1); free(d2); free(rd); unlink(tmp);
}

/* ====================================================================
 * Test 7 (REGRESJA bug 423f319): wiszacy DIR_ITEM typu KATALOG z MALYM ino
 * (< max_ino, bez INODE_ITEM) wskazujacy na nieistniejacy katalog.
 *
 * Bug: fsck_dir_cb2 liczyl taki wiszacy wpis do subdirs[parent]. Pass (a)
 * usuwa wiszacy wpis, ale pass (c) ustawia nlink rodzica = 2+subdirs[parent]
 * z NIEAKTUALNYM (zawyzonym) subdirs -> nlink ZDROWEGO rodzica zepsuty i fsck
 * po remount TRWALE zglasza niespojnosc.
 *
 * Wariant A: /p ma poprawny nlink=2 (brak realnych podkatalogow). Wstrzykniecie
 * wiszacego DIR-entry (male ino, bez INODE) -> repair -> remount fsck==0 ORAZ
 * /p nlink==2 (NIENARUSZONY).
 *
 * Aby uzyskac "male ino < max_ino bez INODE_ITEM": tworzymy pliki by podbic
 * next_ino > 500, potem usuwamy ten z ino==500 -> dziura ponizej max_ino.
 * ==================================================================== */
static uint64_t make_hole_ino_500(struct gh2_fs *fs) {
    /* Twórz pliki az ktorys dostanie ino == 500; potem go usun -> czysta dziura
     * (ino < max_ino bez INODE_ITEM), bo unlink nie zmniejsza next_ino. */
    const char *hole_path = NULL; char keep[32] = {0};
    for (int i = 0; i < 600; i++) {
        char p[32]; snprintf(p, sizeof(p), "/f%d", i);
        CHECK_EQ(gh2_fs_create(fs, p, 0644), 0);
        CHECK_EQ(gh2_fs_commit(fs), 0);
        uint64_t ino = 0; CHECK_EQ(gh2_path_resolve(fs, p, &ino), 0);
        if (ino == 500ULL) { snprintf(keep, sizeof(keep), "%s", p); hole_path = keep; break; }
        CHECK(ino < 500ULL);   /* nie przeskoczylismy 500 */
    }
    CHECK(hole_path != NULL);
    /* unlink usuwa wpis + INODE_ITEM (nlink->0); next_ino niezmienione ->
     * ino 500 staje sie CZYSTA dziura < max_ino bez INODE_ITEM. */
    CHECK_EQ(gh2_fs_unlink(fs, hole_path), 0);
    CHECK_EQ(gh2_fs_commit(fs), 0);
    return 500ULL;
}

static void test_dangling_dir_small_ino(void) {
    char tmp[] = "/tmp/gh2fsckrep_dsmall_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev; CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* zdrowy katalog /p o nlink=2 (zero podkatalogow) */
    CHECK_EQ(gh2_fs_mkdir(&fs, "/p", 0755), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t hole = make_hole_ino_500(&fs);   /* male ino < max_ino, bez INODE */

    uint64_t pino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/p", &pino), 0);
    /* potwierdz /p nlink==2 przed wstrzyknieciem */
    struct gh2_inode pin; uint64_t tmpino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs, "/p", &pin, &tmpino), 0);
    CHECK_EQ(pin.nlink, 2);

    /* WSTRZYKNIJ: wiszacy DIR-entry typu KATALOG -> hole (ino 500, bez INODE) */
    CHECK_EQ(gh2_fs_test_dir_add(&fs, pino, 0x5151515151515151ULL, "ghostdir", 8,
                                 hole, GH2_FT_DIR), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* fsck wykrywa (links_seen[500]!=0 && !is_inode -> wisienka) */
    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);

    /* repair + commit */
    int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0); CHECK(detected > 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* TRWALOSC: remount -> fsck==0 ORAZ /p nlink==2 (NIENARUSZONY) */
    gh2_fs_unmount(&fs); close_dev(&dev);
    struct gh_dev dev2; CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2; CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs2, "/p", &in, &ino), 0);
    CHECK_EQ(in.nlink, 2);   /* KRYTYCZNE: zdrowy rodzic nienaruszony */
    /* wiszacy wpis zniknal */
    CHECK_EQ(gh2_fs_getattr(&fs2, "/p/ghostdir", &in, &ino), -ENOENT);

    gh2_fs_unmount(&fs2); close_dev(&dev2); unlink(tmp);
}

/* ====================================================================
 * Test 8 (REGRESJA, wariant zepsutego nlink): korupcja zostawila /p nlink=3
 * (zawyzony, bo np. wczesniej istnial podkatalog) ORAZ wiszacy DIR-entry
 * (male ino, bez INODE). repair -> remount fsck==0 ORAZ /p nlink==2.
 * Dowodzi: pass (c) liczy expected nlink ze stanu PO usunieciu wiszacych.
 * ==================================================================== */
static void test_dangling_dir_small_ino_corrupt_nlink(void) {
    char tmp[] = "/tmp/gh2fsckrep_dsmall2_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

    struct gh_dev dev; CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    CHECK_EQ(gh2_fs_mkdir(&fs, "/p", 0755), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t hole = make_hole_ino_500(&fs);

    uint64_t pino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/p", &pino), 0);

    /* korupcja: nlink /p zawyzony do 3 (bez realnego podkatalogu) */
    CHECK_EQ(gh2_fs_test_set_nlink(&fs, pino, 3), 0);
    /* wiszacy DIR-entry (male ino, bez INODE) */
    CHECK_EQ(gh2_fs_test_dir_add(&fs, pino, 0x6262626262626262ULL, "phantomdir", 10,
                                 hole, GH2_FT_DIR), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    int issues = -1; CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0); CHECK(issues > 0);
    int detected = -1; CHECK_EQ(gh2_fsck(&fs, 1, &detected), 0); CHECK(detected > 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    gh2_fs_unmount(&fs); close_dev(&dev);
    struct gh_dev dev2; CHECK_EQ(reopen_dev(&dev2, tmp), 0);
    struct gh2_fs fs2; CHECK_EQ(gh2_fs_mount(&fs2, &dev2), 0);
    issues = -1; CHECK_EQ(gh2_fsck(&fs2, 0, &issues), 0); CHECK_EQ(issues, 0);
    struct gh2_inode in; uint64_t ino = 0;
    CHECK_EQ(gh2_fs_getattr(&fs2, "/p", &in, &ino), 0);
    CHECK_EQ(in.nlink, 2);   /* poprawiony do 2 (zero podkatalogow PO usunieciu wiszacego) */

    gh2_fs_unmount(&fs2); close_dev(&dev2); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_dangling_entry);
    RUN_TEST(test_orphan_freed);
    RUN_TEST(test_bad_nlink);
    RUN_TEST(test_repair_clean_noop);
    RUN_TEST(test_repair_atomic_enospc);
    RUN_TEST(test_realistic_mix);
    RUN_TEST(test_dangling_dir_small_ino);
    RUN_TEST(test_dangling_dir_small_ino_corrupt_nlink);
    return TEST_SUMMARY();
}
