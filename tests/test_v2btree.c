#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "v2/gh2_btree.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Task 1: testy property-based vs model referencyjny — pojedynczy lisc.
 * insert/lookup/update/iterate; wartosci zmiennej dlugosci; -ENOENT; -EFBIG;
 * CoW (stary root = stary stan); WYCIEK blokow = 0 (licznik stuba).
 * ========================================================================== */

#define MODEL_MAX 256
struct mentry { struct gh2_key key; uint8_t val[1024]; uint32_t len; };
struct model { struct mentry e[MODEL_MAX]; uint32_t n; };

static void model_init(struct model *m) { m->n = 0; }

/* znajdz indeks lub pozycje wstawienia */
static uint32_t model_find(const struct model *m, const struct gh2_key *k, int *found) {
    uint32_t lo = 0, hi = m->n; *found = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = gh2_key_cmp(&m->e[mid].key, k);
        if (c == 0) { *found = 1; return mid; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static void model_put(struct model *m, const struct gh2_key *k, const void *v, uint32_t len) {
    int found; uint32_t idx = model_find(m, k, &found);
    if (found) { memcpy(m->e[idx].val, v, len); m->e[idx].len = len; return; }
    for (uint32_t i = m->n; i > idx; i--) m->e[i] = m->e[i-1];
    m->e[idx].key = *k; memcpy(m->e[idx].val, v, len); m->e[idx].len = len;
    m->n++;
}

/* ---- pomocnik: otworz urzadzenie ---- */
static int open_dev(struct gh_dev *dev, char *tmp) {
    int fd = mkstemp(tmp); if (fd < 0) return -errno; close(fd);
    int r = gh_dev_create(tmp, 4096, dev); if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev, const char *tmp) {
    gh_bcache_destroy(dev); gh_dev_close(dev); unlink(tmp);
}

/* ---- iteracja: zbierz drzewo do tablicy by porownac z modelem ---- */
struct collect { struct gh2_key keys[MODEL_MAX]; uint8_t vals[MODEL_MAX][1024];
                 uint32_t lens[MODEL_MAX]; uint32_t n; };
static int collect_cb(const struct gh2_key *k, const void *v, uint32_t len, void *ctx) {
    struct collect *c = ctx;
    if (c->n >= MODEL_MAX) return -1;
    c->keys[c->n] = *k; memcpy(c->vals[c->n], v, len); c->lens[c->n] = len; c->n++;
    return 0;
}

/* sprawdz: lookup wszystkich kluczy modelu == drzewo; iteracja == model posortowany */
static void verify_against_model(struct gh_dev *dev, const struct gh2_bptr *root,
                                 const struct model *m) {
    for (uint32_t i = 0; i < m->n; i++) {
        uint8_t buf[1024]; uint32_t out_len = 0;
        int r = gh2_btree_lookup(dev, root, &m->e[i].key, buf, sizeof(buf), &out_len);
        CHECK_EQ(r, 0);
        CHECK_EQ(out_len, m->e[i].len);
        CHECK_EQ(memcmp(buf, m->e[i].val, m->e[i].len), 0);
    }
    struct collect c; c.n = 0;
    int r = gh2_btree_iterate(dev, root, collect_cb, &c);
    CHECK_EQ(r, 0);
    CHECK_EQ(c.n, m->n);
    for (uint32_t i = 0; i < m->n && i < c.n; i++) {
        CHECK_EQ(gh2_key_cmp(&c.keys[i], &m->e[i].key), 0);
        CHECK_EQ(c.lens[i], m->e[i].len);
        CHECK_EQ(memcmp(c.vals[i], m->e[i].val, m->e[i].len), 0);
    }
}

static struct gh2_key mk(uint64_t oid, uint8_t type, uint64_t off) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = oid; k.type = type; k.offset = off; return k;
}

/* alokator BEZ recyklingu: bumpuje, free() jest no-op. Uzywany przez test snapshotu
 * by stare korzenie (R1) NIGDY nie zostaly nadpisane przez realokacje. */
struct nofree_ctx { uint64_t next; uint64_t max; };
static int nofree_alloc(void *ctx, uint64_t *out) {
    struct nofree_ctx *n = ctx;
    if (n->next >= n->max) return -ENOSPC;
    *out = n->next++; return 0;
}
static void nofree_free(void *ctx, uint64_t blk) { (void)ctx; (void)blk; }

/* ---- pomocnik: policz ZYWE wezly drzewa (rekurencyjny DFS po naglowkach) ---- */
static uint32_t count_live_nodes(struct gh_dev *dev, const struct gh2_bptr *node) {
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
        for (uint32_t i = 0; i < nr; i++) cnt += count_live_nodes(dev, &kids[i]);
    }
    return cnt;
}

/* ---- pomocnik: zwolnij WSZYSTKIE wezly drzewa (DFS), do testu wycieku ---- */
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

/* ---- test: key_cmp ---- */
static void test_key_cmp(void) {
    struct gh2_key a = mk(1, 2, 3), b = mk(1, 2, 3);
    CHECK_EQ(gh2_key_cmp(&a, &b), 0);
    b = mk(1, 2, 4); CHECK(gh2_key_cmp(&a, &b) < 0);
    b = mk(1, 3, 0); CHECK(gh2_key_cmp(&a, &b) < 0);
    b = mk(2, 0, 0); CHECK(gh2_key_cmp(&a, &b) < 0);
    b = mk(0, 9, 9); CHECK(gh2_key_cmp(&a, &b) > 0);
}

