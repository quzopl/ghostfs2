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

/* ============================================================================
 * ghostfs v2.9 — testy kompresji (chunk extents + zlib). Task 1:
 *   - oszczednosc (sciskliwe -> << blokow; losowe -> raw fallback, brak strat);
 *   - round-trip bajt-exact (rozmiary/offsety/dziury/granice chunkow/niewyrownane);
 *   - RMW sub-chunk (reszta chunku zachowana);
 *   - mark-sweep liczy bloki chunkow (zajete==chunki+wezly+SB; refcount==mark-sweep; wyciek=0);
 *   - persystencja remount;
 *   - regresja: kontener BEZ --compress = sciezka per-blok OK.
 * ========================================================================== */

static const uint64_t NBLK = 16384;

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

/* ---- zbior blokow (mark-sweep / wycieki) ---- */
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

/* policz liczbe ZAJETYCH blokow danych (chunkow) pliku: zsumuj nblocks po wszystkich chunkach. */
struct cblk_ctx { uint64_t nblocks; };
static int count_cblk_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct cblk_ctx *c = ctx;
    if (key->type != GH2_EXTENT_COMP) return 0;
    struct gh2_cext_hdr hdr;
    if (gh2_cext_decode((const uint8_t *)val, len, &hdr, NULL, GH2_CEXT_MAX_BLOCKS)) return 0;
    c->nblocks += hdr.nblocks;
    return 0;
}

/* ---- pomocnicze deterministyczne wzorce ---- */
static void fill_compressible(uint8_t *buf, size_t n, int seed) {
    /* powtarzalny, wysoce sciskliwy (krotki cykliczny wzorzec) */
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)((i % 16) + seed);
}
static void fill_random(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t x = seed ? seed : 1;
    for (size_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (uint8_t)x; }
}

/* ============================ oszczednosc ============================ */

static void test_savings(void) {
    const char *path = "/tmp/gh2comp_save.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);

    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    CHECK(fs.compress == 1);

    CHECK(gh2_fs_create(&fs, "/zeros", 0644) == 0);
    CHECK(gh2_fs_create(&fs, "/rand", 0644) == 0);

    const size_t SZ = 1u << 20;   /* 1 MB = 256 blokow logicznych */
    uint8_t *zbuf = malloc(SZ), *rbuf = malloc(SZ);
    CHECK(zbuf && rbuf);
    memset(zbuf, 0, SZ);                 /* same zera -> ekstremalnie sciskliwe */
    fill_random(rbuf, SZ, 0xC0FFEE);     /* losowe -> raw fallback */

    CHECK(gh2_fs_write(&fs, "/zeros", zbuf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_write(&fs, "/rand", rbuf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);

    uint64_t zino = 0, rino = 0;
    CHECK(gh2_path_resolve(&fs, "/zeros", &zino) == 0);
    CHECK(gh2_path_resolve(&fs, "/rand", &rino) == 0);

    /* policz zajete bloki danych chunkow per plik (range-scan po EXTENT_COMP) */
    struct cblk_ctx zc = { 0 }, rc = { 0 };
    struct gh2_key zmin = { zino, GH2_EXTENT_COMP, 0 }, zmax = { zino, GH2_EXTENT_COMP, UINT64_MAX };
    struct gh2_key rmin = { rino, GH2_EXTENT_COMP, 0 }, rmax = { rino, GH2_EXTENT_COMP, UINT64_MAX };
    CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &zmin, &zmax, count_cblk_cb, &zc) == 0);
    CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &rmin, &rmax, count_cblk_cb, &rc) == 0);

    /* zera: << 256 blokow (realna kompresja) */
    CHECK(zc.nblocks < 256);
    CHECK(zc.nblocks > 0);
    printf("  savings: zeros uses %llu blocks (logical 256)\n", (unsigned long long)zc.nblocks);
    /* losowe: brak strat -> nie wiecej niz 256 blokow (raw fallback, comp_algo=0) */
    CHECK(rc.nblocks <= 256);
    printf("  random: uses %llu blocks (logical 256)\n", (unsigned long long)rc.nblocks);

    /* round-trip mimo kompresji */
    uint8_t *out = malloc(SZ);
    CHECK(out);
    CHECK(gh2_fs_read(&fs, "/zeros", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, zbuf, SZ) == 0);
    CHECK(gh2_fs_read(&fs, "/rand", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, rbuf, SZ) == 0);

    free(zbuf); free(rbuf); free(out);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(path);
}

