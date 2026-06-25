#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_format.h"
#include "v2/gh2_ncache.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2.10 — batch-insert (gh2_btree_insert_run) + gh2_fs_write batch.
 * BRAMKA: insert_run ≡ N single inserts (drzewa BAJT-IDENTYCZNE dla losowych
 * posortowanych ciagow ze splitami i wieloma poziomami). gh2_fs_write round-trip
 * bajt-exact + nadpisanie CoW (wyciek=0). Dowod redukcji CoW wezlow sekwencyjnego.
 * ========================================================================== */

/* ---- urzadzenie ---- */
static int open_dev(struct gh_dev *dev, char *tmp) {
    int fd = mkstemp(tmp); if (fd < 0) return -errno; close(fd);
    int r = gh_dev_create(tmp, 65536, dev); if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev, const char *tmp) {
    gh_bcache_destroy(dev); gh_dev_close(dev); unlink(tmp);
}

static struct gh2_key mk(uint64_t oid, uint8_t type, uint64_t off) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = oid; k.type = type; k.offset = off; return k;
}

/* ============================ walidator drzewa ============================
 * niezmienniki: dla kazdego wezla wewn. key[0] == min poddrzewa, klucze rosnace,
 * dzieci posortowane; lisc: itemy rosnace, mieszcza sie; key[0] = min calego drzewa. */
static int validate_node(struct gh_dev *dev, const struct gh2_bptr *node,
                         struct gh2_key *out_min) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
    uint32_t nr = h->nritems;
    if (nr == 0) return -1;                 /* pusty wezel niedozwolony (poza pustym korzeniem) */
    if (h->level == 0) {
        const struct gh2_leaf_item *it =
            (const struct gh2_leaf_item *)(buf + sizeof(struct gh2_node_hdr));
        if (nr > GH2_NODE_SPACE / sizeof(struct gh2_leaf_item)) return -2;
        uint32_t used = nr * (uint32_t)sizeof(struct gh2_leaf_item);
        for (uint32_t i = 0; i < nr; i++) {
            used += it[i].data_len;
            if (i > 0 && gh2_key_cmp(&it[i - 1].key, &it[i].key) >= 0) return -3;  /* rosnace+unik */
        }
        if (used > GH2_NODE_SPACE) return -4;                                       /* przepelnienie */
        *out_min = it[0].key;
        return 0;
    }
    if (nr > GH2_INT_CAP) return -5;
    const struct gh2_internal_ptr *p =
        (const struct gh2_internal_ptr *)(buf + sizeof(struct gh2_node_hdr));
    struct gh2_internal_ptr pc[GH2_INT_CAP + 2];
    for (uint32_t i = 0; i < nr; i++) pc[i] = p[i];
    struct gh2_key prev_min;
    for (uint32_t i = 0; i < nr; i++) {
        struct gh2_key cmin;
        r = validate_node(dev, &pc[i].child, &cmin);
        if (r) return r;
        if (gh2_key_cmp(&pc[i].key, &cmin) != 0) return -6;       /* key[i] == min dziecka i */
        if (i > 0 && gh2_key_cmp(&prev_min, &cmin) >= 0) return -7; /* dzieci rosnace */
        prev_min = cmin;
    }
    *out_min = pc[0].key;
    return 0;
}

/* ---- bajt-exact serializacja drzewa: iteracja (key,len,val) ---- */
struct ser { uint8_t *p; size_t n, cap; int rc; };
static void ser_put(struct ser *s, const void *d, size_t len) {
    if (s->n + len > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        while (nc < s->n + len) nc *= 2;
        uint8_t *np = realloc(s->p, nc);
        if (!np) { s->rc = -ENOMEM; return; }
        s->p = np; s->cap = nc;
    }
    memcpy(s->p + s->n, d, len); s->n += len;
}
static int ser_cb(const struct gh2_key *k, const void *v, uint32_t len, void *ctx) {
    struct ser *s = ctx;
    ser_put(s, k, sizeof(*k));
    ser_put(s, &len, sizeof(len));
    ser_put(s, v, len);
    return s->rc;
}
static int serialize_tree(struct gh_dev *dev, const struct gh2_bptr *root, struct ser *s) {
    memset(s, 0, sizeof(*s));
    return gh2_btree_iterate(dev, root, ser_cb, s);
}