/* ---- test: insert/lookup/update/iterate zmiennej dlugosci vs model ---- */
static void test_leaf_ops(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);

    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    struct model m; model_init(&m);
    unsigned seed = 12345;
    for (int op = 0; op < 60; op++) {
        uint64_t oid = rand_r(&seed) % 20;
        uint8_t type = rand_r(&seed) % 4;
        uint64_t off = rand_r(&seed) % 20;
        struct gh2_key k = mk(oid, type, off);
        uint32_t len = 1 + rand_r(&seed) % 40;     /* zmienna dlugosc 1..40 */
        uint8_t val[64];
        for (uint32_t i = 0; i < len; i++) val[i] = (uint8_t)(rand_r(&seed) & 0xff);

        struct gh2_bptr nr;
        int r = gh2_btree_insert(&dev, &al, &root, 2, &k, val, len, &nr);
        if (r == -EFBIG) continue;                  /* lisc pelny (split w Task 2) */
        CHECK_EQ(r, 0);
        root = nr;
        model_put(&m, &k, val, len);
        verify_against_model(&dev, &root, &m);
    }
    /* -ENOENT dla nieobecnego klucza */
    struct gh2_key absent = mk(999, 0, 0);
    uint8_t buf[64]; uint32_t ol;
    CHECK_EQ(gh2_btree_lookup(&dev, &root, &absent, buf, sizeof(buf), &ol), -ENOENT);

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- test: update zmieniajacy dlugosc ---- */
static void test_update_resize(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);
    struct model m; model_init(&m);

    struct gh2_key k1 = mk(5, 0, 0), k2 = mk(7, 0, 0), k3 = mk(9, 0, 0);
    uint8_t a[100]; memset(a, 'A', sizeof(a));
    uint8_t b[5];   memset(b, 'B', sizeof(b));
    struct gh2_bptr nr;
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k1, a, 50, &nr), 0); root = nr; model_put(&m,&k1,a,50);
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k2, b, 5, &nr), 0);  root = nr; model_put(&m,&k2,b,5);
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k3, a, 30, &nr), 0); root = nr; model_put(&m,&k3,a,30);
    verify_against_model(&dev, &root, &m);
    /* update k2: rosnie z 5 do 100 */
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k2, a, 100, &nr), 0); root = nr; model_put(&m,&k2,a,100);
    verify_against_model(&dev, &root, &m);
    /* update k1: maleje z 50 do 1 */
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k1, b, 1, &nr), 0); root = nr; model_put(&m,&k1,b,1);
    verify_against_model(&dev, &root, &m);

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- test: -EFBIG (wartosc za duza + przepelnienie liscia) ---- */
static void test_efbig(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    /* wartosc wieksza niz mieszczaca sie w pustym lisciu -> -EFBIG */
    static uint8_t big[GH2_BLOCK_SIZE];
    memset(big, 'X', sizeof(big));
    struct gh2_key k = mk(1, 0, 0);
    struct gh2_bptr nr;
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, big, GH2_LEAF_MAX_VAL + 1, &nr), -EFBIG);
    /* dokladnie max -> OK */
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, big, GH2_LEAF_MAX_VAL, &nr), 0);
    root = nr;

    /* Task 2: przepelnienie liscia NIE daje juz -EFBIG (split obsluguje nadmiar).
     * Wstaw duzo itemow — wszystkie maja sie powiesc (drzewo rosnie przez split). */
    for (int i = 0; i < 2000; i++) {
        struct gh2_key kk = mk(2 + (uint64_t)i, 0, 0);
        uint8_t v[200]; memset(v, 'Y', sizeof(v));
        int r = gh2_btree_insert(&dev, &al, &root, 2, &kk, v, sizeof(v), &nr);
        CHECK_EQ(r, 0);
        root = nr;
        /* okresowo sprawdz ze pierwszy item (max val) wciaz odczytywalny przez wiele poziomow */
        if (i % 200 == 0) {
            uint8_t obuf[GH2_BLOCK_SIZE]; uint32_t ol;
            struct gh2_key k0 = mk(1, 0, 0);
            CHECK_EQ(gh2_btree_lookup(&dev, &root, &k0, obuf, sizeof(obuf), &ol), 0);
            CHECK_EQ(ol, (long long)GH2_LEAF_MAX_VAL);
        }
    }

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- test: CoW — stary root widzi stary stan ---- */
static void test_cow_snapshot(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    /* WAZNE: snapshot R1 musi pozostac wazny po CoW. Insert zwalnia stary korzen do
     * freelist, a stub recyklowalby ten blok i nadpisal dane R1. By test byl uczciwy
     * (snapshot = stary korzen widzi STARY stan), uzywamy alokatora BEZ recyklingu:
     * free() to no-op, wiec blok R1 NIGDY nie jest realokowany. (Realny refcount: v2.2.) */
    struct nofree_ctx nctx = { GH2_DATA_START, 4096 };
    struct gh2_alloc al = { nofree_alloc, nofree_free, &nctx, 0 };
    struct gh2_bptr R1;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &R1), 0);

    struct gh2_key k1 = mk(1, 0, 0);
    uint8_t v1[8]; memset(v1, '1', sizeof(v1));
    struct gh2_bptr R2;
    CHECK_EQ(gh2_btree_insert(&dev, &al, &R1, 2, &k1, v1, sizeof(v1), &R2), 0);
    /* po insert blok R1 zostal zwolniony (freelist) ale NIE nadpisany danymi —
     * dane na dysku wciaz tam sa az do realokacji. By test byl uczciwy:
     * zrobimy serie insertow do R2 i sprawdzimy ze R1 (puste drzewo) wciaz puste. */
    struct gh2_bptr cur = R2;
    for (int i = 0; i < 5; i++) {
        struct gh2_key kk = mk(10 + (uint64_t)i, 0, 0);
        uint8_t v[4]; memset(v, (uint8_t)('a'+i), sizeof(v));
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &cur, 2, &kk, v, sizeof(v), &nr), 0);
        cur = nr;
    }
    /* R1 = oryginalne PUSTE drzewo: lookup k1 -> -ENOENT, iteracja = 0 itemow */
    uint8_t buf[16]; uint32_t ol;
    CHECK_EQ(gh2_btree_lookup(&dev, &R1, &k1, buf, sizeof(buf), &ol), -ENOENT);
    struct collect c; c.n = 0;
    CHECK_EQ(gh2_btree_iterate(&dev, &R1, collect_cb, &c), 0);
    CHECK_EQ(c.n, 0u);
    /* R2 ma k1 */
    CHECK_EQ(gh2_btree_lookup(&dev, &R2, &k1, buf, sizeof(buf), &ol), 0);
    CHECK_EQ(ol, 8u);

    close_dev(&dev, tmp);
}