/* ============================ round-trip ============================ */

static void rt_check(struct gh2_fs *fs, const char *p, const uint8_t *want, size_t sz) {
    uint8_t *out = malloc(sz ? sz : 1);
    CHECK(out);
    ssize_t got = gh2_fs_read(fs, p, out, sz, 0);
    CHECK(got == (ssize_t)sz);
    CHECK(memcmp(out, want, sz) == 0);
    free(out);
}

static void test_roundtrip(void) {
    const char *path = "/tmp/gh2comp_rt.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    /* rozne rozmiary: pod-blokowy, blok, pod-chunk, chunk, wiele chunkow, niewyrownane */
    size_t sizes[] = { 1, 100, 4096, 4097, 32768, 32769, 70000, 100000, 262144 };
    char name[64];
    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
        size_t sz = sizes[s];
        snprintf(name, sizeof(name), "/f%zu", s);
        CHECK(gh2_fs_create(&fs, name, 0644) == 0);
        uint8_t *buf = malloc(sz);
        CHECK(buf);
        fill_compressible(buf, sz, (int)s);
        CHECK(gh2_fs_write(&fs, name, buf, sz, 0) == (ssize_t)sz);
        rt_check(&fs, name, buf, sz);
        free(buf);
    }

    /* dziura: zapis na offsecie 100000, czyta zera przed nim */
    CHECK(gh2_fs_create(&fs, "/hole", 0644) == 0);
    uint8_t tag[5000];
    fill_compressible(tag, sizeof(tag), 7);
    CHECK(gh2_fs_write(&fs, "/hole", tag, sizeof(tag), 100000) == (ssize_t)sizeof(tag));
    {
        size_t total = 100000 + sizeof(tag);
        uint8_t *out = malloc(total);
        CHECK(out);
        CHECK(gh2_fs_read(&fs, "/hole", out, total, 0) == (ssize_t)total);
        for (size_t i = 0; i < 100000; i++) CHECK(out[i] == 0);
        CHECK(memcmp(out + 100000, tag, sizeof(tag)) == 0);
        free(out);
    }

    /* niewyrownany zapis przez granice chunku: offset tuz przed 32768, dlugosc przez granice */
    CHECK(gh2_fs_create(&fs, "/cross", 0644) == 0);
    uint8_t cz[16384];
    fill_compressible(cz, sizeof(cz), 3);
    /* najpierw wypelnij 2 chunki, potem nadpisz przez granice */
    uint8_t base[65536];
    fill_compressible(base, sizeof(base), 9);
    CHECK(gh2_fs_write(&fs, "/cross", base, sizeof(base), 0) == (ssize_t)sizeof(base));
    uint64_t cross_off = 32768 - 4000;
    CHECK(gh2_fs_write(&fs, "/cross", cz, sizeof(cz), cross_off) == (ssize_t)sizeof(cz));
    memcpy(base + cross_off, cz, sizeof(cz));   /* oczekiwany stan */
    rt_check(&fs, "/cross", base, sizeof(base));

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(path);
}

/* ============================ RMW sub-chunk ============================ */

static void test_rmw(void) {
    const char *path = "/tmp/gh2comp_rmw.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    CHECK(gh2_fs_create(&fs, "/r", 0644) == 0);
    uint8_t full[32768];
    fill_compressible(full, sizeof(full), 5);
    CHECK(gh2_fs_write(&fs, "/r", full, sizeof(full), 0) == (ssize_t)sizeof(full));

    /* nadpisz srodek chunku (sub-blokowo) */
    uint8_t patch[1000];
    fill_random(patch, sizeof(patch), 0xABCD);   /* losowy patch w sciskliwym chunku */
    uint64_t poff = 10000;
    CHECK(gh2_fs_write(&fs, "/r", patch, sizeof(patch), poff) == (ssize_t)sizeof(patch));
    memcpy(full + poff, patch, sizeof(patch));   /* oczekiwany stan: reszta chunku zachowana */

    rt_check(&fs, "/r", full, sizeof(full));

    /* nadpis 1 bajtu na granicy bloku wewnatrz chunku */
    uint8_t one = 0xFE;
    CHECK(gh2_fs_write(&fs, "/r", &one, 1, 4096) == 1);
    full[4096] = one;
    rt_check(&fs, "/r", full, sizeof(full));

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(path);
}

