#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_format.h"
#include "v2/gh2_ncache.h"
#include "block.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2-batchread — range-read sekwencyjny (gh2_btree_iterate_range zamiast
 * extent_lookup_at per blok 4 KB). BRAMKA: round-trip BAJT-EXACT (== model w
 * pamieci): dziury=zera, czesciowy ostatni blok, niewyrownane offsety, plik
 * WIELOPOZIOMOWY (>1 lisc, ~4 MB); csum->-EIO (dup OK->odczyt); read NIE MUTUJE
 * (mapa/refcount/fs_root niezmienione); read na zaszyfrowanym/snapshot; po
 * truncate; DOWOD redukcji descentow (gh_disk_read_count ~ liczba lisci, NIE ~N).
 * ========================================================================== */

static const uint64_t NBLK = 65536;

static int fs_open(struct gh_dev *dev, char *tmp) {
    int fd = mkstemp(tmp); if (fd < 0) return -errno; close(fd);
    int r = gh_dev_create(tmp, NBLK, dev); if (r) return r;
    r = gh_bcache_create(dev); if (r) return r;
    return gh2_fs_format(dev, NBLK, 0);
}
static int fs_open_key(struct gh_dev *dev, char *tmp, const char *key) {
    int fd = mkstemp(tmp); if (fd < 0) return -errno; close(fd);
    int r = gh_dev_create(tmp, NBLK, dev); if (r) return r;
    r = gh_bcache_create(dev); if (r) return r;
    r = gh2_fs_format_key(dev, NBLK, 0, key);
    /* format pozostawia cipher na dev; gh2_fs_mount_key utworzy WLASNY -> wymaz ten (brak wycieku). */
    if (dev->cipher) { gh_crypto_wipe(dev->cipher); free(dev->cipher); dev->cipher = NULL; }
    return r;
}
static void fs_close(struct gh2_fs *fs, struct gh_dev *dev, const char *tmp) {
    gh2_fs_unmount(fs);
    gh_bcache_destroy(dev); gh_dev_close(dev); unlink(tmp);
}

static void fill_pat(uint8_t *b, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((seed * 2654435761u + i * 131u) & 0xff);
}

/* leniwy "model": plik = bajty modelu[0..size); odczyt [off,len) klamrowany do size,
 * reszta bufora (poza klamra) NIE dotykana przez read (caller wie ile zwrocono). */
static void model_read(const uint8_t *model, size_t size, uint8_t *out, size_t len, uint64_t off,
                       size_t *exp_ret) {
    size_t ret = 0;
    if (off < size) {
        size_t avail = size - (size_t)off;
        ret = len < avail ? len : avail;
        memcpy(out, model + off, ret);
    }
    *exp_ret = ret;
}

/* ============================ round-trip bajt-exact ============================ */
static void check_ranges(struct gh2_fs *fs, const char *path, const uint8_t *model, size_t size) {
    /* zestaw offsetow/dlugosci: sub-blok, granice blokow, niewyrownane, przez granice lisci,
     * poza EOF (obciecie/0), caly plik. */
    uint64_t offs[] = { 0, 1, 7, 4095, 4096, 4097, 8191, 8192, 12345, 65536, 65537,
                        size > 100 ? size - 100 : 0, size, size + 10,
                        size > 4096 ? size - 4096 : 0 };
    size_t lens[] = { 1, 3, 100, 4095, 4096, 4097, 8192, 50000, size, size + 1000 };
    uint8_t *buf = malloc(size + 8192);
    uint8_t *mr  = malloc(size + 8192);
    for (unsigned oi = 0; oi < sizeof(offs)/sizeof(offs[0]); oi++) {
        for (unsigned li = 0; li < sizeof(lens)/sizeof(lens[0]); li++) {
            uint64_t off = offs[oi]; size_t len = lens[li];
            if (len == 0) continue;
            memset(buf, 0xCC, size + 8192);
            size_t exp = 0;
            model_read(model, size, mr, len, off, &exp);
            ssize_t rd = gh2_fs_read(fs, path, buf, len, off);
            CHECK_EQ(rd, (ssize_t)exp);
            if (rd > 0) CHECK_EQ(memcmp(buf, mr, (size_t)rd), 0);
        }
    }
    free(buf); free(mr);
}