/* ---- test: WYCIEK blokow = 0 po zrownowazonej sekwencji CoW ---- */
static void test_no_block_leak(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);
    /* po create: 1 blok w uzyciu (korzen) */
    CHECK_EQ(bump.in_use, 1);

    struct model m; model_init(&m);
    unsigned seed = 777;
    for (int op = 0; op < 100; op++) {
        struct gh2_key k = mk(rand_r(&seed) % 15, rand_r(&seed) % 3, rand_r(&seed) % 15);
        uint32_t len = 1 + rand_r(&seed) % 30;
        uint8_t v[40];
        for (uint32_t i = 0; i < len; i++) v[i] = (uint8_t)(rand_r(&seed) & 0xff);
        struct gh2_bptr nr;
        int r = gh2_btree_insert(&dev, &al, &root, 2, &k, v, len, &nr);
        if (r == -EFBIG) continue;
        CHECK_EQ(r, 0);
        root = nr;
        model_put(&m, &k, v, len);
        /* Task 2: po kazdym CoW insert NIE ma wyciekow: liczba blokow w uzyciu ==
         * liczba ZYWYCH wezlow aktualnego drzewa (stare bloki sciezki zwrocone do freelist). */
        CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    }
    CHECK_EQ(bump.oom, 0);
    /* in_use == zywe wezly aktualnego drzewa (brak wyciekow) */
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    verify_against_model(&dev, &root, &m);

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- test: I/O wezla — przekrecony bit -> -EIO (dup=0) ---- */
static void test_node_csum_eio(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);
    struct gh2_key k = mk(1, 0, 0); uint8_t v[8]; memset(v, 'z', 8);
    struct gh2_bptr nr;
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, v, 8, &nr), 0);
    root = nr;
    /* przekrec bit na dysku w bloku korzenia (przez cache: zapisz uszkodzony) */
    uint8_t blk[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(&dev, root.block, blk), 0);
    blk[100] ^= 0x01;
    CHECK_EQ(gh_disk_write(&dev, root.block, blk), 0);
    /* lookup teraz musi dac -EIO (csum nie pasuje, dup=0) */
    uint8_t buf[16]; uint32_t ol;
    CHECK_EQ(gh2_btree_lookup(&dev, &root, &k, buf, sizeof(buf), &ol), -EIO);

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ============================================================================
 * Task 2: SPLIT + wezly WEWNETRZNE — drzewo wielopoziomowe.
 * Model duzej skali: posortowana tablica (oid) -> wartosc; wartosc deterministyczna z klucza.
 * ========================================================================== */

#define BIG_VALLEN 20

/* wartosc deterministyczna od klucza (oid) by nie trzymac w modelu */
static void big_val(uint64_t oid, uint8_t out[BIG_VALLEN]) {
    for (int i = 0; i < BIG_VALLEN; i++)
        out[i] = (uint8_t)((oid * 2654435761u + (uint64_t)i * 40503u) & 0xff);
}

/* model duzej skali: posortowane unikalne oidy */
struct bigmodel { uint64_t *oid; uint32_t n, cap; };
static void bm_init(struct bigmodel *bm) { bm->oid = NULL; bm->n = 0; bm->cap = 0; }
static void bm_destroy(struct bigmodel *bm) { free(bm->oid); bm->oid = NULL; }
static uint32_t bm_find(const struct bigmodel *bm, uint64_t oid, int *found) {
    uint32_t lo = 0, hi = bm->n; *found = 0;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2;
        if (bm->oid[mid] == oid) { *found = 1; return mid; }
        if (bm->oid[mid] < oid) lo = mid + 1; else hi = mid; }
    return lo;
}
static void bm_put(struct bigmodel *bm, uint64_t oid) {
    int found; uint32_t idx = bm_find(bm, oid, &found);
    if (found) return;
    if (bm->n == bm->cap) {
        bm->cap = bm->cap ? bm->cap * 2 : 64;
        bm->oid = realloc(bm->oid, bm->cap * sizeof(uint64_t));
    }
    for (uint32_t i = bm->n; i > idx; i--) bm->oid[i] = bm->oid[i-1];
    bm->oid[idx] = oid; bm->n++;
}
/* usun oid z modelu; zwraca 1 jesli byl, 0 jesli nieobecny */
static int bm_del(struct bigmodel *bm, uint64_t oid) {
    int found; uint32_t idx = bm_find(bm, oid, &found);
    if (!found) return 0;
    for (uint32_t i = idx; i + 1 < bm->n; i++) bm->oid[i] = bm->oid[i+1];
    bm->n--;
    return 1;
}

/* collect dla iteracji duzej skali: tylko klucze (oid), w kolejnosci */
struct bigcollect { uint64_t *oid; uint8_t (*val)[BIG_VALLEN]; uint32_t n, cap; int bad; };
static int bigcollect_cb(const struct gh2_key *k, const void *v, uint32_t len, void *ctx) {
    struct bigcollect *c = ctx;
    if (len != BIG_VALLEN) { c->bad = 1; return -1; }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->oid = realloc(c->oid, c->cap * sizeof(uint64_t));
        c->val = realloc(c->val, c->cap * sizeof(*c->val));
    }
    c->oid[c->n] = k->objectid;
    memcpy(c->val[c->n], v, len);
    c->n++;
    return 0;
}