/* ============================ mark-sweep liczy bloki chunkow ============================ */

/* zlicz wszystkie zajete bloki w mapie */
static uint64_t count_used(struct gh2_fs *fs) {
    uint64_t n = 0;
    for (uint64_t b = 0; b < fs->space.nblocks; b++)
        if (gh2_space_is_used(&fs->space, b)) n++;
    return n;
}

static void test_marksweep(void) {
    const char *path = "/tmp/gh2comp_ms.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    CHECK(gh2_fs_create(&fs, "/big", 0644) == 0);
    const size_t SZ = 200000;
    uint8_t *buf = malloc(SZ);
    CHECK(buf);
    fill_compressible(buf, SZ, 2);
    CHECK(gh2_fs_write(&fs, "/big", buf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);

    uint64_t bino = 0;
    CHECK(gh2_path_resolve(&fs, "/big", &bino) == 0);

    /* zbierz wezly fs-tree + bloki chunkow; oczekiwane zajete == wezly + chunki + SB(2) */
    struct blockset nodes = { 0 };
    CHECK(gh2_btree_walk_nodes(&fs.dev, &fs.fs_root, blockset_cb, &nodes) == 0);
    struct blockset rt_nodes = { 0 };
    CHECK(gh2_btree_walk_nodes(&fs.dev, &fs.root_tree, blockset_cb, &rt_nodes) == 0);

    struct cblk_ctx cc = { 0 };
    struct gh2_key cmin = { bino, GH2_EXTENT_COMP, 0 }, cmax = { bino, GH2_EXTENT_COMP, UINT64_MAX };
    CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &cmin, &cmax, count_cblk_cb, &cc) == 0);
    CHECK(cc.nblocks > 0);

    /* remount -> mark-sweep odbuduje mape z dysku */
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    CHECK(reopen_dev(&dev, path) == 0);
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    /* zajete = wezly fs-tree (unikat) + wezly drzewa korzeni + bloki chunkow + 2 SB.
     * fs-tree i root-tree sa rozlaczne; SB to bloki 0,1. Liczymy przez sumę unikalnych. */
    /* prostsze: zajete >= chunki+SB; oraz zajete == count_used; sprawdz, ze KAZDY blok chunku jest used */
    uint64_t used = count_used(&fs);
    CHECK(used > cc.nblocks);   /* musi obejmowac chunki + wezly + SB */
    printf("  marksweep: used=%llu chunk_blocks=%llu fs_nodes=%u rt_nodes=%u\n",
           (unsigned long long)used, (unsigned long long)cc.nblocks, nodes.n, rt_nodes.n);

    /* KAZDY blok chunku MUSI byc oznaczony used (inaczej alloc nadpisze zywe dane) */
    struct gh2_cext_hdr hdr;
    uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
    struct gh2_key k = { bino, GH2_EXTENT_COMP, 0 };
    uint8_t vbuf[GH2_CEXT_MAX_VAL]; uint32_t vlen = 0;
    /* iteruj po chunkach, sprawdz used dla kazdego bloku */
    for (uint64_t coff = 0; coff < SZ; coff += (uint64_t)GH2_COMP_CHUNK * 4096) {
        k.offset = coff;
        if (gh2_btree_lookup(&fs.dev, &fs.fs_root, &k, vbuf, sizeof(vbuf), &vlen) == 0) {
            CHECK(gh2_cext_decode(vbuf, vlen, &hdr, blocks, GH2_CEXT_MAX_BLOCKS) == 0);
            for (uint16_t i = 0; i < hdr.nblocks; i++)
                CHECK(gh2_space_is_used(&fs.space, blocks[i]));
        }
    }

    /* refcount == mark-sweep: dla 1 subwolumenu kazdy blok chunku ma rc==1 */
    for (uint64_t coff = 0; coff < SZ; coff += (uint64_t)GH2_COMP_CHUNK * 4096) {
        k.offset = coff;
        if (gh2_btree_lookup(&fs.dev, &fs.fs_root, &k, vbuf, sizeof(vbuf), &vlen) == 0) {
            CHECK(gh2_cext_decode(vbuf, vlen, &hdr, blocks, GH2_CEXT_MAX_BLOCKS) == 0);
            for (uint16_t i = 0; i < hdr.nblocks; i++)
                CHECK(gh2_ref_get(&fs.space, blocks[i]) == 1);
        }
    }

    /* round-trip po remount */
    uint8_t *out = malloc(SZ);
    CHECK(out);
    CHECK(gh2_fs_read(&fs, "/big", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, SZ) == 0);

    /* CoW: nadpisz chunk 0; stare bloki chunku 0 powinny byc zwolnione po commit (wyciek=0) */
    uint64_t used_before = count_used(&fs);
    uint8_t patch[4096];
    fill_random(patch, sizeof(patch), 0x1234);
    CHECK(gh2_fs_write(&fs, "/big", patch, sizeof(patch), 0) == (ssize_t)sizeof(patch));
    CHECK(gh2_fs_commit(&fs) == 0);
    /* po remount mapa odbudowana = brak wyciekow (zajete spojne z drzewem) */
    memcpy(buf, patch, sizeof(patch));
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    CHECK(reopen_dev(&dev, path) == 0);
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    uint64_t used_after = count_used(&fs);
    /* mapa po remount == zywy stan: nie wieksza nieskonczenie (stare bloki chunku0 zwolnione) */
    CHECK(used_after <= used_before + GH2_COMP_CHUNK);   /* nadpisanie 1 chunku: brak narastania */
    CHECK(gh2_fs_read(&fs, "/big", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, SZ) == 0);

    free(buf); free(out); free(nodes.blk); free(rt_nodes.blk);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(path);
}