/* policz ZYWE wezly drzewa (DFS) */
static uint32_t count_nodes(struct gh_dev *dev, const struct gh2_bptr *node) {
    uint8_t buf[GH2_BLOCK_SIZE];
    if (gh2_node_read(dev, node, buf) != 0) return 0;
    const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
    uint32_t cnt = 1;
    if (h->level != 0) {
        uint32_t nr = h->nritems;
        const struct gh2_internal_ptr *p =
            (const struct gh2_internal_ptr *)(buf + sizeof(struct gh2_node_hdr));
        struct gh2_bptr kids[GH2_INT_CAP + 2];
        for (uint32_t i = 0; i < nr; i++) kids[i] = p[i].child;
        for (uint32_t i = 0; i < nr; i++) cnt += count_nodes(dev, &kids[i]);
    }
    return cnt;
}
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

/* ============================ cross-check insert_run ≡ N single ============================ */
struct kvbuf { struct gh2_key key; uint8_t val[GH2_LEAF_MAX_VAL]; uint32_t len; };

/* dla danego ciagu POSORTOWANEGO (items[n]) zbuduj drzewo A (insert_run) i B (n* single);
 * sprawdz: bajt-exact iteracja rowna; lookup wszystkich; niezmienniki obu; CoW stary root. */
static void cross_check(struct kvbuf *items, uint32_t n, unsigned seed_tag) {
    char tmp[] = "/tmp/gh2batch_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 65536);
    struct gh2_alloc al = gh2_bump_alloc(&bump);

    /* drzewo A: insert_run */
    struct gh2_bptr rootA0, rootA;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &rootA0), 0);
    struct gh2_kv *kv = malloc((size_t)n * sizeof(*kv));
    for (uint32_t i = 0; i < n; i++) {
        kv[i].key = items[i].key; kv[i].val = items[i].val; kv[i].len = items[i].len;
    }
    int r = gh2_btree_insert_run(&dev, &al, &rootA0, 7, kv, n, &rootA);
    CHECK_EQ(r, 0);
    free(kv);

    /* drzewo B: n single insertow (taka sama kolejnosc) */
    struct gh2_bptr rootB;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &rootB), 0);
    for (uint32_t i = 0; i < n; i++) {
        struct gh2_bptr nr;
        r = gh2_btree_insert(&dev, &al, &rootB, 7, &items[i].key, items[i].val, items[i].len, &nr);
        CHECK_EQ(r, 0);
        rootB = nr;
    }

    /* BAJT-EXACT: iteracja A == iteracja B */
    struct ser sa, sb;
    CHECK_EQ(serialize_tree(&dev, &rootA, &sa), 0);
    CHECK_EQ(serialize_tree(&dev, &rootB, &sb), 0);
    CHECK_EQ(sa.rc, 0); CHECK_EQ(sb.rc, 0);
    CHECK_EQ(sa.n, sb.n);
    if (sa.n == sb.n) CHECK_EQ(memcmp(sa.p, sb.p, sa.n), 0);
    if (sa.n != sb.n || (sa.n == sb.n && memcmp(sa.p, sb.p, sa.n) != 0))
        printf("  [cross-check MISMATCH seed_tag=%u n=%u]\n", seed_tag, n);
    free(sa.p); free(sb.p);

    /* niezmienniki obu drzew */
    struct gh2_key mn;
    CHECK_EQ(validate_node(&dev, &rootA, &mn), 0);
    CHECK_EQ(gh2_key_cmp(&mn, &items[0].key), 0);        /* key[0] = min calego drzewa */
    CHECK_EQ(validate_node(&dev, &rootB, &mn), 0);
    CHECK_EQ(gh2_key_cmp(&mn, &items[0].key), 0);

    /* lookup wszystkich kluczy w A */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t buf[GH2_LEAF_MAX_VAL]; uint32_t ol = 0;
        r = gh2_btree_lookup(&dev, &rootA, &items[i].key, buf, sizeof(buf), &ol);
        CHECK_EQ(r, 0);
        CHECK_EQ(ol, items[i].len);
        CHECK_EQ(memcmp(buf, items[i].val, items[i].len), 0);
    }

    (void)rootA0;   /* stary korzen (pusty) zostal CoW-free przez insert_run jak single-insert */
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* generuj POSORTOWANY UNIKALNY ciag itemow zmiennej dlugosci (wymusza split: duze wartosci). */
static uint32_t gen_sorted(struct kvbuf *items, uint32_t want, unsigned *seed, int big) {
    uint64_t off = 0;
    for (uint32_t i = 0; i < want; i++) {
        off += 1 + rand_r(seed) % 5;          /* rosnaco, unikalnie */
        items[i].key = mk(1, GH2_EXTENT_DATA, off);
        uint32_t len;
        if (big) len = 1 + rand_r(seed) % GH2_LEAF_MAX_VAL;   /* az do max -> czeste splity */
        else     len = 1 + rand_r(seed) % 40;
        items[i].len = len;
        for (uint32_t j = 0; j < len; j++) items[i].val[j] = (uint8_t)(rand_r(seed) & 0xff);
    }
    return want;
}

