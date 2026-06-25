#define _POSIX_C_SOURCE 200809L
#include "test.h"
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
 * ghostfs v2.2 — testy alokatora: mapa wolnej przestrzeni, transakcyjny
 * deferred-free/abort, mark-sweep spojnosc (mapa z drzewa == rzeczywista),
 * integracja insert/delete + brak double-alloc/wycieku.
 * ========================================================================== */

/* ---- pomocnik: otworz urzadzenie ---- */
static int open_dev(struct gh_dev *dev, char *tmp, uint64_t nblocks) {
    int fd = mkstemp(tmp); if (fd < 0) return -errno; close(fd);
    int r = gh_dev_create(tmp, nblocks, dev); if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev, const char *tmp) {
    gh_bcache_destroy(dev); gh_dev_close(dev); unlink(tmp);
}

static struct gh2_key mk(uint64_t oid) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = oid; k.type = 1; k.offset = 0;
    return k;
}

/* ============================ Test 1: mapa podstawy ============================ */
static void test_space_basic(void) {
    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 100), 0);
    /* bloki 0,1 (SB) zajete; reszta wolna */
    CHECK_EQ(gh2_space_is_used(&s, GH2_SB_SLOT_A), 1);
    CHECK_EQ(gh2_space_is_used(&s, GH2_SB_SLOT_B), 1);
    CHECK_EQ(gh2_space_is_used(&s, GH2_DATA_START), 0);
    CHECK_EQ(gh2_space_is_used(&s, 99), 0);
    /* nfree = 100 - 2 */
    CHECK_EQ(s.nfree, 98);

    /* set/is_used */
    gh2_space_set(&s, 50, 1);
    CHECK_EQ(gh2_space_is_used(&s, 50), 1);
    CHECK_EQ(s.nfree, 97);
    gh2_space_set(&s, 50, 1);          /* idempotentne */
    CHECK_EQ(s.nfree, 97);
    gh2_space_set(&s, 50, 0);
    CHECK_EQ(gh2_space_is_used(&s, 50), 0);
    CHECK_EQ(s.nfree, 98);
    gh2_space_set(&s, 50, 0);          /* idempotentne */
    CHECK_EQ(s.nfree, 98);

    /* alloc_one zwraca tylko wolne; nfree maleje; oznacza zajety */
    uint64_t prev_free = s.nfree;
    for (int i = 0; i < 10; i++) {
        uint64_t b = 0;
        CHECK_EQ(gh2_space_alloc_one(&s, &b), 0);
        CHECK(b >= GH2_DATA_START && b < 100);
        CHECK_EQ(gh2_space_is_used(&s, b), 1);
    }
    CHECK_EQ(s.nfree, prev_free - 10);

    /* wyczerpanie -> -ENOSPC; nfree==0 */
    uint64_t b = 0;
    while (s.nfree > 0) CHECK_EQ(gh2_space_alloc_one(&s, &b), 0);
    CHECK_EQ(s.nfree, 0);
    CHECK_EQ(gh2_space_alloc_one(&s, &b), -ENOSPC);

    gh2_space_destroy(&s);
}