static void test_roundtrip_sizes(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    size_t sizes[] = { 1, 100, 4095, 4096, 4097, 8192, 10000, 65535, 65536, (1<<16)+123,
                       300000 };
    for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
        size_t sz = sizes[si];
        char path[32]; snprintf(path, sizeof(path), "/f%u", si);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
        uint8_t *w = malloc(sz);
        fill_pat(w, sz, si + 1);
        CHECK_EQ(gh2_fs_write(&fs, path, w, sz, 0), (ssize_t)sz);
        check_ranges(&fs, path, w, sz);
        free(w);
    }
    fs_close(&fs, &dev, tmp);
}

/* plik WIELOPOZIOMOWY: ~4 MB (1024 blokow) -> drzewo ekstentow ma wiele lisci i >1 poziom. */
static void test_multilevel(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/big", 0644), 0);

    size_t sz = 4u * 1024 * 1024 + 777;   /* niewyrownany ogon */
    uint8_t *w = malloc(sz);
    fill_pat(w, sz, 0xAB);
    CHECK_EQ(gh2_fs_write(&fs, "/big", w, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* dowod wielopoziomowosci: drzewo ma >1 lisc */
    /* pelny odczyt bajt-exact */
    uint8_t *r = malloc(sz);
    CHECK_EQ(gh2_fs_read(&fs, "/big", r, sz, 0), (ssize_t)sz);
    CHECK_EQ(memcmp(w, r, sz), 0);
    free(r);

    /* odczyty przez granice lisci: zakresy obejmujace wiele lisci, niewyrownane */
    uint64_t offs[] = { 0, 1, 4095, 100000, 1000000, 2000000, sz - 50000, sz - 3 };
    size_t lens[] = { 1, 4097, 65536, 500000, sz };
    uint8_t *buf = malloc(sz + 8192), *mr = malloc(sz + 8192);
    for (unsigned oi = 0; oi < sizeof(offs)/sizeof(offs[0]); oi++)
        for (unsigned li = 0; li < sizeof(lens)/sizeof(lens[0]); li++) {
            uint64_t off = offs[oi]; size_t len = lens[li];
            memset(buf, 0xCC, sz + 8192);
            size_t exp = 0; model_read(w, sz, mr, len, off, &exp);
            ssize_t rd = gh2_fs_read(&fs, "/big", buf, len, off);
            CHECK_EQ(rd, (ssize_t)exp);
            if (rd > 0) CHECK_EQ(memcmp(buf, mr, (size_t)rd), 0);
        }
    free(buf); free(mr); free(w);
    fs_close(&fs, &dev, tmp);
}

/* dziury (sparse): zapis na wysokim offsecie; odczyt dziur = zera; czesciowy ostatni blok. */
static void test_sparse_holes(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/h", 0644), 0);

    /* model: 0..size-1, z danymi tylko w [blok0] i [blok5 czesciowo] i [wysoki offset] */
    size_t size = 100000;
    uint8_t *model = calloc(size, 1);
    uint8_t a[4096]; fill_pat(a, sizeof(a), 11);
    CHECK_EQ(gh2_fs_write(&fs, "/h", a, sizeof(a), 0), 4096);
    memcpy(model, a, 4096);
    uint8_t b[1234]; fill_pat(b, sizeof(b), 22);
    CHECK_EQ(gh2_fs_write(&fs, "/h", b, sizeof(b), 5 * 4096 + 50), (ssize_t)sizeof(b));
    memcpy(model + 5 * 4096 + 50, b, sizeof(b));
    uint8_t c[777]; fill_pat(c, sizeof(c), 33);
    CHECK_EQ(gh2_fs_write(&fs, "/h", c, sizeof(c), size - sizeof(c)), (ssize_t)sizeof(c));
    memcpy(model + size - sizeof(c), c, sizeof(c));
    /* size = max(off+len) = exactly `size` */

    check_ranges(&fs, "/h", model, size);

    /* jawny test dziury: blok 2 (czysta dziura) -> wszystkie zera */
    uint8_t z[4096]; memset(z, 0x9, sizeof(z));
    CHECK_EQ(gh2_fs_read(&fs, "/h", z, 4096, 2 * 4096), 4096);
    for (int i = 0; i < 4096; i++) CHECK_EQ(z[i], 0);

    /* read poza size -> 0 (obcity) */
    uint8_t q[16]; memset(q, 0x7, sizeof(q));
    CHECK_EQ(gh2_fs_read(&fs, "/h", q, 16, size), 0);
    CHECK_EQ(gh2_fs_read(&fs, "/h", q, 16, size + 9999), 0);

    free(model);
    fs_close(&fs, &dev, tmp);
}

/* ============================ csum -> -EIO / dup ============================ */
/* znajdz disk_block (i dup_block) ekstentu pliku ino na offsecie file_off. */
struct find_ext { uint64_t want_off; struct gh2_extent e; int found; };
static int find_ext_cb(const struct gh2_key *k, const void *v, uint32_t len, void *ctx) {
    struct find_ext *f = ctx;
    if (k->type != GH2_EXTENT_DATA || k->offset != f->want_off) return 0;
    memcpy(&f->e, v, len < sizeof(f->e) ? len : sizeof(f->e));
    f->found = 1;
    return 1;
}
static int find_extent(struct gh2_fs *fs, uint64_t ino, uint64_t file_off, struct gh2_extent *out) {
    struct find_ext f; memset(&f, 0, sizeof(f)); f.want_off = file_off;
    struct gh2_key min; memset(&min, 0, sizeof(min));
    min.objectid = ino; min.type = GH2_EXTENT_DATA; min.offset = file_off;
    struct gh2_key max = min;
    int r = gh2_btree_iterate_range(&fs->dev, &fs->fs_root, &min, &max, find_ext_cb, &f);
    (void)r;
    if (!f.found) return -1;
    *out = f.e;
    return 0;
}

static void test_csum_eio(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    uint8_t w[3 * 4096]; fill_pat(w, sizeof(w), 5);
    CHECK_EQ(gh2_fs_write(&fs, "/f", w, sizeof(w), 0), (ssize_t)sizeof(w));
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint64_t ino = 0; CHECK_EQ(gh2_path_resolve(&fs, "/f", &ino), 0);
    struct gh2_extent e;
    CHECK_EQ(find_extent(&fs, ino, 4096, &e), 0);   /* blok 1 (srodkowy) */
    CHECK(e.disk_block != 0);

    /* przekrec bit na disk_block -> niezgodnosc csum; dup_block==0 (v2.8) -> read -EIO */
    uint8_t blk[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(&dev, e.disk_block, blk), 0);
    blk[10] ^= 0x40;
    CHECK_EQ(gh_disk_write(&dev, e.disk_block, blk), 0);

    /* odczyt obejmujacy uszkodzony blok -> -EIO */
    uint8_t r[3 * 4096];
    CHECK_EQ(gh2_fs_read(&fs, "/f", r, sizeof(r), 0), -EIO);
    /* odczyt TYLKO bloku 0 (nieuszkodzonego) -> OK, bajt-exact */
    CHECK_EQ(gh2_fs_read(&fs, "/f", r, 4096, 0), 4096);
    CHECK_EQ(memcmp(r, w, 4096), 0);

    fs_close(&fs, &dev, tmp);
}

/* ============================ read NIE MUTUJE ============================ */
static void test_read_no_mutation(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);
    size_t sz = 512 * 1024;
    uint8_t *w = malloc(sz); fill_pat(w, sz, 1);
    CHECK_EQ(gh2_fs_write(&fs, "/f", w, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* snapshot stanu PRZED odczytami: fs_root, mapa bits/refs/nfree */
    struct gh2_bptr root_before = fs.fs_root;
    size_t bits_bytes = (size_t)(fs.space.nblocks + 7) / 8;
    size_t refs_bytes = (size_t)fs.space.nblocks * sizeof(uint16_t);
    uint8_t *bits0 = malloc(bits_bytes); memcpy(bits0, fs.space.bits, bits_bytes);
    uint16_t *refs0 = malloc(refs_bytes); memcpy(refs0, fs.space.refs, refs_bytes);
    uint64_t nfree0 = fs.space.nfree;
    unsigned long writes0 = gh_disk_write_count;

    /* wiele odczytow roznych zakresow */
    uint8_t *r = malloc(sz);
    for (int i = 0; i < 50; i++) {
        uint64_t off = (uint64_t)(i * 7919u) % sz;
        size_t len = 1 + ((size_t)(i * 104729u) % (sz - off));
        CHECK_EQ(gh2_fs_read(&fs, "/f", r, len, off), (ssize_t)len);
        CHECK_EQ(memcmp(r, w + off, len), 0);
    }
    free(r);

    /* BRAMKA: brak mutacji */
    CHECK_EQ(fs.fs_root.block, root_before.block);
    CHECK_EQ(memcmp(bits0, fs.space.bits, bits_bytes), 0);
    CHECK_EQ(memcmp(refs0, fs.space.refs, refs_bytes), 0);
    CHECK_EQ(fs.space.nfree, nfree0);
    CHECK_EQ(gh_disk_write_count, writes0);   /* read nie pisze na dysk */

    free(bits0); free(refs0); free(w);
    fs_close(&fs, &dev, tmp);
}

/* ============================ DOWOD redukcji descentow ============================ */
/* policz ZYWE lisce drzewa (DFS). */
static uint32_t count_leaves(struct gh_dev *dev, const struct gh2_bptr *node) {
    uint8_t buf[GH2_BLOCK_SIZE];
    if (gh2_node_read(dev, node, buf) != 0) return 0;
    const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
    if (h->level == 0) return 1;
    uint32_t nr = h->nritems, cnt = 0;
    const struct gh2_internal_ptr *p =
        (const struct gh2_internal_ptr *)(buf + sizeof(struct gh2_node_hdr));
    struct gh2_bptr kids[GH2_INT_CAP + 2];
    for (uint32_t i = 0; i < nr; i++) kids[i] = p[i].child;
    for (uint32_t i = 0; i < nr; i++) cnt += count_leaves(dev, &kids[i]);
    return cnt;
}
static uint32_t tree_height(struct gh_dev *dev, const struct gh2_bptr *node) {
    uint8_t buf[GH2_BLOCK_SIZE];
    if (gh2_node_read(dev, node, buf) != 0) return 0;
    const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
    return (uint32_t)h->level + 1;
}

static void test_descent_reduction(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/seq", 0644), 0);

    size_t N = 256;
    size_t sz = N * 4096;        /* 1 MB sekwencyjnie -> 256 ekstentow */
    uint8_t *w = malloc(sz); fill_pat(w, sz, 3);
    CHECK_EQ(gh2_fs_write(&fs, "/seq", w, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint32_t leaves = count_leaves(&dev, &fs.fs_root);
    uint32_t height = tree_height(&dev, &fs.fs_root);

    /* gh_disk_read_count liczy LOGICZNE odczyty bloku (wezly drzewa + bloki danych). Dla
     * porownania: odczyt range = (descent: height wezlow na pierwszy lisc) + (lisce zakresu) +
     * (N blokow DANYCH). Stary per-blok robil ~N*height odczytow WEZLOW + N danych. Mierzymy
     * odczyty WEZLOW = total - dane(N). */
    uint8_t *r = malloc(sz);
    gh_disk_read_count = 0;
    CHECK_EQ(gh2_fs_read(&fs, "/seq", r, sz, 0), (ssize_t)sz);
    unsigned long total_reads = gh_disk_read_count;
    CHECK_EQ(memcmp(w, r, sz), 0);

    /* odczyty WEZLOW = total - N blokow danych (kazdy ekstent czyta 1 blok danych) */
    unsigned long node_reads = total_reads > N ? total_reads - N : 0;
    printf("  [reduction] read %zu blokow: total_reads=%lu, node_reads=%lu "
           "(lisci=%u, wysokosc=%u). Per-blok bylby ~%zu*%u=%zu odczytow WEZLOW.\n",
           N, total_reads, node_reads, leaves, height, N, height, N * height);

    /* BRAMKA dowodu: odczyty WEZLOW ~ liczba lisci (+ kilka descentow: path_resolve, inode,
     * range), NIE ~ N*height. range-read odwiedza kazdy lisc raz + descent raz (zamiast
     * extent_lookup_at = pelny descent z korzenia na KAZDY z N blokow).
     * Per-blok robil >= N odczytow WEZLOW (>= 1 descent/blok); my robimy << N. */
    CHECK(node_reads < N);                         /* drastycznie mniej niz per-blok (>= N) */
    /* hojny gorny limit: lisce zakresu + kilka pelnych descentow (path/inode/range), height-glebokie */
    CHECK(node_reads <= (unsigned long)leaves + 3 * (height + 1) + 4);

    free(w); free(r);
    fs_close(&fs, &dev, tmp);
}

/* ============================ read po truncate ============================ */
static void test_after_truncate(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    size_t sz = 50000;
    uint8_t *w = malloc(sz); fill_pat(w, sz, 4);
    CHECK_EQ(gh2_fs_write(&fs, "/f", w, sz, 0), (ssize_t)sz);

    /* skroc do 10000 (niewyrownane) -> ostatni czesciowy blok + zwolnione ekstenty */
    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 10000), 0);
    check_ranges(&fs, "/f", w, 10000);

    /* rozszerz (sparse) do 70000 -> ogon = zera */
    CHECK_EQ(gh2_fs_truncate(&fs, "/f", 70000), 0);
    uint8_t *model = calloc(70000, 1);
    memcpy(model, w, 10000);
    check_ranges(&fs, "/f", model, 70000);

    free(model); free(w);
    fs_close(&fs, &dev, tmp);
}

/* ============================ read na zaszyfrowanym ============================ */
static void test_encrypted(void) {
    const char *KEY = "correct horse battery staple";
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open_key(&dev, tmp, KEY), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount_key(&fs, &dev, KEY), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/e", 0644), 0);

    size_t sz = 200000;
    uint8_t *w = malloc(sz); fill_pat(w, sz, 0x55);
    CHECK_EQ(gh2_fs_write(&fs, "/e", w, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    check_ranges(&fs, "/e", w, sz);

    free(w);
    fs_close(&fs, &dev, tmp);
}

/* ============================ read na snapshocie (read_subvol) ============================ */
struct sv_collect { uint64_t ids[16]; char names[16][64]; uint32_t n; };
static int sv_collect_cb(uint64_t id, const char *name, void *ctx) {
    struct sv_collect *c = ctx;
    if (c->n < 16) { c->ids[c->n] = id; snprintf(c->names[c->n], 64, "%s", name); c->n++; }
    return 0;
}
static int find_snap(struct sv_collect *c, const char *name, uint64_t *out) {
    for (uint32_t i = 0; i < c->n; i++) if (strcmp(c->names[i], name) == 0) { *out = c->ids[i]; return 1; }
    return 0;
}
static void test_snapshot_read(void) {
    char tmp[] = "/tmp/gh2br_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    size_t sz = 300000;
    uint8_t *w = malloc(sz); fill_pat(w, sz, 0x33);
    CHECK_EQ(gh2_fs_write(&fs, "/f", w, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    CHECK_EQ(gh2_fs_snapshot(&fs, "snap"), 0);
    struct sv_collect c = {0};
    gh2_fs_subvol_list(&fs, sv_collect_cb, &c);
    uint64_t sid = 0; CHECK(find_snap(&c, "snap", &sid));

    /* zmodyfikuj aktywny (CoW) -> snapshot wciaz STARA tresc; range-read na snapshocie bajt-exact */
    uint8_t patch[8192]; fill_pat(patch, sizeof(patch), 0x77);
    CHECK_EQ(gh2_fs_write(&fs, "/f", patch, sizeof(patch), 4096), (ssize_t)sizeof(patch));
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* snapshot: bajt-exact ze starym modelem w (rozne zakresy przez range-read) */
    uint8_t *buf = malloc(sz + 8192), *mr = malloc(sz + 8192);
    uint64_t offs[] = { 0, 1, 4095, 4096, 100000, sz - 100 };
    size_t lens[] = { 1, 4097, 65536, sz };
    for (unsigned oi = 0; oi < sizeof(offs)/sizeof(offs[0]); oi++)
        for (unsigned li = 0; li < sizeof(lens)/sizeof(lens[0]); li++) {
            uint64_t off = offs[oi]; size_t len = lens[li];
            memset(buf, 0xCC, sz + 8192);
            size_t exp = 0; model_read(w, sz, mr, len, off, &exp);
            ssize_t rd = gh2_fs_read_subvol(&fs, sid, "/f", buf, len, off);
            CHECK_EQ(rd, (ssize_t)exp);
            if (rd > 0) CHECK_EQ(memcmp(buf, mr, (size_t)rd), 0);
        }

    free(buf); free(mr); free(w);
    fs_close(&fs, &dev, tmp);
}

int main(void) {
    RUN_TEST(test_roundtrip_sizes);
    RUN_TEST(test_multilevel);
    RUN_TEST(test_sparse_holes);
    RUN_TEST(test_csum_eio);
    RUN_TEST(test_read_no_mutation);
    RUN_TEST(test_descent_reduction);
    RUN_TEST(test_after_truncate);
    RUN_TEST(test_encrypted);
    RUN_TEST(test_snapshot_read);
    return TEST_SUMMARY();
}
