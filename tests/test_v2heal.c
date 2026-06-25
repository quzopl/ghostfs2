#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_super.h"
#include "v2/gh2_format.h"
#include "csum.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2.8 (Task 1) — testy samonaprawy (DUP metadane + read-repair).
 *
 * Pokrycie:
 *  - format z DUP -> kazdy wezel ma dup_block!=0, obie kopie bajt-identyczne (ta sama csum);
 *  - read-repair: zepsuj `block` -> odczyt OK z dup + NAPRAWA; zepsuj dup -> OK z block;
 *    zepsuj OBIE -> -EIO;
 *  - mark-sweep/refcount LICZA OBIE kopie (zajete == 2x wezly + dane + SB; refcount==mark-sweep;
 *    CoW zwalnia obie stare; wyciek=0);
 *  - snapshot z DUP: refcount==2 dla block I dup_block; delete zwalnia obie wylaczne;
 *  - realistyczna samonaprawa: FS z plikami/katalogami DUP, losowy bitrot 1 kopii losowych
 *    wezlow -> cały odczyt poprawny, fsck==0;
 *  - crash + DUP: crash-sweep -> fsck==0;
 *  - regresja DUP-off: dup_block==0, zachowanie jak v2.0-2.7.
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

/* ---- czytaj root_tree z superbloku ---- */
static int read_sb(struct gh_dev *dev, struct gh2_superblock *sb) {
    return gh2_mount(dev, sb);
}

/* ---- zbieracz wezlow drzewa (walk po block/dup) ---- */
struct nodewalk {
    struct gh2_bptr nodes[4096];
    uint32_t n;
};
/* walk rekurencyjny zapisujacy CALE bptr (z dup) kazdego wezla */
static int walk_collect(struct gh_dev *dev, const struct gh2_bptr *node, struct nodewalk *w) {
    if (node->block == 0) return 0;
    if (w->n < 4096) w->nodes[w->n++] = *node;
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
    if (h->level == 0) return 0;
    uint32_t nr = h->nritems;
    struct gh2_internal_ptr p[GH2_INT_CAP + 2];
    const struct gh2_internal_ptr *pc =
        (const struct gh2_internal_ptr *)(buf + sizeof(struct gh2_node_hdr));
    for (uint32_t i = 0; i < nr; i++) p[i] = pc[i];
    for (uint32_t i = 0; i < nr; i++) {
        r = walk_collect(dev, &p[i].child, w);
        if (r) return r;
    }
    return 0;
}

/* zbierz wezly drzewa korzeni ORAZ wszystkich fs-tree (przez subvol_item) */
static int collect_all_nodes(struct gh_dev *dev, const struct gh2_bptr *root_tree,
                             struct nodewalk *w) {
    int r = walk_collect(dev, root_tree, w);
    if (r) return r;
    /* iteruj wpisy GH2_ROOT_ITEM, dla kazdego fs_root walk */
    struct gh2_bptr fsroots[16]; int nfs = 0;
    uint8_t buf[GH2_BLOCK_SIZE];
    /* prosty in-order po drzewie korzeni szukajacy ROOT_ITEM */
    struct stack { struct gh2_bptr b; } st[64]; int sp = 0;
    st[sp++].b = *root_tree;
    while (sp > 0) {
        struct gh2_bptr cur = st[--sp].b;
        if (cur.block == 0) continue;
        r = gh2_node_read(dev, &cur, buf);
        if (r) return r;
        const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
        if (h->level == 0) {
            const struct gh2_leaf_item *it =
                (const struct gh2_leaf_item *)(buf + sizeof(struct gh2_node_hdr));
            for (uint32_t i = 0; i < h->nritems; i++) {
                if (it[i].key.type == GH2_ROOT_ITEM &&
                    it[i].data_len == sizeof(struct gh2_subvol_item)) {
                    struct gh2_subvol_item sv;
                    memcpy(&sv, buf + it[i].data_off, sizeof(sv));
                    if (nfs < 16) fsroots[nfs++] = sv.fs_root;
                }
            }
        } else {
            const struct gh2_internal_ptr *pc =
                (const struct gh2_internal_ptr *)(buf + sizeof(struct gh2_node_hdr));
            for (uint32_t i = 0; i < h->nritems && sp < 64; i++) st[sp++].b = pc[i].child;
        }
    }
    for (int i = 0; i < nfs; i++) {
        r = walk_collect(dev, &fsroots[i], w);
        if (r) return r;
    }
    return 0;
}