/* ============================ Test 2: deferred-free + abort ============================ */
static void test_deferred_free(void) {
    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 200), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    /* alloc N -> NATYCHMIAST zajete */
    uint64_t alloced[20];
    for (int i = 0; i < 20; i++) {
        CHECK_EQ(a.alloc(a.ctx, &alloced[i]), 0);
        CHECK_EQ(gh2_space_is_used(&s, alloced[i]), 1);
    }
    /* free M (pierwsze 8) -> WCIAZ zajete (deferred) */
    for (int i = 0; i < 8; i++) {
        a.free(a.ctx, alloced[i]);
        CHECK_EQ(gh2_space_is_used(&s, alloced[i]), 1);   /* nadal zajety do commitu */
    }
    /* commit -> teraz wolne */
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    for (int i = 0; i < 8; i++)
        CHECK_EQ(gh2_space_is_used(&s, alloced[i]), 0);
    for (int i = 8; i < 20; i++)
        CHECK_EQ(gh2_space_is_used(&s, alloced[i]), 1);   /* nie zwolnione -> zajete */
    /* listy wyczyszczone */
    CHECK_EQ(t.ndd, 0);
    CHECK_EQ(t.nta, 0);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);

    /* --- scenariusz abort: mapa wraca dokladnie jak przed txn --- */
    CHECK_EQ(gh2_space_init(&s, 200), 0);
    /* snapshot mapy przed txn */
    uint8_t snap[200/8 + 1];
    memcpy(snap, s.bits, (200 + 7) / 8);
    uint64_t free_before = s.nfree;

    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    a = gh2_txn_alloc_vtable(&t);
    uint64_t a2[30];
    for (int i = 0; i < 30; i++) {
        CHECK_EQ(a.alloc(a.ctx, &a2[i]), 0);
        CHECK_EQ(gh2_space_is_used(&s, a2[i]), 1);
    }
    /* takze odlozmy kilka free (porzucane przy abort) */
    a.free(a.ctx, a2[0]);
    a.free(a.ctx, a2[1]);
    /* abort -> wszystkie zaalokowane znow wolne; mapa == przed txn */
    gh2_txn_alloc_abort(&t);
    for (int i = 0; i < 30; i++)
        CHECK_EQ(gh2_space_is_used(&s, a2[i]), 0);
    CHECK_EQ(s.nfree, free_before);
    CHECK_EQ(memcmp(snap, s.bits, (200 + 7) / 8), 0);   /* bit-po-bicie identyczna */
    CHECK_EQ(t.nta, 0);
    CHECK_EQ(t.ndd, 0);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
}

/* ============================ pomocnik: licznik wezlow + zbior blokow ============================ */
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

/* ============================ Test 3: mark-sweep spojnosc ============================ */
static void test_mark_sweep(void) {
    char tmp[] = "/tmp/gh2space_ms_XXXXXX";
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp, 4096), 0);

    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 4096), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    /* zbuduj wielopoziomowe B-drzewo realnym txn-alloc (alloc + commit po mutacji) */
    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &a, 1, 1, &root), 0);
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);

    uint8_t val[64];
    memset(val, 0xAB, sizeof(val));
    for (uint64_t i = 0; i < 400; i++) {
        struct gh2_key k = mk(i);
        struct gh2_bptr nroot;
        int r = gh2_btree_insert(&dev, &a, &root, 2, &k, val, sizeof(val), &nroot);
        CHECK_EQ(r, 0);
        if (r == 0) { root = nroot; CHECK_EQ(gh2_txn_alloc_commit(&t), 0); }
    }

    /* mapa po budowie drzewa (rzeczywista) = s. Zbuduj swieza mape z drzewa. */
    struct gh2_space s2;
    CHECK_EQ(gh2_space_init(&s2, 4096), 0);
    CHECK_EQ(gh2_space_build_from_tree(&dev, &s2, &root), 0);

    /* DOKLADNIE te same bloki zajete: porownaj bit-po-bicie */
    CHECK_EQ(memcmp(s.bits, s2.bits, (4096 + 7) / 8), 0);
    CHECK_EQ(s.nfree, s2.nfree);

    /* dodatkowo: walk_nodes liczy bloki; kazdy unikalny (brak double-alloc) */
    struct blockset bs; memset(&bs, 0, sizeof(bs));
    CHECK_EQ(gh2_btree_walk_nodes(&dev, &root, blockset_cb, &bs), 0);
    CHECK(bs.n > 1);   /* wielopoziomowe -> wiele wezlow */
    for (uint32_t i = 0; i < bs.n; i++)
        for (uint32_t j = i + 1; j < bs.n; j++)
            CHECK(bs.blk[i] != bs.blk[j]);   /* zaden blok nie wspoldzielony */
    /* liczba zywych wezlow == liczba zajetych poza SB (2 sloty) */
    CHECK_EQ((uint64_t)bs.n, s.nblocks - s.nfree - 2);
    free(bs.blk);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
    gh2_space_destroy(&s2);
    close_dev(&dev, tmp);
}