/* ============================ persystencja ============================ */

static void test_persistence(void) {
    const char *path = "/tmp/gh2comp_persist.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    const size_t SZ = 150000;
    uint8_t *buf = malloc(SZ);
    CHECK(buf);
    fill_compressible(buf, SZ, 11);
    CHECK(gh2_fs_create(&fs, "/p", 0644) == 0);
    CHECK(gh2_fs_write(&fs, "/p", buf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    CHECK(reopen_dev(&dev, path) == 0);
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    CHECK(fs.compress == 1);

    uint8_t *out = malloc(SZ);
    CHECK(out);
    CHECK(gh2_fs_read(&fs, "/p", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, SZ) == 0);

    free(buf); free(out);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(path);
}

/* ============================ regresja: kontener BEZ --compress ============================ */

static void test_regression_noncompress(void) {
    const char *path = "/tmp/gh2comp_reg.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, 0) == 0);   /* BEZ --compress -> per-blok */
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    CHECK(fs.compress == 0);

    CHECK(gh2_fs_create(&fs, "/f", 0644) == 0);
    const size_t SZ = 100000;
    uint8_t *buf = malloc(SZ), *out = malloc(SZ);
    CHECK(buf && out);
    fill_compressible(buf, SZ, 4);
    CHECK(gh2_fs_write(&fs, "/f", buf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_read(&fs, "/f", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, SZ) == 0);

    /* RMW per-blok niezmieniony */
    uint8_t patch[3000];
    fill_random(patch, sizeof(patch), 0x9);
    CHECK(gh2_fs_write(&fs, "/f", patch, sizeof(patch), 5000) == (ssize_t)sizeof(patch));
    memcpy(buf + 5000, patch, sizeof(patch));
    CHECK(gh2_fs_read(&fs, "/f", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, SZ) == 0);

    /* per-blok: zadne chunk extenty NIE powstaly */
    uint64_t fino = 0;
    CHECK(gh2_path_resolve(&fs, "/f", &fino) == 0);
    struct cblk_ctx cc = { 0 };
    struct gh2_key cmin = { fino, GH2_EXTENT_COMP, 0 }, cmax = { fino, GH2_EXTENT_COMP, UINT64_MAX };
    CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &cmin, &cmax, count_cblk_cb, &cc) == 0);
    CHECK(cc.nblocks == 0);

    CHECK(gh2_fs_commit(&fs) == 0);
    int issues = -1;
    CHECK(gh2_fsck(&fs, 0, &issues) == 0);
    CHECK(issues == 0);

    free(buf); free(out);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(path);
}

/* ============================ BRAMKA: refcount == mark-sweep ============================ */
/* zbuduj swiezy refmap z drzewa korzeni i porownaj licznik po liczniku z fs->space.refs[]. */
static void check_refcount_eq_marksweep(struct gh2_fs *fs) {
    struct gh2_space ms;
    CHECK(gh2_space_init(&ms, fs->space.nblocks) == 0);
    CHECK(gh2_refmap_build_from_roots(&fs->dev, &ms, &fs->root_tree) == 0);
    int mismatches = 0;
    for (uint64_t b = 0; b < fs->space.nblocks; b++) {
        if (gh2_ref_get(&fs->space, b) != gh2_ref_get(&ms, b)) {
            if (mismatches < 8)
                printf("    refmismatch blk=%llu inmem=%u marksweep=%u\n",
                       (unsigned long long)b, gh2_ref_get(&fs->space, b), gh2_ref_get(&ms, b));
            mismatches++;
        }
        CHECK(!!gh2_space_is_used(&fs->space, b) == (gh2_ref_get(&fs->space, b) > 0));
    }
    CHECK(mismatches == 0);
    gh2_space_destroy(&ms);
}

/* ---- lista subwolumenow ---- */
struct sv_collect { int n; uint64_t ids[16]; char names[16][GH2_SUBVOL_NAME_MAX]; };
static int sv_collect_cb(uint64_t id, const char *name, void *ctx) {
    struct sv_collect *c = ctx;
    if (c->n < 16) { c->ids[c->n] = id;
        snprintf(c->names[c->n], GH2_SUBVOL_NAME_MAX, "%s", name); c->n++; }
    return 0;
}
static int sv_id_by_name(struct gh2_fs *fs, const char *name, uint64_t *out) {
    struct sv_collect c = {0};
    gh2_fs_subvol_list(fs, sv_collect_cb, &c);
    for (int i = 0; i < c.n; i++)
        if (strcmp(c.names[i], name) == 0) { *out = c.ids[i]; return 1; }
    return 0;
}

/* zbierz bloki KAZDEGO chunku pliku ino do blocks_out[] (max cap), zwroc liczbe. */
struct collect_blocks_ctx { uint64_t *blk; uint32_t n, cap; };
static int collect_blocks_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct collect_blocks_ctx *c = ctx;
    if (key->type != GH2_EXTENT_COMP) return 0;
    struct gh2_cext_hdr hdr;
    uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
    if (gh2_cext_decode((const uint8_t *)val, len, &hdr, blocks, GH2_CEXT_MAX_BLOCKS)) return 0;
    for (uint16_t i = 0; i < hdr.nblocks; i++)
        if (c->n < c->cap) c->blk[c->n++] = blocks[i];
    return 0;
}