/* ---- surowa korupcja: czytaj blok (wypelnia cache), flip 1 bit, zapisz (dysk + cache) ---- */
static void corrupt_block(struct gh_dev *dev, uint64_t blk) {
    uint8_t buf[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(dev, blk, buf), 0);
    buf[100] ^= 0x40;                       /* flip bit w danych wezla (poza naglowkiem) */
    CHECK_EQ(gh_disk_write(dev, blk, buf), 0);
}

/* czy surowy blok ma sume zgodna z csum bptr (po naprawie powinno byc znow zgodne) */
static int block_csum_ok(struct gh_dev *dev, uint64_t blk, uint32_t csum) {
    uint8_t buf[GH2_BLOCK_SIZE];
    if (gh_disk_read(dev, blk, buf) != 0) return 0;
    return gh_crc32(buf, GH2_BLOCK_SIZE) == csum;
}

/* ============================ TEST 1: format DUP -> 2 kopie identyczne ============================ */
static void test_format_dup_two_copies(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_DUP_META), 0);

    struct gh2_superblock sb;
    CHECK_EQ(read_sb(&dev, &sb), 0);
    CHECK(sb.flags & GH2_SB_DUP_META);

    struct nodewalk w; w.n = 0;
    CHECK_EQ(collect_all_nodes(&dev, &sb.root_tree, &w), 0);
    CHECK(w.n > 0);
    /* kazdy wezel: dup_block != 0, dup != block, obie kopie bajt-identyczne i zgodne z csum */
    for (uint32_t i = 0; i < w.n; i++) {
        CHECK(w.nodes[i].dup_block != 0);
        CHECK(w.nodes[i].dup_block != w.nodes[i].block);
        uint8_t a[GH2_BLOCK_SIZE], b[GH2_BLOCK_SIZE];
        CHECK_EQ(gh_disk_read(&dev, w.nodes[i].block, a), 0);
        CHECK_EQ(gh_disk_read(&dev, w.nodes[i].dup_block, b), 0);
        CHECK_EQ(memcmp(a, b, GH2_BLOCK_SIZE), 0);
        CHECK_EQ(gh_crc32(a, GH2_BLOCK_SIZE), w.nodes[i].csum);
        CHECK_EQ(gh_crc32(b, GH2_BLOCK_SIZE), w.nodes[i].csum);
    }
    close_dev(&dev); unlink(tmp);
}

/* ============================ TEST 2: regresja DUP-off -> dup_block==0 ============================ */
static void test_format_nodup_no_copies(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);     /* DUP off (domyslnie) */

    struct gh2_superblock sb;
    CHECK_EQ(read_sb(&dev, &sb), 0);
    CHECK_EQ(sb.flags & GH2_SB_DUP_META, 0);

    struct nodewalk w; w.n = 0;
    CHECK_EQ(collect_all_nodes(&dev, &sb.root_tree, &w), 0);
    CHECK(w.n > 0);
    for (uint32_t i = 0; i < w.n; i++)
        CHECK_EQ(w.nodes[i].dup_block, 0);          /* bez DUP: brak 2. kopii */
    close_dev(&dev); unlink(tmp);
}