/* ============================ Test 4: integracja insert/delete ============================ */
/* po kazdej mutacji + commit: mapa spojna z drzewem (build_from_tree == biezaca),
 * brak double-alloc, po usunieciu wszystkiego zajete == zywe wezly (brak wyciekow). */
static void check_consistent(struct gh_dev *dev, struct gh2_space *s,
                             const struct gh2_bptr *root) {
    struct gh2_space sb;
    CHECK_EQ(gh2_space_init(&sb, s->nblocks), 0);
    CHECK_EQ(gh2_space_build_from_tree(dev, &sb, root), 0);
    CHECK_EQ(memcmp(s->bits, sb.bits, (s->nblocks + 7) / 8), 0);
    CHECK_EQ(s->nfree, sb.nfree);
    gh2_space_destroy(&sb);
}

static void test_integration(void) {
    char tmp[] = "/tmp/gh2space_int_XXXXXX";
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp, 8192), 0);

    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 8192), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &a, 1, 1, &root), 0);
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);

    uint8_t val[80];
    memset(val, 0x5C, sizeof(val));

    const uint64_t N = 600;
    /* INSERT duzo; commit po kazdej mutacji; mapa spojna */
    for (uint64_t i = 0; i < N; i++) {
        struct gh2_key k = mk(i * 7 + 3);   /* rozproszone klucze */
        struct gh2_bptr nroot;
        CHECK_EQ(gh2_btree_insert(&dev, &a, &root, 2, &k, val, sizeof(val), &nroot), 0);
        root = nroot;
        CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    }
    check_consistent(&dev, &s, &root);

    /* brak double-alloc: zbior wezlow unikalny */
    struct blockset bs; memset(&bs, 0, sizeof(bs));
    CHECK_EQ(gh2_btree_walk_nodes(&dev, &root, blockset_cb, &bs), 0);
    CHECK_EQ((uint64_t)bs.n, s.nblocks - s.nfree - 2);   /* zajete == zywe wezly (+2 SB) */
    free(bs.blk);

    /* DELETE wszystko; commit po kazdej; mapa spojna */
    for (uint64_t i = 0; i < N; i++) {
        struct gh2_key k = mk(i * 7 + 3);
        struct gh2_bptr nroot;
        CHECK_EQ(gh2_btree_delete(&dev, &a, &root, 3, &k, &nroot), 0);
        root = nroot;
        CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    }
    check_consistent(&dev, &s, &root);

    /* po usunieciu wszystkiego: zostal sam korzen (pusty lisc). Zajete == zywe wezly. */
    struct blockset bs2; memset(&bs2, 0, sizeof(bs2));
    CHECK_EQ(gh2_btree_walk_nodes(&dev, &root, blockset_cb, &bs2), 0);
    CHECK_EQ(bs2.n, 1);   /* tylko korzen-lisc */
    /* zajete bloki = 2 (SB) + 1 (korzen) -> nfree == nblocks - 3 (brak wyciekow) */
    CHECK_EQ(s.nfree, s.nblocks - 3);
    free(bs2.blk);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
    close_dev(&dev, tmp);
}