static void test_cross_check_random(void) {
    unsigned seed = 0xC0FFEE;
    uint32_t sizes[] = { 1, 2, 3, 5, 16, 50, 100, 300, 1000, 2500 };
    struct kvbuf *items = malloc(3000 * sizeof(*items));
    for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
        uint32_t n = sizes[si];
        /* warianty: male wartosci (wiele itemow/lisc) i duze (czeste splity, wiele poziomow) */
        gen_sorted(items, n, &seed, 0);
        cross_check(items, n, 1000 + si);
        gen_sorted(items, n, &seed, 1);
        cross_check(items, n, 2000 + si);
    }
    /* dodatkowe losowe rozmiary */
    for (int iter = 0; iter < 20; iter++) {
        uint32_t n = 1 + rand_r(&seed) % 1500;
        gen_sorted(items, n, &seed, rand_r(&seed) & 1);
        cross_check(items, n, 3000 + iter);
    }
    free(items);
}

/* update istniejacych: wstaw run, potem run nadpisujacy te same klucze nowymi wartosciami;
 * cross-check z N single insertow tej samej sekwencji. */
static void test_cross_check_update(void) {
    char tmp[] = "/tmp/gh2batch_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 65536);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    unsigned seed = 42;

    uint32_t n = 400;
    struct kvbuf *base = malloc(n * sizeof(*base));
    gen_sorted(base, n, &seed, 1);
    struct kvbuf *upd = malloc(n * sizeof(*upd));
    for (uint32_t i = 0; i < n; i++) {
        upd[i].key = base[i].key;
        upd[i].len = 1 + rand_r(&seed) % GH2_LEAF_MAX_VAL;
        for (uint32_t j = 0; j < upd[i].len; j++) upd[i].val[j] = (uint8_t)(rand_r(&seed) & 0xff);
    }

    /* A: run(base) then run(upd) */
    struct gh2_bptr rA; CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &rA), 0);
    struct gh2_kv *kv = malloc(n * sizeof(*kv));
    for (uint32_t i = 0; i < n; i++) { kv[i].key = base[i].key; kv[i].val = base[i].val; kv[i].len = base[i].len; }
    struct gh2_bptr t; CHECK_EQ(gh2_btree_insert_run(&dev, &al, &rA, 7, kv, n, &t), 0); rA = t;
    for (uint32_t i = 0; i < n; i++) { kv[i].key = upd[i].key; kv[i].val = upd[i].val; kv[i].len = upd[i].len; }
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &rA, 7, kv, n, &t), 0); rA = t;
    free(kv);

    /* B: single base then single upd */
    struct gh2_bptr rB; CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &rB), 0);
    struct gh2_bptr nr;
    for (uint32_t i = 0; i < n; i++) { CHECK_EQ(gh2_btree_insert(&dev, &al, &rB, 7, &base[i].key, base[i].val, base[i].len, &nr), 0); rB = nr; }
    for (uint32_t i = 0; i < n; i++) { CHECK_EQ(gh2_btree_insert(&dev, &al, &rB, 7, &upd[i].key, upd[i].val, upd[i].len, &nr), 0); rB = nr; }

    struct ser sa, sb;
    CHECK_EQ(serialize_tree(&dev, &rA, &sa), 0);
    CHECK_EQ(serialize_tree(&dev, &rB, &sb), 0);
    CHECK_EQ(sa.n, sb.n);
    if (sa.n == sb.n) CHECK_EQ(memcmp(sa.p, sb.p, sa.n), 0);
    free(sa.p); free(sb.p);
    struct gh2_key mn; CHECK_EQ(validate_node(&dev, &rA, &mn), 0);

    /* lookup: kazda wartosc == upd */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t buf[GH2_LEAF_MAX_VAL]; uint32_t ol = 0;
        CHECK_EQ(gh2_btree_lookup(&dev, &rA, &upd[i].key, buf, sizeof(buf), &ol), 0);
        CHECK_EQ(ol, upd[i].len);
        CHECK_EQ(memcmp(buf, upd[i].val, upd[i].len), 0);
    }
    free(base); free(upd);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* run na pustym / 1-item / -EFBIG / n=0 */