/* ============================ TEST 3: read-repair (block/dup/oba) ============================ */
static void test_read_repair(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_DUP_META), 0);

    struct gh2_superblock sb;
    CHECK_EQ(read_sb(&dev, &sb), 0);
    struct nodewalk w; w.n = 0;
    CHECK_EQ(collect_all_nodes(&dev, &sb.root_tree, &w), 0);
    CHECK(w.n > 0);
    struct gh2_bptr node = w.nodes[0];   /* dowolny wezel z DUP */
    uint8_t good[GH2_BLOCK_SIZE], out[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(&dev, node.block, good), 0);

    /* (a) zepsuj `block`: odczyt OK (z dup) + naprawa block */
    corrupt_block(&dev, node.block);
    CHECK_EQ(block_csum_ok(&dev, node.block, node.csum), 0);  /* przed: zepsuty */
    CHECK_EQ(gh2_node_read(&dev, &node, out), 0);             /* odczyt OK (z dup) */
    CHECK_EQ(memcmp(out, good, GH2_BLOCK_SIZE), 0);           /* poprawne dane */
    CHECK_EQ(block_csum_ok(&dev, node.block, node.csum), 1);  /* po: block NAPRAWIONY */

    /* (b) zepsuj `dup_block`: odczyt OK (z block) */
    corrupt_block(&dev, node.dup_block);
    CHECK_EQ(gh2_node_read(&dev, &node, out), 0);
    CHECK_EQ(memcmp(out, good, GH2_BLOCK_SIZE), 0);
    /* dup nie jest naprawiany gdy block dobry (read sciezka czyta block i wraca) -> wciaz zly */
    CHECK_EQ(block_csum_ok(&dev, node.dup_block, node.csum), 0);

    /* (c) zepsuj OBIE -> -EIO. Najpierw przywroc dobre kopie (poprzednie kroki zostawily
     * block naprawiony, dup zepsuty), potem zepsuj kazda DOKLADNIE raz (1 flip = zla suma). */
    CHECK_EQ(gh_disk_write(&dev, node.block, good), 0);
    CHECK_EQ(gh_disk_write(&dev, node.dup_block, good), 0);
    corrupt_block(&dev, node.block);
    corrupt_block(&dev, node.dup_block);
    CHECK_EQ(gh2_node_read(&dev, &node, out), -EIO);

    close_dev(&dev); unlink(tmp);
}

/* ============================ TEST 4: mark-sweep/refcount licza OBIE kopie ============================ */
/* policz unikalne bloki w mapie (zajete) */
static uint64_t count_used(struct gh2_space *s) {
    uint64_t n = 0;
    for (uint64_t b = 0; b < s->nblocks; b++) if (gh2_space_is_used(s, b)) n++;
    return n;
}
/* refcount==mark-sweep (jak w test_v2snap) */
static void check_refcount_eq_marksweep(struct gh2_fs *fs) {
    struct gh2_space ms;
    CHECK_EQ(gh2_space_init(&ms, fs->space.nblocks), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&fs->dev, &ms, &fs->root_tree), 0);
    int mism = 0;
    for (uint64_t b = 0; b < fs->space.nblocks; b++) {
        if (gh2_ref_get(&fs->space, b) != gh2_ref_get(&ms, b)) mism++;
        CHECK_EQ(!!gh2_space_is_used(&fs->space, b), gh2_ref_get(&fs->space, b) > 0);
    }
    CHECK_EQ(mism, 0);
    gh2_space_destroy(&ms);
}

static void test_marksweep_counts_both(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_DUP_META), 0);
    close_dev(&dev);

    struct gh2_fs fs;
    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(fs.alloc.dup_meta, 1);              /* mount ustawil DUP z flagi SB */

    /* zlicz wezly drzewa (CALE bptr) z dup */
    struct nodewalk w; w.n = 0;
    CHECK_EQ(collect_all_nodes(&dev, &fs.root_tree, &w), 0);
    CHECK(w.n > 0);
    /* policz unikalne bloki fizyczne (block + dup) */
    uint64_t phys = 0;
    for (uint32_t i = 0; i < w.n; i++) { phys++; if (w.nodes[i].dup_block) phys++; }
    /* wszystkie wezly z dup -> phys == 2 * w.n */
    CHECK_EQ(phys, 2 * (uint64_t)w.n);

    /* zajete w mapie = 2 (SB) + bloki fizyczne wezlow (brak danych: pusty FS) */
    uint64_t used = count_used(&fs.space);
    CHECK_EQ(used, 2 + phys);                     /* mark-sweep policzyl OBIE kopie kazdego wezla */

    /* refcount == mark-sweep, used <=> rc>0 */
    check_refcount_eq_marksweep(&fs);

    /* CoW zwalnia obie stare kopie: utworz plik, commit, sprawdz spojnosc */
    CHECK_EQ(gh2_fs_create(&fs, "/a", 0644), 0);
    char data[5000]; memset(data, 'x', sizeof(data));
    CHECK_EQ(gh2_fs_write(&fs, "/a", data, sizeof(data), 0), (ssize_t)sizeof(data));
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_refcount_eq_marksweep(&fs);
    /* brak wycieku: nfree spojne z mapa */
    CHECK_EQ(fs.space.nfree, fs.space.nblocks - count_used(&fs.space));

    gh2_fs_unmount(&fs);
    close_dev(&dev); unlink(tmp);
}

