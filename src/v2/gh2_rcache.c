#define _POSIX_C_SOURCE 200809L
#include "v2/gh2_rcache.h"
#include "v2/gh2_format.h"   /* GH2_BLOCK_SIZE */
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * ghostfs v2 — read-side node cache (csum-keyed). Tablica o STALYM rozmiarze
 * (GH2_RCACHE_CAP wpisow); indeks hash (open addressing, linear probing) block->slot.
 * Eksmisja: CLOCK (second-chance) — ref-bit ustawiany przy get/put, rece zegara czyszcza.
 *
 * KLUCZOWA WLASNOSC: get(block,csum) zwraca tresc TYLKO gdy zapisany csum == podany csum.
 * Reuzyty blok (CoW) z NOWYM csum -> miss -> re-read. NIGDY nie zwraca starej tresci dla
 * nowego bptr.
 * ========================================================================== */

unsigned long gh2_node_crc_verify_count = 0;   /* test-only (gh2_btree.c inkrementuje) */

#define RC_EMPTY 0u                              /* block==0 => slot wolny (block 0 nigdy wezlem) */
/* indeks hash: 2x pojemnosc, potega 2, dla niskiego load-factora (<=50%). */
#define RC_SLOTS (4096u)                         /* >= 2*GH2_RCACHE_CAP, potega 2 */
_Static_assert(RC_SLOTS >= 2u * GH2_RCACHE_CAP, "RC_SLOTS musi byc >= 2*CAP");

struct rc_ent {
    uint64_t block;       /* RC_EMPTY (0) => wolny */
    uint32_t csum;        /* csum zapisanej (zweryfikowanej) tresci */
    uint8_t  ref;         /* CLOCK second-chance bit */
    uint8_t *buf;         /* 4096B (wlasnosc cache) */
};

struct gh2_rcache {
    struct rc_ent ent[GH2_RCACHE_CAP];  /* sloty zegara (tresc) */
    uint32_t      count;                /* liczba zajetych ent */
    uint32_t      hand;                 /* reka zegara (do eksmisji) */
    /* indeks hash block -> idx do ent[] (idx+1; 0 == pusty slot indeksu) */
    uint32_t      idx[RC_SLOTS];
};

static uint64_t rc_hash(uint64_t k) {
    k += 0x9E3779B97F4A7C15ULL;
    k = (k ^ (k >> 30)) * 0xBF58476D1CE4E5B9ULL;
    k = (k ^ (k >> 27)) * 0x94D049BB133111EBULL;
    k = k ^ (k >> 31);
    return k;
}

/* znajdz w indeksie slot dla `block`: zwraca wskaznik do komorki idx[] z dopasowanym wpisem
 * (idx[]!=0 i ent[idx-1].block==block) lub do pierwszego pustego (idx[]==0). Tablica nigdy
 * pelna (RC_SLOTS >= 2*CAP), wiec petla sie konczy. */
static uint32_t *rc_index_find(struct gh2_rcache *c, uint64_t block) {
    uint64_t mask = RC_SLOTS - 1;
    uint64_t i = rc_hash(block) & mask;
    for (;;) {
        uint32_t e = c->idx[i];
        if (e == 0) return &c->idx[i];
        if (c->ent[e - 1].block == block) return &c->idx[i];
        i = (i + 1) & mask;
    }
}

/* przebuduj CALY indeks hash z tablicy ent[] (po eksmisji, ktora przesuwa wpisy). */
static void rc_index_rebuild(struct gh2_rcache *c) {
    memset(c->idx, 0, sizeof(c->idx));
    for (uint32_t e = 0; e < c->count; e++) {
        if (c->ent[e].block == RC_EMPTY) continue;
        uint32_t *slot = rc_index_find(c, c->ent[e].block);
        *slot = e + 1;
    }
}