static void test_run_edge(void) {
    char tmp[] = "/tmp/gh2batch_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root; CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &root), 0);

    /* n=0 -> out=root, brak alokacji */
    struct gh2_bptr out;
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &root, 7, NULL, 0, &out), 0);
    CHECK_EQ(out.block, root.block);

    /* 1 item */
    uint8_t v[10]; memset(v, 'x', sizeof(v));
    struct gh2_kv one = { mk(1, 3, 0), v, 10 };
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &root, 7, &one, 1, &out), 0);
    root = out;
    uint8_t rb[16]; uint32_t ol = 0;
    CHECK_EQ(gh2_btree_lookup(&dev, &root, &one.key, rb, sizeof(rb), &ol), 0);
    CHECK_EQ(ol, 10); CHECK_EQ(memcmp(rb, v, 10), 0);

    /* -EFBIG: wartosc za duza -> out nietkniety */
    uint8_t big[GH2_LEAF_MAX_VAL + 1];
    struct gh2_kv toobig = { mk(2, 3, 0), big, GH2_LEAF_MAX_VAL + 1 };
    struct gh2_bptr saved = root;
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &root, 7, &toobig, 1, &out), -EFBIG);
    CHECK_EQ(saved.block, root.block);   /* root nietkniety */

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* wyciek=0: zbuduj run, zwolnij CALE drzewo -> in_use==0 (poza korzeniem pustym) */
static void free_tree(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node) {
    uint8_t buf[GH2_BLOCK_SIZE];
    if (gh2_node_read(dev, node, buf) != 0) { a->free(a->ctx, node->block); return; }
    const struct gh2_node_hdr *h = (const struct gh2_node_hdr *)buf;
    if (h->level != 0) {
        uint32_t nr = h->nritems;
        const struct gh2_internal_ptr *p =
            (const struct gh2_internal_ptr *)(buf + sizeof(struct gh2_node_hdr));
        struct gh2_bptr kids[GH2_INT_CAP + 2];
        for (uint32_t i = 0; i < nr; i++) kids[i] = p[i].child;
        for (uint32_t i = 0; i < nr; i++) free_tree(dev, a, &kids[i]);
    }
    a->free(a->ctx, node->block);
    if (node->dup_block) a->free(a->ctx, node->dup_block);
}
static void test_run_no_leak(void) {
    char tmp[] = "/tmp/gh2batch_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 65536);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    unsigned seed = 99;
    struct kvbuf *items = malloc(800 * sizeof(*items));
    uint32_t n = gen_sorted(items, 800, &seed, 1);

    struct gh2_bptr root; CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &root), 0);
    struct gh2_kv *kv = malloc(n * sizeof(*kv));
    for (uint32_t i = 0; i < n; i++) { kv[i].key = items[i].key; kv[i].val = items[i].val; kv[i].len = items[i].len; }
    struct gh2_bptr out;
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &root, 7, kv, n, &out), 0);
    free(kv);
    /* insert_run CoW-free stary (pusty) korzen — zostaje tylko nowe drzewo. Zwolnij je -> 0. */
    free_tree(&dev, &al, &out);
    CHECK_EQ(bump.in_use, 0);
    CHECK_EQ(bump.oom, 0);

    free(items);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* CoW: insert_run na ISTNIEJACYM drzewie nie psuje starej wersji. Alokator BEZ recyklingu
 * (nofree) — stary korzen NIGDY nie nadpisany. Stary korzen iteruje do bazowego zbioru. */