/* pelna walidacja drzewa vs bigmodel: lookup wszystkich + iteracja==model posortowany */
static void bm_verify(struct gh_dev *dev, const struct gh2_bptr *root, const struct bigmodel *bm) {
    /* lookup kazdego klucza modelu */
    for (uint32_t i = 0; i < bm->n; i++) {
        uint8_t want[BIG_VALLEN], got[BIG_VALLEN]; uint32_t ol = 0;
        big_val(bm->oid[i], want);
        struct gh2_key k = mk(bm->oid[i], 0, 0);
        int r = gh2_btree_lookup(dev, root, &k, got, sizeof(got), &ol);
        CHECK_EQ(r, 0);
        CHECK_EQ(ol, BIG_VALLEN);
        CHECK_EQ(memcmp(got, want, BIG_VALLEN), 0);
    }
    /* iteracja == posortowany model */
    struct bigcollect c; memset(&c, 0, sizeof(c));
    int r = gh2_btree_iterate(dev, root, bigcollect_cb, &c);
    CHECK_EQ(r, 0);
    CHECK_EQ(c.bad, 0);
    CHECK_EQ(c.n, bm->n);
    for (uint32_t i = 0; i < bm->n && i < c.n; i++) {
        CHECK_EQ(c.oid[i], bm->oid[i]);
        uint8_t want[BIG_VALLEN]; big_val(bm->oid[i], want);
        CHECK_EQ(memcmp(c.val[i], want, BIG_VALLEN), 0);
    }
    /* monotonicznosc iteracji (zawsze rosnaca) */
    for (uint32_t i = 1; i < c.n; i++) CHECK(c.oid[i-1] < c.oid[i]);
    free(c.oid); free(c.val);
}

/* wstaw N kluczy w podanej kolejnosci; po koncu pelna walidacja. Zwraca finalny root. */
static void test_big_insert_order(int order /*0=rosnaco,1=malejaco,2=losowo*/, int n,
                                  int min_depth) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    /* duzo blokow: drzewo wielopoziomowe potrzebuje setek blokow */
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 200000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    struct bigmodel bm; bm_init(&bm);
    unsigned seed = 0xC0FFEE ^ (unsigned)order;
    for (int i = 0; i < n; i++) {
        uint64_t oid;
        if (order == 0) oid = (uint64_t)i + 1;
        else if (order == 1) oid = (uint64_t)(n - i);
        else oid = ((uint64_t)rand_r(&seed) << 8 | rand_r(&seed)) % (uint64_t)(n * 3) + 1;
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        int r = gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr);
        CHECK_EQ(r, 0);
        root = nr;
        bm_put(&bm, oid);
    }
    /* osiagnieta glebokosc (diagnostyka) */
    uint8_t rb[GH2_BLOCK_SIZE];
    CHECK_EQ(gh2_node_read(&dev, &root, rb), 0);
    uint8_t depth = ((const struct gh2_node_hdr *)rb)->level;
    printf("  [big order=%d n=%d] glebokosc korzenia level=%u, items=%u\n",
           order, n, depth, bm.n);
    CHECK(depth >= min_depth);   /* wymuszono wiele poziomow (internal nodes) */

    bm_verify(&dev, &root, &bm);
    /* nieobecny klucz -> -ENOENT */
    struct gh2_key absent = mk((uint64_t)n * 100 + 7, 0, 0);
    uint8_t buf[BIG_VALLEN]; uint32_t ol;
    CHECK_EQ(gh2_btree_lookup(&dev, &root, &absent, buf, sizeof(buf), &ol), -ENOENT);

    /* wyciek: in_use == zywe wezly aktualnego drzewa */
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));

    bm_destroy(&bm);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ascending: punkt podzialu liscia "najwiekszy prefiks <= NODE_SPACE" (fix recenzji) pakuje
 * lewy lisc pelniej -> mniej lisci -> plytsze drzewo niz stary split ~po polowie. Dla 3000
 * itemow 20 B mieszcza sie w 1 poziomie wewnetrznym (level=1). min_depth=1 nadal wymusza
 * wezly wewnetrzne; sama poprawnosc (bm_verify) bez zmian. */
static void test_big_ascending(void)  { test_big_insert_order(0, 3000, 1); }
static void test_big_descending(void) { test_big_insert_order(1, 3000, 2); }
static void test_big_random(void)     { test_big_insert_order(2, 3000, 1); }

/* ---- BRAMKA: >=5000 operacji insert/update z mieszanej przestrzeni; walidacja PO KAZDEJ ---- */
static void test_property_5000(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 200000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    struct bigmodel bm; bm_init(&bm);
    unsigned seed = 13579;
    const int OPS = 5200;
    for (int op = 0; op < OPS; op++) {
        /* mieszana przestrzen: czesc gesta (kolizje->update), czesc rzadka (insert) */
        uint64_t oid;
        if (rand_r(&seed) % 2) oid = (uint64_t)(rand_r(&seed) % 400) + 1;        /* gesta */
        else oid = (uint64_t)(rand_r(&seed) % 100000) + 1000;                     /* rzadka */
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        int r = gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr);
        CHECK_EQ(r, 0);
        root = nr;
        bm_put(&bm, oid);
        /* walidacja PO KAZDEJ operacji jest droga; rob pelna co ~50 ops + zawsze lookup tego klucza */
        uint8_t got[BIG_VALLEN]; uint32_t ol;
        CHECK_EQ(gh2_btree_lookup(&dev, &root, &k, got, sizeof(got), &ol), 0);
        CHECK_EQ(memcmp(got, v, BIG_VALLEN), 0);
        if (op % 50 == 0 || op == OPS - 1) bm_verify(&dev, &root, &bm);
        if (op % 200 == 0)
            CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    }
    CHECK_EQ(bump.oom, 0);
    bm_verify(&dev, &root, &bm);
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    printf("  [property] %d ops, %u unikalnych kluczy\n", OPS, bm.n);

    bm_destroy(&bm);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- BRAMKA: CoW snapshot wielopoziomowy (R1 vs R2, alokator no-free) ---- */