struct gh2_rcache *gh2_rcache_create(void) {
    struct gh2_rcache *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    /* bufory alokowane leniwie przy pierwszym uzyciu slotu (mniej rezydentnej pamieci gdy
     * cache nie zapelniony). count=0, idx wyzerowany przez calloc. */
    return c;
}

void gh2_rcache_destroy(struct gh2_rcache *c) {
    if (!c) return;
    for (uint32_t e = 0; e < GH2_RCACHE_CAP; e++) free(c->ent[e].buf);
    free(c);
}

int gh2_rcache_get(struct gh2_rcache *c, uint64_t block, uint32_t csum, void *buf_out) {
    if (!c || block == RC_EMPTY) return 0;
    uint32_t e = *rc_index_find(c, block);
    if (e == 0) return 0;                          /* brak w cache */
    struct rc_ent *ent = &c->ent[e - 1];
    if (ent->csum != csum) return 0;               /* csum-keyed: reuzyty/nieaktualny blok -> MISS */
    memcpy(buf_out, ent->buf, GH2_BLOCK_SIZE);
    ent->ref = 1;                                  /* second-chance */
    return 1;
}

/* wybierz ent[] do (re)uzycia: jesli count < CAP -> nowy slot; inaczej CLOCK: skanuj, czysc
 * ref-bity, eksmituj pierwszy z ref==0. Zwraca indeks ent do nadpisania. */
static uint32_t rc_evict_slot(struct gh2_rcache *c) {
    if (c->count < GH2_RCACHE_CAP) return c->count++;   /* swiezy slot */
    for (;;) {
        struct rc_ent *e = &c->ent[c->hand];
        if (e->ref == 0) {
            uint32_t victim = c->hand;
            c->hand = (c->hand + 1) % GH2_RCACHE_CAP;
            return victim;
        }
        e->ref = 0;
        c->hand = (c->hand + 1) % GH2_RCACHE_CAP;
    }
}

void gh2_rcache_put(struct gh2_rcache *c, uint64_t block, uint32_t csum, const void *buf) {
    if (!c || block == RC_EMPTY) return;
    /* juz w cache? aktualizuj (tresc + csum) na miejscu. */
    uint32_t *slot = rc_index_find(c, block);
    if (*slot != 0) {
        struct rc_ent *e = &c->ent[*slot - 1];
        if (!e->buf) { e->buf = malloc(GH2_BLOCK_SIZE); if (!e->buf) { return; } }
        memcpy(e->buf, buf, GH2_BLOCK_SIZE);
        e->csum = csum;
        e->ref  = 1;
        return;
    }
    /* nowy wpis: wybierz slot ent (moze eksmitowac). */
    int was_full = (c->count >= GH2_RCACHE_CAP);
    uint32_t vi = rc_evict_slot(c);
    struct rc_ent *e = &c->ent[vi];
    if (!e->buf) {
        e->buf = malloc(GH2_BLOCK_SIZE);
        if (!e->buf) {                  /* OOM: porzuc — cache best-effort, brak wpisu */
            if (!was_full && vi + 1 == c->count) c->count--;   /* cofnij swiezy slot */
            else e->block = RC_EMPTY;
            rc_index_rebuild(c);
            return;
        }
    }
    memcpy(e->buf, buf, GH2_BLOCK_SIZE);
    e->block = block;
    e->csum  = csum;
    e->ref   = 1;
    /* eksmisja podmienila block w slocie vi -> indeks nieaktualny dla starego klucza; przebuduj.
     * (Tani: CAP+RC_SLOTS, rzadziej niz odczyty hot wezlow ktore trafiaja w get.) */
    if (was_full) rc_index_rebuild(c);
    else *rc_index_find(c, block) = vi + 1;   /* swiezy slot: tylko dodaj do indeksu */
}

uint64_t gh2_rcache_count(const struct gh2_rcache *c) {
    if (!c) return 0;
    uint64_t n = 0;
    for (uint32_t e = 0; e < GH2_RCACHE_CAP; e++) if (c->ent[e].block != RC_EMPTY) n++;
    return n;
}