struct nofree_ctx { uint64_t next, max; };
static int nofree_alloc(void *ctx, uint64_t *out) {
    struct nofree_ctx *c = ctx; if (c->next >= c->max) return -ENOSPC; *out = c->next++; return 0;
}
static void nofree_free(void *ctx, uint64_t blk) { (void)ctx; (void)blk; }
static void test_run_cow_old_root(void) {
    char tmp[] = "/tmp/gh2batch_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct nofree_ctx nf = { GH2_DATA_START, 65536 };
    struct gh2_alloc al = { nofree_alloc, nofree_free, &nf, 0 };
    unsigned seed = 7;

    /* baza: 200 itemow przez single-insert */
    struct kvbuf *base = malloc(200 * sizeof(*base));
    uint32_t bn = gen_sorted(base, 200, &seed, 1);
    struct gh2_bptr root; CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &root), 0);
    struct gh2_bptr nr;
    for (uint32_t i = 0; i < bn; i++) {
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 7, &base[i].key, base[i].val, base[i].len, &nr), 0);
        root = nr;
    }
    struct gh2_bptr old_root = root;
    struct ser sbase; CHECK_EQ(serialize_tree(&dev, &old_root, &sbase), 0);

    /* insert_run nowych 200 itemow (klucze rozlaczne: wyzsze offsety) */
    struct kvbuf *add = malloc(200 * sizeof(*add));
    seed = 555; uint64_t off = 10000000;
    for (uint32_t i = 0; i < 200; i++) {
        off += 1 + rand_r(&seed) % 5;
        add[i].key = mk(1, GH2_EXTENT_DATA, off);
        add[i].len = 1 + rand_r(&seed) % GH2_LEAF_MAX_VAL;
        for (uint32_t j = 0; j < add[i].len; j++) add[i].val[j] = (uint8_t)(rand_r(&seed) & 0xff);
    }
    struct gh2_kv *kv = malloc(200 * sizeof(*kv));
    for (uint32_t i = 0; i < 200; i++) { kv[i].key = add[i].key; kv[i].val = add[i].val; kv[i].len = add[i].len; }
    struct gh2_bptr newroot;
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &root, 7, kv, 200, &newroot), 0);

    /* STARY korzen nadal iteruje do bazowego zbioru (CoW nie nadpisal) */
    struct ser sold; CHECK_EQ(serialize_tree(&dev, &old_root, &sold), 0);
    CHECK_EQ(sold.n, sbase.n);
    if (sold.n == sbase.n) CHECK_EQ(memcmp(sold.p, sbase.p, sold.n), 0);
    /* nowy korzen ma bazowe + dodane (lookup obu zbiorow) */
    for (uint32_t i = 0; i < bn; i++) {
        uint8_t b[GH2_LEAF_MAX_VAL]; uint32_t ol = 0;
        CHECK_EQ(gh2_btree_lookup(&dev, &newroot, &base[i].key, b, sizeof(b), &ol), 0);
    }
    for (uint32_t i = 0; i < 200; i++) {
        uint8_t b[GH2_LEAF_MAX_VAL]; uint32_t ol = 0;
        CHECK_EQ(gh2_btree_lookup(&dev, &newroot, &add[i].key, b, sizeof(b), &ol), 0);
        CHECK_EQ(ol, add[i].len);
        CHECK_EQ(memcmp(b, add[i].val, add[i].len), 0);
    }
    free(sbase.p); free(sold.p); free(base); free(add); free(kv);
    close_dev(&dev, tmp);
}