/* ============================ Test 5: refcount get/inc/dec ============================ */
static void test_refcount_basic(void) {
    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 100), 0);

    /* SB (0,1) maja refcount 1; reszta 0 */
    CHECK_EQ(gh2_ref_get(&s, GH2_SB_SLOT_A), 1);
    CHECK_EQ(gh2_ref_get(&s, GH2_SB_SLOT_B), 1);
    CHECK_EQ(gh2_ref_get(&s, 50), 0);
    CHECK_EQ(gh2_space_is_used(&s, 50), 0);

    /* inc: 0->1 oznacza zajety; used <=> rc>0 */
    gh2_ref_inc(&s, 50);
    CHECK_EQ(gh2_ref_get(&s, 50), 1);
    CHECK_EQ(gh2_space_is_used(&s, 50), 1);
    uint64_t free_after_inc = s.nfree;

    /* dalsze inc-y: rc rosnie, nadal 1 blok zajety (nfree bez zmian) */
    gh2_ref_inc(&s, 50);
    gh2_ref_inc(&s, 50);
    CHECK_EQ(gh2_ref_get(&s, 50), 3);
    CHECK_EQ(s.nfree, free_after_inc);
    CHECK_EQ(gh2_space_is_used(&s, 50), 1);

    /* dec: rc maleje; NIE zwalnia dopoki rc>0 */
    gh2_ref_dec(&s, 50);
    CHECK_EQ(gh2_ref_get(&s, 50), 2);
    CHECK_EQ(gh2_space_is_used(&s, 50), 1);
    gh2_ref_dec(&s, 50);
    CHECK_EQ(gh2_ref_get(&s, 50), 1);
    CHECK_EQ(gh2_space_is_used(&s, 50), 1);

    /* ostatni dec: rc 1->0 -> zwolnij (used->0, nfree wraca) */
    gh2_ref_dec(&s, 50);
    CHECK_EQ(gh2_ref_get(&s, 50), 0);
    CHECK_EQ(gh2_space_is_used(&s, 50), 0);
    CHECK_EQ(s.nfree, free_after_inc + 1);

    /* dec na rc==0 -> bez efektu */
    gh2_ref_dec(&s, 50);
    CHECK_EQ(gh2_ref_get(&s, 50), 0);
    CHECK_EQ(gh2_space_is_used(&s, 50), 0);

    /* gh2_space_set(0) twardo zeruje refcount niezaleznie od licznika */
    gh2_ref_inc(&s, 60); gh2_ref_inc(&s, 60);
    CHECK_EQ(gh2_ref_get(&s, 60), 2);
    gh2_space_set(&s, 60, 0);
    CHECK_EQ(gh2_ref_get(&s, 60), 0);
    CHECK_EQ(gh2_space_is_used(&s, 60), 0);

    gh2_space_destroy(&s);
}

/* ============================ Test 6: alloc daje rc1; free->dec przy commit ============================ */
static void test_alloc_dec_commit(void) {
    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 200), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    /* alloc -> blok z refcount DOKLADNIE 1 */
    uint64_t b[10];
    for (int i = 0; i < 10; i++) {
        CHECK_EQ(a.alloc(a.ctx, &b[i]), 0);
        CHECK_EQ(gh2_ref_get(&s, b[i]), 1);
        CHECK_EQ(gh2_space_is_used(&s, b[i]), 1);
    }

    /* free vtable = DEFER dec: blok WCIAZ rc 1 (zajety) do commitu */
    for (int i = 0; i < 4; i++) {
        a.free(a.ctx, b[i]);
        CHECK_EQ(gh2_ref_get(&s, b[i]), 1);
        CHECK_EQ(gh2_space_is_used(&s, b[i]), 1);
    }
    /* commit: dec 1->0 -> zwolnij DOKLADNIE te 4 */
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(gh2_ref_get(&s, b[i]), 0);
        CHECK_EQ(gh2_space_is_used(&s, b[i]), 0);
    }
    for (int i = 4; i < 10; i++)
        CHECK_EQ(gh2_space_is_used(&s, b[i]), 1);
    CHECK_EQ(t.ndd, 0);
    CHECK_EQ(t.ndi, 0);
    CHECK_EQ(t.nta, 0);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
}

/* ============================ Test 7: wspoldzielenie — dec zwalnia DOPIERO przy ostatnim ============================ */
/* symuluj snapshot recznie: ref_inc bloku (rc 1->2). free->dec przy commit obniza 2->1 (NIE zwalnia).
 * dopiero kolejny dec 1->0 zwalnia. BRAMKA: free zwalnia DOKLADNIE przy rc 0, nie wczesniej. */