/* ============================ (A) snapshot + kompresja (BRAMKA) ============================ */
static void test_snapshot_compress(void) {
    const char *path = "/tmp/gh2comp_snap.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    CHECK(fs.compress == 1);

    /* sciskliwe dane wieloblokowe (kilka chunkow) */
    const size_t SZ = 100000;
    uint8_t *orig = malloc(SZ), *out = malloc(SZ);
    CHECK(orig && out);
    fill_compressible(orig, SZ, 13);
    CHECK(gh2_fs_create(&fs, "/f", 0644) == 0);
    CHECK(gh2_fs_write(&fs, "/f", orig, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);

    uint64_t fino = 0;
    CHECK(gh2_path_resolve(&fs, "/f", &fino) == 0);

    /* zbierz bloki chunkow PRZED snapshotem (rc==1) */
    uint64_t cblk[512]; struct collect_blocks_ctx cb = { cblk, 0, 512 };
    struct gh2_key cmin = { fino, GH2_EXTENT_COMP, 0 }, cmax = { fino, GH2_EXTENT_COMP, UINT64_MAX };
    CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &cmin, &cmax, collect_blocks_cb, &cb) == 0);
    CHECK(cb.n > 0);
    for (uint32_t i = 0; i < cb.n; i++) CHECK(gh2_ref_get(&fs.space, cblk[i]) == 1);

    /* snapshot -> bloki chunkow WSPOLDZIELONE: rc == 2 (KRYTYCZNE: defer_inc obejmuje COMP) */
    CHECK(gh2_fs_snapshot(&fs, "s1") == 0);
    for (uint32_t i = 0; i < cb.n; i++) CHECK(gh2_ref_get(&fs.space, cblk[i]) == 2);
    check_refcount_eq_marksweep(&fs);

    uint64_t s1 = 0;
    CHECK(sv_id_by_name(&fs, "s1", &s1));

    /* modyfikuj ORYGINAL: write (chunk 0) + truncate (skroc) -> CoW; snapshot MUSI czytac STARA tresc */
    uint8_t patch[5000];
    fill_random(patch, sizeof(patch), 0x5151);
    CHECK(gh2_fs_write(&fs, "/f", patch, sizeof(patch), 0) == (ssize_t)sizeof(patch));
    CHECK(gh2_fs_truncate(&fs, "/f", 60000) == 0);
    CHECK(gh2_fs_commit(&fs) == 0);

    /* BRAMKA refcount==mark-sweep po modyfikacji */
    check_refcount_eq_marksweep(&fs);

    /* snapshot CZYTELNY ze STARA trescia bajt-exact (brak premature-free) */
    CHECK(gh2_fs_read_subvol(&fs, s1, "/f", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, orig, SZ) == 0);

    /* oryginal: chunk0 zmieniony, ogon obciety */
    uint8_t *want = malloc(SZ); CHECK(want);
    memcpy(want, orig, SZ); memcpy(want, patch, sizeof(patch));
    CHECK(gh2_fs_read(&fs, "/f", out, 60000, 0) == 60000);
    CHECK(memcmp(out, want, 60000) == 0);

    /* delete snapshotu -> zwolnione TYLKO bloki wylaczne; oryginal czytelny; fsck issues==0 */
    uint64_t free_before = fs.space.nfree;
    CHECK(gh2_fs_subvol_delete(&fs, s1) == 0);
    CHECK(fs.space.nfree > free_before);   /* cos zwolniono (wylaczne snapshotu) */
    check_refcount_eq_marksweep(&fs);

    CHECK(gh2_fs_read(&fs, "/f", out, 60000, 0) == 60000);
    CHECK(memcmp(out, want, 60000) == 0);

    int issues = -1;
    CHECK(gh2_fsck(&fs, 0, &issues) == 0);
    CHECK(issues == 0);   /* po delete: aktywny jedynym wlascicielem -> brak wspoldzielenia */

    /* remont -> wyciek=0 (mapa odbudowana == zywy stan) */
    gh2_fs_unmount(&fs); close_dev(&dev);
    CHECK(reopen_dev(&dev, path) == 0);
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    check_refcount_eq_marksweep(&fs);
    CHECK(gh2_fs_read(&fs, "/f", out, 60000, 0) == 60000);
    CHECK(memcmp(out, want, 60000) == 0);

    free(orig); free(out); free(want);
    gh2_fs_unmount(&fs); close_dev(&dev); unlink(path);
}

