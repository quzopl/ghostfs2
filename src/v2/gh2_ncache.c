#define _POSIX_C_SOURCE 200809L
#include "v2/gh2_ncache.h"
#include "v2/gh2_format.h"   /* GH2_BLOCK_SIZE */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2 — write-back cache brudnych wezlow. Hash table (open addressing,
 * linear probing). Klucz u64 block; slot block==0 == pusty. Bufory 4096B.
 * ========================================================================== */

#define NCACHE_INIT_SLOTS 64u          /* poczatkowa pojemnosc (potega 2) */

static uint64_t ncache_hash(uint64_t k) {
    /* splitmix64 finalizer — dobre rozproszenie numerow blokow */
    k += 0x9E3779B97F4A7C15ULL;
    k = (k ^ (k >> 30)) * 0xBF58476D1CE4E5B9ULL;
    k = (k ^ (k >> 27)) * 0x94D049BB133111EBULL;
    k = k ^ (k >> 31);
    return k;
}

struct gh2_ncache *gh2_ncache_create(void) {
    struct gh2_ncache *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->slots = calloc((size_t)NCACHE_INIT_SLOTS, sizeof(struct gh2_ncache_ent));
    if (!c->slots) { free(c); return NULL; }
    c->nslots = NCACHE_INIT_SLOTS;
    c->count = 0;
    c->oom = 0;
    return c;
}

void gh2_ncache_clear(struct gh2_ncache *c) {
    if (!c) return;
    for (uint64_t i = 0; i < c->nslots; i++) {
        if (c->slots[i].block != 0) {
            free(c->slots[i].buf);
            c->slots[i].buf = NULL;
            c->slots[i].block = 0;
        }
    }
    c->count = 0;
    c->oom = 0;
}

void gh2_ncache_destroy(struct gh2_ncache *c) {
    if (!c) return;
    gh2_ncache_clear(c);
    free(c->slots);
    free(c);
}

/* znajdz slot dla block: zwraca indeks slotu z tym block, albo pierwszy pusty (block==0).
 * Tablica nigdy nie jest pelna (rehash przy >70%), wiec petla zawsze sie konczy. */
static uint64_t ncache_find_slot(const struct gh2_ncache_ent *slots, uint64_t nslots,
                                 uint64_t block) {
    uint64_t mask = nslots - 1;
    uint64_t i = ncache_hash(block) & mask;
    for (;;) {
        if (slots[i].block == 0 || slots[i].block == block) return i;
        i = (i + 1) & mask;
    }
}

/* powieksz tablice 2x i przehashuj (przy zapelnieniu >70%). 0 lub -ENOMEM. */
static int ncache_grow(struct gh2_ncache *c) {
    uint64_t ncap = c->nslots * 2;
    struct gh2_ncache_ent *ns = calloc((size_t)ncap, sizeof(struct gh2_ncache_ent));
    if (!ns) return -ENOMEM;
    for (uint64_t i = 0; i < c->nslots; i++) {
        if (c->slots[i].block != 0) {
            uint64_t j = ncache_find_slot(ns, ncap, c->slots[i].block);
            ns[j] = c->slots[i];
        }
    }
    free(c->slots);
    c->slots = ns;
    c->nslots = ncap;
    return 0;
}

int gh2_ncache_put(struct gh2_ncache *c, uint64_t block, const void *buf) {
    if (!c || block == 0) return -EINVAL;
    /* utrzymaj wspolczynnik zapelnienia < ~70% (count+1) */
    if ((c->count + 1) * 10 >= c->nslots * 7) {
        if (ncache_grow(c) != 0) { c->oom = 1; return -ENOMEM; }
    }
    uint64_t i = ncache_find_slot(c->slots, c->nslots, block);
    if (c->slots[i].block == block) {
        /* aktualizacja istniejacego bufora (re-CoW tego samego bloku — nie powinno wystapic,
         * ale obslugujemy idempotentnie) */
        memcpy(c->slots[i].buf, buf, GH2_BLOCK_SIZE);
        return 0;
    }
    uint8_t *nb = malloc(GH2_BLOCK_SIZE);
    if (!nb) { c->oom = 1; return -ENOMEM; }
    memcpy(nb, buf, GH2_BLOCK_SIZE);
    c->slots[i].block = block;
    c->slots[i].buf = nb;
    c->count++;
    return 0;
}

int gh2_ncache_has(const struct gh2_ncache *c, uint64_t block) {
    if (!c || block == 0) return 0;
    uint64_t i = ncache_find_slot(c->slots, c->nslots, block);
    return c->slots[i].block == block;
}

int gh2_ncache_get(struct gh2_ncache *c, uint64_t block, void *out) {
    if (!c || block == 0) return 0;
    uint64_t i = ncache_find_slot(c->slots, c->nslots, block);
    if (c->slots[i].block != block) return 0;
    memcpy(out, c->slots[i].buf, GH2_BLOCK_SIZE);
    return 1;
}

/* usun wpis block: zwolnij bufor, potem przehashuj klaster (Robin Hood / backward-shift
 * dla open addressingu — przesun nastepne wpisy, ktore moglyby byc "za" usunietym). */
void gh2_ncache_remove(struct gh2_ncache *c, uint64_t block) {
    if (!c || block == 0) return;
    uint64_t mask = c->nslots - 1;
    uint64_t i = ncache_find_slot(c->slots, c->nslots, block);
    if (c->slots[i].block != block) return;
    free(c->slots[i].buf);
    c->slots[i].buf = NULL;
    c->slots[i].block = 0;
    c->count--;
    /* backward-shift delete: napraw klaster, by lookup pozostalych nie pekl na dziurze */
    uint64_t j = (i + 1) & mask;
    while (c->slots[j].block != 0) {
        uint64_t home = ncache_hash(c->slots[j].block) & mask;
        /* czy slot j moze przesunac sie do i (i lezy w jego przedziale [home..j])? */
        int can_move;
        if (i <= j) can_move = (home <= i || home > j);
        else        can_move = (home <= i && home > j);
        if (can_move) {
            c->slots[i] = c->slots[j];
            c->slots[j].block = 0;
            c->slots[j].buf = NULL;
            i = j;
        }
        j = (j + 1) & mask;
    }
}

uint64_t gh2_ncache_count(const struct gh2_ncache *c) {
    return c ? c->count : 0;
}

int gh2_ncache_foreach(struct gh2_ncache *c,
                       int (*cb)(uint64_t block, const void *buf, void *ctx), void *ctx) {
    if (!c) return 0;
    for (uint64_t i = 0; i < c->nslots; i++) {
        if (c->slots[i].block != 0) {
            int r = cb(c->slots[i].block, c->slots[i].buf, ctx);
            if (r != 0) return r;
        }
    }
    return 0;
}