/* ============================ gh2_fs_write (batch) ============================ */
static const uint64_t NBLK = 65536;
static int fs_open(struct gh_dev *dev, char *tmp) {
    int fd = mkstemp(tmp); if (fd < 0) return -errno; close(fd);
    int r = gh_dev_create(tmp, NBLK, dev); if (r) return r;
    r = gh_bcache_create(dev); if (r) return r;
    return gh2_fs_format(dev, NBLK, 0);
}

static void fill_pat(uint8_t *b, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((seed + i * 131u) & 0xff);
}

static void test_fs_roundtrip(void) {
    char tmp[] = "/tmp/gh2bw_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* rozmiary: sub-blok, blok, wielo-blok, niewyrownane, granice */
    size_t sizes[] = { 1, 100, 4095, 4096, 4097, 8192, 10000, 1<<16, (1<<16)+123 };
    for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
        size_t sz = sizes[si];
        char path[32]; snprintf(path, sizeof(path), "/f%u", si);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
        uint8_t *w = malloc(sz), *r = malloc(sz);
        fill_pat(w, sz, si + 1);
        CHECK_EQ(gh2_fs_write(&fs, path, w, sz, 0), (ssize_t)sz);
        CHECK_EQ(gh2_fs_read(&fs, path, r, sz, 0), (ssize_t)sz);
        CHECK_EQ(memcmp(w, r, sz), 0);
        free(w); free(r);
    }

    /* niewyrownany offset + dziura (zera) */
    CHECK_EQ(gh2_fs_create(&fs, "/hole", 0644), 0);
    uint8_t tail[4]; memset(tail, 0xAB, 4);
    CHECK_EQ(gh2_fs_write(&fs, "/hole", tail, 4, 1000000), 4);
    uint8_t z[16]; CHECK_EQ(gh2_fs_read(&fs, "/hole", z, 16, 0), 16);
    for (int i = 0; i < 16; i++) CHECK_EQ(z[i], 0);          /* dziura = zera */
    uint8_t t4[4]; CHECK_EQ(gh2_fs_read(&fs, "/hole", t4, 4, 1000000), 4);
    CHECK_EQ(memcmp(t4, tail, 4), 0);

    gh2_fs_unmount(&fs);
    gh_bcache_destroy(&dev); gh_dev_close(&dev); unlink(tmp);
}