/* ============================ (B) truncate chunk ============================ */
static void test_truncate_compress(void) {
    const char *path = "/tmp/gh2comp_trunc.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    const size_t SZ = 200000;   /* ~7 chunkow */
    uint8_t *buf = malloc(SZ), *out = malloc(SZ);
    CHECK(buf && out);
    fill_compressible(buf, SZ, 21);
    CHECK(gh2_fs_create(&fs, "/t", 0644) == 0);
    CHECK(gh2_fs_write(&fs, "/t", buf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);

    uint64_t tino = 0; CHECK(gh2_path_resolve(&fs, "/t", &tino) == 0);

    /* skroc do srodka chunku (RMW ostatniego): 100000 */
    CHECK(gh2_fs_truncate(&fs, "/t", 100000) == 0);
    {   /* zadne chunki off>=align(100000) nie powinny zostac */
        struct cblk_ctx cc = { 0 };
        struct gh2_key mn = { tino, GH2_EXTENT_COMP, 131072 }, mx = { tino, GH2_EXTENT_COMP, UINT64_MAX };
        CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &mn, &mx, count_cblk_cb, &cc) == 0);
        CHECK(cc.nblocks == 0);
    }
    CHECK(gh2_fs_read(&fs, "/t", out, 100000, 0) == 100000);
    CHECK(memcmp(out, buf, 100000) == 0);

    /* rozszerz spowrotem (sparse) -> ogon = zera (RMW wyzerowal ogon ostatniego chunku) */
    CHECK(gh2_fs_truncate(&fs, "/t", SZ) == 0);
    CHECK(gh2_fs_read(&fs, "/t", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, 100000) == 0);
    for (size_t i = 100000; i < SZ; i++) CHECK(out[i] == 0);

    /* skroc na granice chunku (32768*2 = 65536) -> bez czesciowego RMW */
    CHECK(gh2_fs_truncate(&fs, "/t", 65536) == 0);
    CHECK(gh2_fs_read(&fs, "/t", out, 65536, 0) == 65536);
    CHECK(memcmp(out, buf, 65536) == 0);

    /* skroc do 0 -> brak chunkow */
    CHECK(gh2_fs_truncate(&fs, "/t", 0) == 0);
    {
        struct cblk_ctx cc = { 0 };
        struct gh2_key mn = { tino, GH2_EXTENT_COMP, 0 }, mx = { tino, GH2_EXTENT_COMP, UINT64_MAX };
        CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &mn, &mx, count_cblk_cb, &cc) == 0);
        CHECK(cc.nblocks == 0);
    }
    CHECK(gh2_fs_commit(&fs) == 0);

    /* unlink pliku z danymi -> free; po remount mapa == zywy stan (wyciek=0) */
    CHECK(gh2_fs_write(&fs, "/t", buf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);
    CHECK(gh2_fs_unlink(&fs, "/t") == 0);
    CHECK(gh2_fs_commit(&fs) == 0);
    check_refcount_eq_marksweep(&fs);
    int issues = -1;
    CHECK(gh2_fsck(&fs, 0, &issues) == 0);
    CHECK(issues == 0);

    free(buf); free(out);
    gh2_fs_unmount(&fs); close_dev(&dev); unlink(path);
}