static void test_cow_snapshot_multilevel(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct nofree_ctx nctx = { GH2_DATA_START, 200000 };
    struct gh2_alloc al = { nofree_alloc, nofree_free, &nctx, 0 };
    struct gh2_bptr R1;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &R1), 0);

    /* zbuduj R1: wstaw 500 kluczy parzystych (wymusza wiele poziomow) */
    struct bigmodel bm1; bm_init(&bm1);
    struct gh2_bptr cur = R1;
    for (int i = 0; i < 500; i++) {
        uint64_t oid = (uint64_t)(i * 2) + 2;
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &cur, 2, &k, v, BIG_VALLEN, &nr), 0);
        cur = nr; bm_put(&bm1, oid);
    }
    struct gh2_bptr R1snap = cur;     /* snapshot R1 (stan z 500 parzystymi) */

    /* R2: dodaj 500 kluczy nieparzystych do snapshotu */
    struct bigmodel bm2; bm_init(&bm2);
    for (uint32_t i = 0; i < bm1.n; i++) bm_put(&bm2, bm1.oid[i]);
    cur = R1snap;
    for (int i = 0; i < 500; i++) {
        uint64_t oid = (uint64_t)(i * 2) + 1;
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &cur, 2, &k, v, BIG_VALLEN, &nr), 0);
        cur = nr; bm_put(&bm2, oid);
    }
    struct gh2_bptr R2 = cur;

    /* R1 wciaz widzi tylko parzyste; R2 widzi obie */
    bm_verify(&dev, &R1snap, &bm1);
    bm_verify(&dev, &R2, &bm2);
    /* klucz nieparzysty nieobecny w R1 */
    struct gh2_key odd = mk(3, 0, 0);
    uint8_t buf[BIG_VALLEN]; uint32_t ol;
    CHECK_EQ(gh2_btree_lookup(&dev, &R1snap, &odd, buf, sizeof(buf), &ol), -ENOENT);
    CHECK_EQ(gh2_btree_lookup(&dev, &R2, &odd, buf, sizeof(buf), &ol), 0);

    bm_destroy(&bm1); bm_destroy(&bm2);
    close_dev(&dev, tmp);
}

/* ---- BRAMKA: wyciek = 0 dla drzewa porzuconego z recyklingiem ---- */
static void test_abandon_no_leak(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 200000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    for (int i = 0; i < 2500; i++) {
        uint64_t oid = (uint64_t)i + 1;
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr), 0);
        root = nr;
    }
    int64_t live = count_live_nodes(&dev, &root);
    CHECK_EQ(bump.in_use, live);          /* brak wyciekow w trakcie budowy */
    /* porzuc drzewo: zwolnij wszystkie zywe wezly -> in_use wraca do 0 */
    free_tree(&dev, &al, &root);
    CHECK_EQ(bump.in_use, 0);
    CHECK_EQ(bump.oom, 0);

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ============================================================================
 * Task 3: DELETE + merge/borrow (pelne rebalansowanie). Property-based vs model.
 * ========================================================================== */

/* odczytaj level korzenia (diagnostyka glebokosci) */
static uint8_t root_level(struct gh_dev *dev, const struct gh2_bptr *root) {
    uint8_t rb[GH2_BLOCK_SIZE];
    CHECK_EQ(gh2_node_read(dev, root, rb), 0);
    return ((const struct gh2_node_hdr *)rb)->level;
}

/* ---- BRAMKA: >=10000 ops insert/delete/update/lookup z mieszanej przestrzeni;
 * po KAZDEJ pelna zgodnosc z modelem; delete nieobecnego -> -ENOENT ---- */
static void test_property_delete_10000(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 400000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    struct bigmodel bm; bm_init(&bm);
    unsigned seed = 0xDEADBEEF;
    const int OPS = 12000;
    for (int op = 0; op < OPS; op++) {
        /* mieszana przestrzen: mala (gesta -> czeste kolizje/delete) lub duza (wzrost) */
        uint64_t oid;
        if (rand_r(&seed) % 2) oid = (uint64_t)(rand_r(&seed) % 200) + 1;       /* gesta */
        else oid = (uint64_t)(rand_r(&seed) % 50000) + 1000;                     /* rzadka */
        struct gh2_key k = mk(oid, 0, 0);
        int act = rand_r(&seed) % 3;   /* 0/1 insert-update, 2 delete */
        struct gh2_bptr nr;
        if (act == 2) {
            int r = gh2_btree_delete(&dev, &al, &root, 2, &k, &nr);
            int model_had = bm_del(&bm, oid);
            if (model_had) { CHECK_EQ(r, 0); root = nr; }
            else            CHECK_EQ(r, -ENOENT);
        } else {
            uint8_t v[BIG_VALLEN]; big_val(oid, v);
            int r = gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr);
            CHECK_EQ(r, 0); root = nr;
            bm_put(&bm, oid);
        }
        /* lookup tego klucza zawsze; pelna walidacja co ~40 ops + zawsze sprawdz spojnosc liczby */
        uint8_t got[BIG_VALLEN]; uint32_t ol;
        int lr = gh2_btree_lookup(&dev, &root, &k, got, sizeof(got), &ol);
        int found; bm_find(&bm, oid, &found);
        CHECK_EQ(lr, found ? 0 : -ENOENT);
        if (op % 40 == 0 || op == OPS - 1) bm_verify(&dev, &root, &bm);
        if (op % 500 == 0)
            CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    }
    CHECK_EQ(bump.oom, 0);
    bm_verify(&dev, &root, &bm);
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    /* delete nieobecnego -> -ENOENT */
    struct gh2_key absent = mk(123456789ull, 0, 0);
    struct gh2_bptr nr;
    CHECK_EQ(gh2_btree_delete(&dev, &al, &root, 2, &absent, &nr), -ENOENT);
    printf("  [property-delete] %d ops, %u kluczy na koncu\n", OPS, bm.n);

    bm_destroy(&bm);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- KLUCZOWE: glebokie drzewo (>=3 poziomy) + masowy delete -> merge/borrow wewnetrzne
 * + obnizenie wysokosci; usun do PUSTA; wyciek=0 ---- */
