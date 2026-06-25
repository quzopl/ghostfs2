#ifndef GH2_NCACHE_H
#define GH2_NCACHE_H
/* ghostfs v2 — write-back cache brudnych wezlow B-drzewa (technika CoW-FS jak btrfs).
 *
 * PROBLEM: gh2_node_write byl write-through (kazda mutacja CoW -> gh_disk_write wezla
 * NATYCHMIAST). Losowy zapis 4K przepisywal korzen drzewa przy KAZDEJ operacji.
 *
 * ROZWIAZANIE: brudne wezly trzymane w PAMIECI (mapa block->bufor 4096B); zapis na dysk
 * dopiero przy commicie (flush). Wezly posrednie (CoW nadpisane przed commitem) NIGDY nie
 * trafiaja na dysk — ich bloki sa reuzywane w tej samej transakcji (immediate-free).
 *
 * Per-montaz (na gh_dev->v2_ncache). NULL gdy nieuzywane (v1 / format bez cache) -> I/O
 * wezlow zachowuje sie jak write-through. Crash-consistency NIEZMIENIONA: flush przed
 * podmiana superbloku, superblok pisany OSTATNI.
 *
 * Hash table z open addressingiem (linear probing). Klucz u64 block (block!=0). */
#include <stdint.h>

struct gh2_ncache_ent {
    uint64_t block;          /* 0 = pusty slot; inaczej numer bloku (klucz) */
    uint8_t *buf;            /* bufor 4096B (wlasnosc cache) */
};

struct gh2_ncache {
    struct gh2_ncache_ent *slots;
    uint64_t nslots;         /* potega 2 */
    uint64_t count;          /* liczba zajetych slotow (brudnych wezlow) */
    int      oom;            /* 1 = brak pamieci przy wstawianiu (twardy blad) */
};

/* utworz cache (alloc). Zwraca wskaznik lub NULL przy OOM. */
struct gh2_ncache *gh2_ncache_create(void);
/* zwolnij WSZYSTKIE bufory + strukture. */
void gh2_ncache_destroy(struct gh2_ncache *c);

/* wstaw/zaktualizuj wpis block -> kopia buf (4096B). Zwraca 0 lub -ENOMEM. */
int gh2_ncache_put(struct gh2_ncache *c, uint64_t block, const void *buf);
/* jesli block w cache -> skopiuj 4096B do out i zwroc 1; inaczej 0. */
int gh2_ncache_get(struct gh2_ncache *c, uint64_t block, void *out);
/* czy block jest w cache (bez kopiowania). */
int gh2_ncache_has(const struct gh2_ncache *c, uint64_t block);
/* usun wpis (zwolnij bufor). Bez efektu gdy nieobecny. */
void gh2_ncache_remove(struct gh2_ncache *c, uint64_t block);
/* liczba brudnych wezlow. */
uint64_t gh2_ncache_count(const struct gh2_ncache *c);

/* iteruj po WSZYSTKICH wpisach (block, buf 4096B). cb !=0 przerywa i propaguje. */
int gh2_ncache_foreach(struct gh2_ncache *c,
                       int (*cb)(uint64_t block, const void *buf, void *ctx), void *ctx);
/* oproznij cache (zwolnij bufory, count=0); struktura zostaje gotowa do dalszego uzycia. */
void gh2_ncache_clear(struct gh2_ncache *c);

#endif