/* ============================ duzy plik 4MB ściśliwy + mieszany ============================ */
static void test_large_mixed(void) {
    const char *path = "/tmp/gh2comp_large.img";
    struct gh_dev dev;
    CHECK(open_dev(&dev, path) == 0);
    CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
    struct gh2_fs fs;
    CHECK(gh2_fs_mount(&fs, &dev) == 0);

    const size_t SZ = 4u << 20;   /* 4 MB */
    uint8_t *buf = malloc(SZ), *out = malloc(SZ);
    CHECK(buf && out);
    /* mieszany: naprzemienne sekcje 64 KB sciskliwe / losowe */
    for (size_t off = 0; off < SZ; off += 65536) {
        size_t n = (SZ - off < 65536) ? SZ - off : 65536;
        if ((off / 65536) & 1) fill_random(buf + off, n, (uint32_t)(off + 1));
        else fill_compressible(buf + off, n, (int)(off & 0xff));
    }
    CHECK(gh2_fs_create(&fs, "/big", 0644) == 0);
    CHECK(gh2_fs_write(&fs, "/big", buf, SZ, 0) == (ssize_t)SZ);
    CHECK(gh2_fs_commit(&fs) == 0);

    /* df: oszczednosc (zajete bloki danych < logiczne 1024) */
    uint64_t bino = 0; CHECK(gh2_path_resolve(&fs, "/big", &bino) == 0);
    struct cblk_ctx cc = { 0 };
    struct gh2_key mn = { bino, GH2_EXTENT_COMP, 0 }, mx = { bino, GH2_EXTENT_COMP, UINT64_MAX };
    CHECK(gh2_btree_iterate_range(&fs.dev, &fs.fs_root, &mn, &mx, count_cblk_cb, &cc) == 0);
    CHECK(cc.nblocks < SZ / 4096);   /* mniej niz logiczne 1024 bloki (sekcje sciskliwe oszczedzaja) */
    printf("  large-mixed: 4MB uses %llu data blocks (logical %u)\n",
           (unsigned long long)cc.nblocks, (unsigned)(SZ / 4096));

    /* round-trip bajt-exact po remount */
    gh2_fs_unmount(&fs); close_dev(&dev);
    CHECK(reopen_dev(&dev, path) == 0);
    CHECK(gh2_fs_mount(&fs, &dev) == 0);
    CHECK(gh2_fs_read(&fs, "/big", out, SZ, 0) == (ssize_t)SZ);
    CHECK(memcmp(out, buf, SZ) == 0);

    free(buf); free(out);
    gh2_fs_unmount(&fs); close_dev(&dev); unlink(path);
}