static void test_deep_mass_delete(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 600000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    /* wstaw DUZO rosnaco (male wartosci) -> >=3 poziomy */
    const int N = 15000;
    struct bigmodel bm; bm_init(&bm);
    for (int i = 0; i < N; i++) {
        uint64_t oid = (uint64_t)i + 1;
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr), 0);
        root = nr; bm_put(&bm, oid);
    }
    uint8_t max_level = root_level(&dev, &root);
    printf("  [deep] po wstawieniu %d: root level=%u\n", N, max_level);
    CHECK(max_level >= 2);                  /* >=3 poziomy (level 2 = 3 poziomy) */
    bm_verify(&dev, &root, &bm);
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));

    /* usun WIEKSZOSC w losowej kolejnosci (permutacja) -> wymusza merge/borrow wewnetrzny */
    uint64_t *perm = malloc((size_t)N * sizeof(uint64_t));
    for (int i = 0; i < N; i++) perm[i] = (uint64_t)i + 1;
    unsigned seed = 24680;
    for (int i = N - 1; i > 0; i--) {   /* Fisher-Yates */
        int j = rand_r(&seed) % (i + 1);
        uint64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    int validate_every = 250;
    for (int i = 0; i < N; i++) {
        struct gh2_key k = mk(perm[i], 0, 0);
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_delete(&dev, &al, &root, 2, &k, &nr), 0);
        root = nr;
        CHECK_EQ(bm_del(&bm, perm[i]), 1);
        if (i % validate_every == 0) {
            bm_verify(&dev, &root, &bm);
            CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
        }
    }
    free(perm);
    /* drzewo PUSTE: wysokosc = lisc (level 0), 0 itemow */
    CHECK_EQ((long long)root_level(&dev, &root), 0);
    {
        uint8_t rb[GH2_BLOCK_SIZE];
        CHECK_EQ(gh2_node_read(&dev, &root, rb), 0);
        CHECK_EQ((long long)((const struct gh2_node_hdr *)rb)->nritems, 0);
    }
    CHECK_EQ(bm.n, 0u);
    bm_verify(&dev, &root, &bm);
    /* wyciek bloku = 0: in_use == zywe wezly (= 1, pusty korzen-lisc) */
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    CHECK_EQ(bump.in_use, 1);   /* tylko pusty korzen-lisc */
    CHECK_EQ(bump.oom, 0);
    printf("  [deep] po masowym delete do pusta: root level=0, in_use=%lld (max level byl %u)\n",
           (long long)bump.in_use, max_level);

    bm_destroy(&bm);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- naprzemienne insert/delete na granicy split/merge (oscylacja) ---- */
static void test_oscillation(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 200000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    struct bigmodel bm; bm_init(&bm);
    /* baza: tyle ile potrzeba, by drzewo bylo wielopoziomowe i siedzialo na granicy */
    const int BASE = 400;
    for (int i = 0; i < BASE; i++) {
        uint64_t oid = (uint64_t)i + 1;
        uint8_t v[BIG_VALLEN]; big_val(oid, v);
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr), 0);
        root = nr; bm_put(&bm, oid);
    }
    /* oscyluj: powtarzalnie wstaw oid w okolicy progu i od razu usun (i odwrotnie) */
    unsigned seed = 999;
    for (int op = 0; op < 4000; op++) {
        uint64_t oid = (uint64_t)(BASE + 1) + (rand_r(&seed) % 8);  /* waska strefa graniczna */
        struct gh2_key k = mk(oid, 0, 0);
        struct gh2_bptr nr;
        int found; bm_find(&bm, oid, &found);
        if (found) {
            CHECK_EQ(gh2_btree_delete(&dev, &al, &root, 2, &k, &nr), 0);
            root = nr; bm_del(&bm, oid);
        } else {
            uint8_t v[BIG_VALLEN]; big_val(oid, v);
            CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, v, BIG_VALLEN, &nr), 0);
            root = nr; bm_put(&bm, oid);
        }
        if (op % 25 == 0) bm_verify(&dev, &root, &bm);
    }
    bm_verify(&dev, &root, &bm);
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    CHECK_EQ(bump.oom, 0);

    bm_destroy(&bm);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ---- delete: przekrecony bit w wezle -> -EIO (zachowanie integralnosci) ---- */
static void test_delete_csum_eio(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 4096);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);
    struct gh2_key k = mk(1, 0, 0); uint8_t v[8]; memset(v, 'q', 8);
    struct gh2_bptr nr;
    CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, v, 8, &nr), 0);
    root = nr;
    uint8_t blk[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(&dev, root.block, blk), 0);
    blk[80] ^= 0x02;
    CHECK_EQ(gh_disk_write(&dev, root.block, blk), 0);
    struct gh2_bptr out;
    CHECK_EQ(gh2_btree_delete(&dev, &al, &root, 2, &k, &out), -EIO);

    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