/* ============================ TEST 5: snapshot z DUP -> refcount==2 dla block I dup ============================ */
static void test_snapshot_dup(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_DUP_META), 0);
    close_dev(&dev);

    struct gh2_fs fs;
    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/d/f", 0644), 0);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* zbierz wszystkie wezly fs_root aktywnego subwol PRZED snapshotem */
    struct nodewalk w; w.n = 0;
    CHECK_EQ(walk_collect(&dev, &fs.fs_root, &w), 0);
    CHECK(w.n > 0);

    CHECK_EQ(gh2_fs_snapshot(&fs, "snap1"), 0);
    check_refcount_eq_marksweep(&fs);

    /* refcount == 2 dla block I dup_block kazdego WSPOLDZIELONEGO wezla fs_root */
    for (uint32_t i = 0; i < w.n; i++) {
        CHECK_EQ(gh2_ref_get(&fs.space, w.nodes[i].block), 2);
        CHECK(w.nodes[i].dup_block != 0);
        CHECK_EQ(gh2_ref_get(&fs.space, w.nodes[i].dup_block), 2);
    }

    /* delete snapshotu zwalnia obie WYLACZNE kopie (rc 2->1, nic nie zwolnione bo wspoldzielone) */
    /* znajdz id snapshotu */
    uint64_t snap_id = 0;
    {
        /* iteruj subwol: snap1 != default(1) */
        struct gh2_superblock sb; CHECK_EQ(read_sb(&dev, &sb), 0);
        (void)sb;
        snap_id = 2;   /* pierwszy snapshot dostaje max+1 = 2 */
    }
    CHECK_EQ(gh2_fs_subvol_delete(&fs, snap_id), 0);
    check_refcount_eq_marksweep(&fs);
    /* po delete: rc==1 (tylko default) */
    for (uint32_t i = 0; i < w.n; i++) {
        CHECK_EQ(gh2_ref_get(&fs.space, w.nodes[i].block), 1);
        CHECK_EQ(gh2_ref_get(&fs.space, w.nodes[i].dup_block), 1);
    }

    gh2_fs_unmount(&fs);
    close_dev(&dev); unlink(tmp);
}