static void test_shared_dec(void) {
    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 200), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    uint64_t blk;
    CHECK_EQ(a.alloc(a.ctx, &blk), 0);
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    CHECK_EQ(gh2_ref_get(&s, blk), 1);

    /* "snapshot": rc 1->2 (drugi wlasciciel) */
    gh2_ref_inc(&s, blk);
    CHECK_EQ(gh2_ref_get(&s, blk), 2);

    /* free (oryginal CoW-uje) -> defer dec; commit: 2->1 NIE zwalnia (snapshot trzyma) */
    a.free(a.ctx, blk);
    CHECK_EQ(gh2_ref_get(&s, blk), 2);          /* przed commit niezmienione */
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    CHECK_EQ(gh2_ref_get(&s, blk), 1);
    CHECK_EQ(gh2_space_is_used(&s, blk), 1);     /* WCIAZ zajety (rc>0) */

    /* drugi free (snapshot usuniety) -> commit: 1->0 -> teraz zwalnia */
    a.free(a.ctx, blk);
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    CHECK_EQ(gh2_ref_get(&s, blk), 0);
    CHECK_EQ(gh2_space_is_used(&s, blk), 0);     /* zwolniony DOKLADNIE przy ostatnim dec */

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
}

/* ============================ Test 8: defer_inc + savepoint/rollback refcount-deltas ============================ */
static void test_defer_inc_rollback(void) {
    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 200), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    /* przygotuj dwa wspoldzielone bloki (rc 1) */
    uint64_t x, y;
    CHECK_EQ(a.alloc(a.ctx, &x), 0);
    CHECK_EQ(a.alloc(a.ctx, &y), 0);
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    CHECK_EQ(gh2_ref_get(&s, x), 1);
    CHECK_EQ(gh2_ref_get(&s, y), 1);

    /* defer_inc: stosowane przy commit (inc PRZED dec) */
    gh2_txn_alloc_defer_inc(&t, x);
    gh2_txn_alloc_defer_inc(&t, y);
    CHECK_EQ(t.ndi, 2);
    CHECK_EQ(gh2_ref_get(&s, x), 1);   /* jeszcze nie zastosowane */
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    CHECK_EQ(gh2_ref_get(&s, x), 2);
    CHECK_EQ(gh2_ref_get(&s, y), 2);

    /* savepoint/rollback obejmuje defer_inc + defer_dec + txn_alloced */
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&t);
    uint64_t z;
    CHECK_EQ(a.alloc(a.ctx, &z), 0);          /* nowy blok rc 1 */
    CHECK_EQ(gh2_ref_get(&s, z), 1);
    a.free(a.ctx, x);                          /* defer dec */
    gh2_txn_alloc_defer_inc(&t, y);            /* defer inc */
    CHECK(t.nta > sp.nta);
    CHECK(t.ndd > sp.ndd);
    CHECK(t.ndi > sp.ndi);

    /* rollback: cofnij alloc z (rc->0), porzuc defer dec x / defer inc y */
    gh2_txn_alloc_rollback(&t, sp);
    CHECK_EQ(gh2_ref_get(&s, z), 0);          /* nowy blok zwolniony */
    CHECK_EQ(gh2_space_is_used(&s, z), 0);
    CHECK_EQ(t.nta, sp.nta);
    CHECK_EQ(t.ndd, sp.ndd);
    CHECK_EQ(t.ndi, sp.ndi);

    /* commit po rollbacku: NIE dotyka x ani y (porzucone) */
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    CHECK_EQ(gh2_ref_get(&s, x), 2);          /* nietkniety */
    CHECK_EQ(gh2_ref_get(&s, y), 2);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
}