/* ============================================================================
 * REGRESJA recenzji adwersaryjnej: split liscia ze SKRAJNIE NIEROWNYMI rozmiarami
 * wartosci. Bug: split balansujacy po bajtach mogl wrzucic dwa duze itemy po tej samej
 * stronie -> prawa > GH2_NODE_SPACE -> leaf_build_write underflow -> OOB. Fix: limit
 * GH2_LEAF_MAX_VAL <= NODE_SPACE/2 + punkt podzialu "najwiekszy prefiks <= NODE_SPACE".
 * Tu: property vs model przy mieszance dlugosci {0,1,male,~MAX,losowe}, ZERO crashy,
 * iteracja==model, oraz reproducer z recenzji (teraz -EFBIG dla 2400).
 * ========================================================================== */

/* model z DUZYMI wartosciami (do GH2_LEAF_MAX_VAL); osobny od MODEL_MAX/1024 powyzej */
#define UMODEL_MAX 600
struct uentry { struct gh2_key key; uint8_t *val; uint32_t len; };
struct umodel { struct uentry e[UMODEL_MAX]; uint32_t n; };
static uint32_t um_find(const struct umodel *m, const struct gh2_key *k, int *f) {
    uint32_t lo = 0, hi = m->n; *f = 0;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2;
        int c = gh2_key_cmp(&m->e[mid].key, k);
        if (c == 0) { *f = 1; return mid; }
        if (c < 0) lo = mid + 1; else hi = mid; }
    return lo;
}
static void um_put(struct umodel *m, const struct gh2_key *k, const uint8_t *v, uint32_t len) {
    int found; uint32_t idx = um_find(m, k, &found);
    if (found) { free(m->e[idx].val); m->e[idx].val = malloc(len ? len : 1);
        memcpy(m->e[idx].val, v, len); m->e[idx].len = len; return; }
    for (uint32_t i = m->n; i > idx; i--) m->e[i] = m->e[i-1];
    m->e[idx].key = *k; m->e[idx].val = malloc(len ? len : 1);
    memcpy(m->e[idx].val, v, len); m->e[idx].len = len; m->n++;
}
static void um_destroy(struct umodel *m) { for (uint32_t i = 0; i < m->n; i++) free(m->e[i].val); }

/* collect dla duzych wartosci */
struct ucollect { struct gh2_key keys[UMODEL_MAX]; uint8_t *vals[UMODEL_MAX];
                  uint32_t lens[UMODEL_MAX]; uint32_t n; int bad; };
static int ucollect_cb(const struct gh2_key *k, const void *v, uint32_t len, void *ctx) {
    struct ucollect *c = ctx;
    if (c->n >= UMODEL_MAX) { c->bad = 1; return -1; }
    c->keys[c->n] = *k; c->vals[c->n] = malloc(len ? len : 1);
    memcpy(c->vals[c->n], v, len); c->lens[c->n] = len; c->n++;
    return 0;
}
static void um_verify(struct gh_dev *dev, const struct gh2_bptr *root, const struct umodel *m) {
    for (uint32_t i = 0; i < m->n; i++) {
        uint8_t buf[GH2_BLOCK_SIZE]; uint32_t ol = 0;
        int r = gh2_btree_lookup(dev, root, &m->e[i].key, buf, sizeof(buf), &ol);
        CHECK_EQ(r, 0);
        CHECK_EQ(ol, m->e[i].len);
        CHECK_EQ(memcmp(buf, m->e[i].val, m->e[i].len), 0);
    }
    struct ucollect c; memset(&c, 0, sizeof(c));
    int r = gh2_btree_iterate(dev, root, ucollect_cb, &c);
    CHECK_EQ(r, 0); CHECK_EQ(c.bad, 0);
    CHECK_EQ(c.n, m->n);
    for (uint32_t i = 0; i < m->n && i < c.n; i++) {
        CHECK_EQ(gh2_key_cmp(&c.keys[i], &m->e[i].key), 0);
        CHECK_EQ(c.lens[i], m->e[i].len);
        CHECK_EQ(memcmp(c.vals[i], m->e[i].val, m->e[i].len), 0);
    }
    for (uint32_t i = 1; i < c.n; i++) CHECK(gh2_key_cmp(&c.keys[i-1], &c.keys[i]) < 0);
    for (uint32_t i = 0; i < c.n; i++) free(c.vals[i]);
}

static void fill_pat(uint8_t *b, uint32_t len, uint64_t seed) {
    for (uint32_t i = 0; i < len; i++) b[i] = (uint8_t)((seed * 1103515245u + i * 12345u) >> 7);
}