/* ============================ TEST 6: realistyczna samonaprawa ============================ */
static void test_realistic_selfheal(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_DUP_META), 0);
    close_dev(&dev);

    struct gh2_fs fs;
    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* zbuduj FS z katalogami i plikami (zmusza B-drzewo do wielu wezlow) */
    char path[64];
    for (int d = 0; d < 6; d++) {
        snprintf(path, sizeof(path), "/dir%d", d);
        CHECK_EQ(gh2_fs_mkdir(&fs, path, 0755), 0);
        for (int f = 0; f < 8; f++) {
            snprintf(path, sizeof(path), "/dir%d/file%d", d, f);
            CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
            char data[300];
            memset(data, 'A' + (d + f) % 26, sizeof(data));
            CHECK_EQ(gh2_fs_write(&fs, path, data, sizeof(data), 0), (ssize_t)sizeof(data));
        }
    }
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* zbierz wezly fs-tree aktywnego subwolumenu (fsck/odczyty przechodza je w calosci ->
     * kazda zepsuta kopia `block` zostanie naprawiona). Wezly drzewa korzeni nie sa czytane
     * przez odczyt FS, wiec ich nie psujemy (poza zakresem read-repair w tym scenariuszu). */
    struct nodewalk w; w.n = 0;
    CHECK_EQ(walk_collect(&dev, &fs.fs_root, &w), 0);
    CHECK(w.n >= 3);   /* wielopoziomowe drzewo */

    /* przekreć losowy bit w JEDNEJ kopii (block) losowych wezlow (~polowa) */
    unsigned seed = 0xC0FFEE;
    int corrupted = 0;
    for (uint32_t i = 0; i < w.n; i++) {
        if (rand_r(&seed) & 1) {
            corrupt_block(&dev, w.nodes[i].block);
            corrupted++;
        }
    }
    CHECK(corrupted > 0);

    /* CALY odczyt FS poprawny (samonaprawa z dup) */
    for (int d = 0; d < 6; d++) {
        for (int f = 0; f < 8; f++) {
            snprintf(path, sizeof(path), "/dir%d/file%d", d, f);
            char rbuf[300]; char exp[300];
            memset(exp, 'A' + (d + f) % 26, sizeof(exp));
            ssize_t n = gh2_fs_read(&fs, path, rbuf, sizeof(rbuf), 0);
            CHECK_EQ(n, (ssize_t)sizeof(rbuf));
            CHECK_EQ(memcmp(rbuf, exp, sizeof(rbuf)), 0);
        }
    }

    /* fsck == 0 (po naprawie struktura spojna) */
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    /* po odczycie zepsute kopie `block` powinny byc NAPRAWIONE (suma znow dobra) */
    int still_bad = 0;
    for (uint32_t i = 0; i < w.n; i++)
        if (!block_csum_ok(&dev, w.nodes[i].block, w.nodes[i].csum)) still_bad++;
    CHECK_EQ(still_bad, 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev); unlink(tmp);
}

/* ============================ TEST 7: crash + DUP -> fsck==0 ============================ */
static void test_crash_dup(void) {
    char tmp[] = "/tmp/ghost_v2healXXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, GH2_SB_DUP_META), 0);
    close_dev(&dev);

    /* zapisz dane i commit */
    struct gh2_fs fs;
    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_mkdir(&fs, "/x", 0755), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/x/a", 0644), 0);
    char data[1000]; memset(data, 'Z', sizeof(data));
    CHECK_EQ(gh2_fs_write(&fs, "/x/a", data, sizeof(data), 0), (ssize_t)sizeof(data));
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    gh2_fs_unmount(&fs);
    close_dev(&dev);

    /* symuluj "crash": niezacommitowane mutacje porzucone (brak commitu). Remount -> crash-sweep
     * odbuduje mape z trwalego SB; obie kopie spojne -> fsck==0. */
    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    /* mutacja BEZ commitu (porzucona) */
    CHECK_EQ(gh2_fs_create(&fs, "/x/b", 0644), 0);
    gh2_fs_unmount(&fs);       /* brak commitu -> jak crash */
    close_dev(&dev);

    CHECK_EQ(reopen_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);
    check_refcount_eq_marksweep(&fs);
    /* trwale dane wciaz czytelne */
    char rbuf[1000];
    CHECK_EQ(gh2_fs_read(&fs, "/x/a", rbuf, sizeof(rbuf), 0), (ssize_t)sizeof(rbuf));
    CHECK_EQ(memcmp(rbuf, data, sizeof(rbuf)), 0);
    /* porzucona mutacja zniknela */
    struct gh2_inode in; uint64_t ino;
    CHECK_EQ(gh2_fs_getattr(&fs, "/x/b", &in, &ino), -ENOENT);

    gh2_fs_unmount(&fs);
    close_dev(&dev); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_format_dup_two_copies);
    RUN_TEST(test_format_nodup_no_copies);
    RUN_TEST(test_read_repair);
    RUN_TEST(test_marksweep_counts_both);
    RUN_TEST(test_snapshot_dup);
    RUN_TEST(test_realistic_selfheal);
    RUN_TEST(test_crash_dup);
    return TEST_SUMMARY();
}