/* ============================ Test 9: refmap mark-sweep liczy poprawnie ============================ */
/* zbuduj realne B-drzewo (txn-alloc); refmap_build_from_roots -> KAZDY zywy blok rc==1
 * (1 korzen), used == mark-sweep (bit-po-bicie z build_from_tree). Regresja: bez wspoldzielenia
 * in_use == zywe wezly. */
static void test_refmap_count(void) {
    char tmp[] = "/tmp/gh2space_rm_XXXXXX";
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp, 4096), 0);

    struct gh2_space s;
    CHECK_EQ(gh2_space_init(&s, 4096), 0);
    struct gh2_txn_alloc t;
    CHECK_EQ(gh2_txn_alloc_init(&t, &s), 0);
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    struct gh2_bptr root;
    CHECK_EQ(gh2_btree_create(&dev, &a, 1, 1, &root), 0);
    CHECK_EQ(gh2_txn_alloc_commit(&t), 0);

    uint8_t val[64];
    memset(val, 0xAB, sizeof(val));
    for (uint64_t i = 0; i < 400; i++) {
        struct gh2_key k = mk(i);
        struct gh2_bptr nroot;
        CHECK_EQ(gh2_btree_insert(&dev, &a, &root, 2, &k, val, sizeof(val), &nroot), 0);
        root = nroot;
        CHECK_EQ(gh2_txn_alloc_commit(&t), 0);
    }

    /* zbuduj refmap z JEDNEGO korzenia */
    struct gh2_space rm;
    CHECK_EQ(gh2_space_init(&rm, 4096), 0);
    CHECK_EQ(gh2_refmap_build_from_roots(&dev, &rm, &root), 0);

    /* used == mark-sweep (build_from_tree): bit-po-bicie + nfree */
    struct gh2_space ms;
    CHECK_EQ(gh2_space_init(&ms, 4096), 0);
    CHECK_EQ(gh2_space_build_from_tree(&dev, &ms, &root), 0);
    CHECK_EQ(memcmp(rm.bits, ms.bits, (4096 + 7) / 8), 0);
    CHECK_EQ(rm.nfree, ms.nfree);

    /* used == biezaca mapa po budowie (s) */
    CHECK_EQ(memcmp(rm.bits, s.bits, (4096 + 7) / 8), 0);
    CHECK_EQ(rm.nfree, s.nfree);

    /* KAZDY zywy blok ma rc DOKLADNIE 1 (1 korzen, brak wspoldzielenia) */
    uint64_t live = 0;
    for (uint64_t b = 0; b < rm.nblocks; b++) {
        if (gh2_space_is_used(&rm, b)) {
            CHECK_EQ(gh2_ref_get(&rm, b), 1);
            live++;
        } else {
            CHECK_EQ(gh2_ref_get(&rm, b), 0);
        }
    }
    /* zywe == zajete w biezacej mapie */
    CHECK_EQ(live, rm.nblocks - rm.nfree);

    /* regresja: zywe wezly (walk) + 2 SB == zajete (bez ekstentow danych w tym drzewie) */
    struct blockset bs; memset(&bs, 0, sizeof(bs));
    CHECK_EQ(gh2_btree_walk_nodes(&dev, &root, blockset_cb, &bs), 0);
    CHECK_EQ((uint64_t)bs.n, rm.nblocks - rm.nfree - 2);
    free(bs.blk);

    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&s);
    gh2_space_destroy(&rm);
    gh2_space_destroy(&ms);
    close_dev(&dev, tmp);
}

int main(void) {
    RUN_TEST(test_space_basic);
    RUN_TEST(test_deferred_free);
    RUN_TEST(test_mark_sweep);
    RUN_TEST(test_integration);
    RUN_TEST(test_refcount_basic);
    RUN_TEST(test_alloc_dec_commit);
    RUN_TEST(test_shared_dec);
    RUN_TEST(test_defer_inc_rollback);
    RUN_TEST(test_refmap_count);
    return TEST_SUMMARY();
}