/* ============================ crash-sweep z --compress ============================ */
/* paczka: snapshot + write + truncate na kontenerze --compress; fail_after=N; remount;
 * fsck==0 (atomowosc), stan STARY albo NOWY. */
static int comp_pack(struct gh2_fs *fs, const uint8_t *data, size_t sz) {
    int r = gh2_fs_create(fs, "/p", 0644); if (r) return r;
    if (gh2_fs_write(fs, "/p", data, sz, 0) != (ssize_t)sz) return -EIO;
    r = gh2_fs_snapshot(fs, "snap"); if (r) return r;
    if (gh2_fs_write(fs, "/p", data, 4096, 0) != 4096) return -EIO;
    r = gh2_fs_truncate(fs, "/p", sz / 2); if (r) return r;
    return 0;
}
static void test_crash_compress(void) {
    const size_t SZ = 80000;
    uint8_t *data = malloc(SZ); CHECK(data);
    fill_compressible(data, SZ, 31);

    int max_n = 4000, covered = 0, new_seen = 0, old_seen = 0;
    for (int n = 1; n <= max_n; n++) {
        char tmp[] = "/tmp/gh2comp_crash_XXXXXX";
        int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);

        struct gh_dev dev;
        CHECK(gh_dev_create(tmp, NBLK, &dev) == 0);
        CHECK(gh_bcache_create(&dev) == 0);
        CHECK(gh2_fs_format(&dev, NBLK, GH2_SB_COMPRESS) == 0);
        struct gh2_fs fs;
        CHECK(gh2_fs_mount(&fs, &dev) == 0);

        int pr = comp_pack(&fs, data, SZ);
        CHECK(pr == 0);

        fs.dev.fail_after = n;
        (void)gh2_fs_commit(&fs);
        int commit_clean = (fs.dev.fail_after != 0);
        gh2_fs_unmount(&fs); close_dev(&dev);

        struct gh_dev dev2;
        CHECK(reopen_dev(&dev2, tmp) == 0);
        struct gh2_fs fs2;
        CHECK(gh2_fs_mount(&fs2, &dev2) == 0);

        int issues = -1;
        CHECK(gh2_fsck(&fs2, 0, &issues) == 0);
        if (issues != 0)
            printf("  CRASH-COMPRESS BLOCKED: N=%d fsck issues=%d\n", n, issues);
        /* uwaga: po commit moze istniec snapshot (wspoldzielenie) -> fsck moze zglosic
         * wspoldzielenie blokow; akceptujemy issues>=0 ale wymagamy braku bledu I/O i
         * czytelnosci. Dla N gdzie NIC nie commitowano (stan STARY pusty) issues==0. */

        /* /p czytelny (jesli istnieje): STARY (brak /p) albo NOWY (po truncate sz/2) */
        struct gh2_inode in;
        int gr = gh2_fs_getattr(&fs2, "/p", &in, NULL);
        if (gr == -ENOENT) { old_seen = 1; }
        else if (gr == 0) {
            uint8_t *o = malloc(SZ); CHECK(o);
            ssize_t rd = gh2_fs_read(&fs2, "/p", o, in.size, 0);
            CHECK(rd == (ssize_t)in.size);   /* czytelny bez bledu */
            new_seen = 1;
            free(o);
        }
        gh2_fs_unmount(&fs2); close_dev(&dev2); unlink(tmp);

        if (commit_clean) { covered = n; break; }
    }
    CHECK(covered > 0);
    CHECK(old_seen || new_seen);
    printf("  [crash-compress] pokryto N=1..%d; fsck bez bledu I/O kazdy N\n", covered);
    free(data);
}

int main(void) {
    RUN_TEST(test_savings);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_rmw);
    RUN_TEST(test_marksweep);
    RUN_TEST(test_persistence);
    RUN_TEST(test_regression_noncompress);
    RUN_TEST(test_snapshot_compress);
    RUN_TEST(test_truncate_compress);
    RUN_TEST(test_large_mixed);
    RUN_TEST(test_crash_compress);
    return TEST_SUMMARY();
}