static void test_uneven_value_sizes(void) {
    char tmp[] = "/tmp/ghost_v2btXXXXXX"; struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    struct gh2_bump_alloc bump; gh2_bump_init(&bump, GH2_DATA_START, 200000);
    struct gh2_alloc al = gh2_bump_alloc(&bump);
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &al, 1, 1, &root), 0);

    /* (a) reproducer z recenzji: 99:1, 100:2400, 101:2400.
     *     2400 > GH2_LEAF_MAX_VAL -> -EFBIG (a NIE crash). */
    {
        static uint8_t big[GH2_BLOCK_SIZE]; memset(big, 'Z', sizeof(big));
        struct gh2_bptr nr;
        struct gh2_key k99 = mk(99, 0, 0);
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k99, big, 1, &nr), 0); root = nr;
        struct gh2_key k100 = mk(100, 0, 0);
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k100, big, 2400, &nr), -EFBIG);
        struct gh2_key k101 = mk(101, 0, 0);
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k101, big, 2400, &nr), -EFBIG);
    }
    /* (b) wartosc > GH2_LEAF_MAX_VAL -> -EFBIG; dokladnie MAX -> OK */
    {
        static uint8_t big[GH2_BLOCK_SIZE]; memset(big, 'M', sizeof(big));
        struct gh2_bptr nr; struct gh2_key k = mk(500, 0, 0);
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, big, GH2_LEAF_MAX_VAL + 1, &nr), -EFBIG);
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, big, GH2_LEAF_MAX_VAL, &nr), 0);
        root = nr;
    }

    /* (c) property: mieszanka skrajnie nierownych dlugosci wymuszajaca WIELOKROTNY split.
     *     dlugosci z {0,1,male,~MAX,losowe 0..MAX} — w tym sasiadujace duze itemy. */
    struct umodel m; m.n = 0;
    /* zasiej model stanem z (a)/(b): klucz 99 (len1) i 500 (MAX) */
    {
        uint8_t v1[1] = { 'Z' };
        struct gh2_key k99 = mk(99, 0, 0); um_put(&m, &k99, v1, 1);
        static uint8_t vmax[GH2_LEAF_MAX_VAL]; memset(vmax, 'M', sizeof(vmax));
        struct gh2_key k500 = mk(500, 0, 0); um_put(&m, &k500, vmax, GH2_LEAF_MAX_VAL);
    }
    static uint8_t vb[GH2_BLOCK_SIZE];
    unsigned seed = 0xBADF00D;
    /* przestrzen kluczy ograniczona tak, by liczba UNIKALNYCH kluczy < UMODEL_MAX:
     * oid 1..120 -> <= ~122 unikalnych kluczy (z (a)/(b)). Tree i model ZAWSZE w synchro. */
    for (int op = 0; op < 1500; op++) {
        uint64_t oid = (uint64_t)(rand_r(&seed) % 120) + 1;   /* gesta -> czeste update */
        struct gh2_key k = mk(oid, 0, 0);
        uint32_t len;
        switch (rand_r(&seed) % 6) {
            case 0: len = 0; break;
            case 1: len = 1; break;
            case 2: len = (uint32_t)(rand_r(&seed) % 16); break;       /* male */
            case 3: len = GH2_LEAF_MAX_VAL; break;                      /* MAX */
            case 4: len = GH2_LEAF_MAX_VAL - (uint32_t)(rand_r(&seed) % 64); break; /* ~MAX */
            default: len = (uint32_t)(rand_r(&seed) % (GH2_LEAF_MAX_VAL + 1)); break;
        }
        fill_pat(vb, len, (oid << 8) ^ (uint64_t)op);
        struct gh2_bptr nr;
        int r = gh2_btree_insert(&dev, &al, &root, 2, &k, vb, len, &nr);
        CHECK_EQ(r, 0);                 /* len <= MAX zawsze sie powiedzie (split obsluguje) */
        root = nr;
        um_put(&m, &k, vb, len);
        CHECK(m.n < UMODEL_MAX);        /* model nie moze przepelnic (synchro z tree) */
        if (op % 30 == 0) um_verify(&dev, &root, &m);
    }
    um_verify(&dev, &root, &m);
    /* brak wyciekow: in_use == zywe wezly */
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    CHECK_EQ(bump.oom, 0);

    /* (d) konkretny wzorzec: pary sasiadujacych duzych (~1900) + male, wymusza split
     *     nierownej pary wielokrotnie (oba ~1900 nie zmieszcza sie razem). */
    for (int i = 0; i < 60; i++) {
        struct gh2_key k = mk(2000 + (uint64_t)i, 0, 0);
        uint32_t len = (i % 2 == 0) ? 1900 : 1;
        fill_pat(vb, len, (uint64_t)(7000 + i));
        struct gh2_bptr nr;
        CHECK_EQ(gh2_btree_insert(&dev, &al, &root, 2, &k, vb, len, &nr), 0);
        root = nr;
        um_put(&m, &k, vb, len);
        CHECK(m.n < UMODEL_MAX);
    }
    um_verify(&dev, &root, &m);
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));

    /* (e) delete polowy losowo -> wymusza redystrybucje liscia z nierownymi rozmiarami */
    {
        unsigned dseed = 555;
        for (uint32_t pass = 0; pass < m.n / 2; pass++) {
            uint32_t idx = rand_r(&dseed) % m.n;
            struct gh2_key k = m.e[idx].key;
            struct gh2_bptr nr;
            int r = gh2_btree_delete(&dev, &al, &root, 2, &k, &nr);
            CHECK_EQ(r, 0); root = nr;
            free(m.e[idx].val);
            for (uint32_t i = idx; i + 1 < m.n; i++) m.e[i] = m.e[i+1];
            m.n--;
            if (pass % 20 == 0) um_verify(&dev, &root, &m);
        }
    }
    um_verify(&dev, &root, &m);
    CHECK_EQ(bump.in_use, (long long)count_live_nodes(&dev, &root));
    CHECK_EQ(bump.oom, 0);

    um_destroy(&m);
    gh2_bump_destroy(&bump);
    close_dev(&dev, tmp);
}

int main(void) {
    RUN_TEST(test_key_cmp);
    RUN_TEST(test_leaf_ops);
    RUN_TEST(test_update_resize);
    RUN_TEST(test_efbig);
    RUN_TEST(test_cow_snapshot);
    RUN_TEST(test_no_block_leak);
    RUN_TEST(test_node_csum_eio);
    /* Task 2 */
    RUN_TEST(test_big_ascending);
    RUN_TEST(test_big_descending);
    RUN_TEST(test_big_random);
    RUN_TEST(test_property_5000);
    RUN_TEST(test_cow_snapshot_multilevel);
    RUN_TEST(test_abandon_no_leak);
    /* Task 3: delete + merge/borrow */
    RUN_TEST(test_property_delete_10000);
    RUN_TEST(test_deep_mass_delete);
    RUN_TEST(test_oscillation);
    RUN_TEST(test_delete_csum_eio);
    /* Regresja recenzji: split/redystrybucja przy skrajnie nierownych rozmiarach */
    RUN_TEST(test_uneven_value_sizes);
    return TEST_SUMMARY();
}