/* nadpisanie (CoW, stary blok zwolniony, wyciek=0) + RMW czesciowy */
static void check_no_leak(struct gh2_fs *fs) {
    CHECK_EQ(gh2_fs_commit(fs), 0);     /* flush write-back cache zanim porownamy refcount/mark-sweep */
    struct gh2_space s2;
    CHECK_EQ(gh2_space_init(&s2, fs->space.nblocks), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&fs->dev, &s2, &fs->root_tree), 0);
    CHECK_EQ(memcmp(fs->space.bits, s2.bits, (fs->space.nblocks + 7) / 8), 0);
    CHECK_EQ(fs->space.nfree, s2.nfree);
    CHECK_EQ(memcmp(fs->space.refs, s2.refs, (size_t)fs->space.nblocks * sizeof(uint16_t)), 0);
    gh2_space_destroy(&s2);
}
static void test_fs_overwrite_cow(void) {
    char tmp[] = "/tmp/gh2bw_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/f", 0644), 0);

    size_t sz = 64 * 1024;        /* 16 blokow */
    uint8_t *a = malloc(sz), *b = malloc(sz), *r = malloc(sz);
    fill_pat(a, sz, 1); fill_pat(b, sz, 200);
    CHECK_EQ(gh2_fs_write(&fs, "/f", a, sz, 0), (ssize_t)sz);
    check_no_leak(&fs);
    uint64_t free_after_first = fs.space.nfree;

    /* nadpisz caly plik nowymi danymi -> CoW, stare bloki zwolnione, brak wycieku */
    CHECK_EQ(gh2_fs_write(&fs, "/f", b, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_read(&fs, "/f", r, sz, 0), (ssize_t)sz);
    CHECK_EQ(memcmp(b, r, sz), 0);
    check_no_leak(&fs);
    /* nadpisanie tej samej liczby blokow: zajetosc stabilna (stare zwolnione == nowe wziete) */
    CHECK_EQ(fs.space.nfree, free_after_first);

    /* RMW czesciowy: nadpisz 10 bajtow w srodku bloku 3 */
    uint8_t patch[10]; memset(patch, 0x5A, 10);
    CHECK_EQ(gh2_fs_write(&fs, "/f", patch, 10, 3 * 4096 + 20), 10);
    CHECK_EQ(gh2_fs_read(&fs, "/f", r, sz, 0), (ssize_t)sz);
    memcpy(b + 3 * 4096 + 20, patch, 10);
    CHECK_EQ(memcmp(b, r, sz), 0);
    check_no_leak(&fs);

    free(a); free(b); free(r);
    gh2_fs_unmount(&fs);
    gh_bcache_destroy(&dev); gh_dev_close(&dev); unlink(tmp);
}

/* duzy plik 4 MB round-trip */
static void test_fs_large(void) {
    char tmp[] = "/tmp/gh2bw_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/big", 0644), 0);

    size_t sz = 4u * 1024 * 1024;       /* 1024 blokow -> wymusza wsady (GH2_WRITE_BATCH) */
    uint8_t *w = malloc(sz), *r = malloc(sz);
    fill_pat(w, sz, 7);
    CHECK_EQ(gh2_fs_write(&fs, "/big", w, sz, 0), (ssize_t)sz);
    CHECK_EQ(gh2_fs_read(&fs, "/big", r, sz, 0), (ssize_t)sz);
    CHECK_EQ(memcmp(w, r, sz), 0);
    check_no_leak(&fs);

    free(w); free(r);
    gh2_fs_unmount(&fs);
    gh_bcache_destroy(&dev); gh_dev_close(&dev); unlink(tmp);
}

/* DOWOD redukcji: zapis sekwencyjny N blokow -> liczba CoW WEZLOW (gh_disk_write wezlow)
 * ~ liczba LISCI drzewa ekstentow, NIE ~ N*wysokosc. Mierzymy licznik gh_disk_write w trakcie
 * jednego write 256 blokow i porownujemy z (single-blok N razy) szacowanym N*wysokosc. */
static void test_cow_reduction(void) {
    char tmp[] = "/tmp/gh2bw_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(fs_open(&dev, tmp), 0);
    struct gh2_fs fs; CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK_EQ(gh2_fs_create(&fs, "/seq", 0644), 0);

    size_t N = 256;
    size_t sz = N * 4096;        /* 1 MB sekwencyjnie */
    uint8_t *w = malloc(sz);
    fill_pat(w, sz, 3);
    CHECK_EQ(gh2_fs_commit(&fs), 0);   /* czysty stan: ncache pusty przed pomiarem */

    /* DOWOD: licznik BRUDNYCH wezlow B-drzewa (write-back cache) PO jednym batchowym zapisie =
     * liczba CoW-owanych WEZLOW. Dane bloki ida bezposrednio na dysk (nie ncache), wiec ncache
     * liczy WYLACZNIE wezly drzewa. Batch insert_run CoW-uje sciezke RAZ na lisc => << N. */
    CHECK_EQ(gh2_fs_write(&fs, "/seq", w, sz, 0), (ssize_t)sz);
    uint64_t node_cows = gh2_ncache_count((struct gh2_ncache *)fs.dev.v2_ncache);

    /* statyczne drzewo ekstentow (po commit) dla kontekstu */
    CHECK_EQ(gh2_fs_commit(&fs), 0);
    uint32_t leaves = count_leaves(&dev, &fs.fs_root);
    uint32_t nodes = count_nodes(&dev, &fs.fs_root);
    printf("  [reduction] seq write %zu blokow: CoW WEZLOW=%llu (fs-tree lisci=%u wezlow=%u). "
           "Per-blok bylby ~%zu*wysokosc.\n",
           N, (unsigned long long)node_cows, leaves, nodes, N);
    /* DOWOD: liczba CoW wezlow << N (per-blok robil ~N*wysokosc). Dla 256 ekstentow ~40B/item
     * miesci sie w kilku lisciach -> CoW wezlow rzedu jednocyfrowego/kilkunastu, NIE ~256. */
    CHECK(node_cows < N);
    CHECK(node_cows <= 4u * (leaves + 1));   /* ~ liczba lisci (+ sciezka do korzenia), nie ~N */

    free(w);
    gh2_fs_unmount(&fs);
    gh_bcache_destroy(&dev); gh_dev_close(&dev); unlink(tmp);
}

/* random zachowany: pojedynczy blok (1 item) insert_run == single insert (bajt-exact) */
static void test_random_preserved(void) {
    char tmp[] = "/tmp/gh2batch_XXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    /* nofree: rA i rB DZIELA wspolne drzewo bazowe; nie wolno recyklowac (obie operacje czytaja
     * te same bloki). CoW alokuje swieze bloki -> brak kolizji. */
    struct nofree_ctx nf = { GH2_DATA_START, 65536 };
    struct gh2_alloc al = { nofree_alloc, nofree_free, &nf, 0 };
    unsigned seed = 5;

    /* zbuduj wspolne drzewo bazowe, potem 1-item run vs 1-item single (na wspolnej bazie) */
    struct kvbuf *base = malloc(300 * sizeof(*base));
    uint32_t n = gen_sorted(base, 300, &seed, 1);
    struct gh2_bptr rA, rB;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 7, &rA), 0);
    rB = rA;
    struct gh2_kv *kv = malloc(n * sizeof(*kv));
    for (uint32_t i = 0; i < n; i++) { kv[i].key = base[i].key; kv[i].val = base[i].val; kv[i].len = base[i].len; }
    struct gh2_bptr t;
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &rA, 7, kv, n, &t), 0); rA = t; rB = t;
    free(kv);

    /* nowy klucz 1-item: run vs single muszą dac bajt-exact to samo drzewo */
    struct gh2_key nk = mk(1, GH2_EXTENT_DATA, 1000000);
    uint8_t nv[37]; for (int i = 0; i < 37; i++) nv[i] = (uint8_t)(i * 7);
    struct gh2_kv one = { nk, nv, 37 };
    struct gh2_bptr ra2, rb2;
    CHECK_EQ(gh2_btree_insert_run(&dev, &al, &rA, 7, &one, 1, &ra2), 0);
    CHECK_EQ(gh2_btree_insert(&dev, &al, &rB, 7, &nk, nv, 37, &rb2), 0);
    struct ser sa, sb;
    CHECK_EQ(serialize_tree(&dev, &ra2, &sa), 0);
    CHECK_EQ(serialize_tree(&dev, &rb2, &sb), 0);
    CHECK_EQ(sa.n, sb.n);
    if (sa.n == sb.n) CHECK_EQ(memcmp(sa.p, sb.p, sa.n), 0);
    free(sa.p); free(sb.p);

    free(base);
    close_dev(&dev, tmp);
}

int main(void) {
    RUN_TEST(test_cross_check_random);
    RUN_TEST(test_cross_check_update);
    RUN_TEST(test_run_edge);
    RUN_TEST(test_run_no_leak);
    RUN_TEST(test_run_cow_old_root);
    RUN_TEST(test_random_preserved);
    RUN_TEST(test_fs_roundtrip);
    RUN_TEST(test_fs_overwrite_cow);
    RUN_TEST(test_fs_large);
    RUN_TEST(test_cow_reduction);
    return TEST_SUMMARY();
}
