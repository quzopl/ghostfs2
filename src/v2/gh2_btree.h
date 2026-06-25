#ifndef GH2_BTREE_H
#define GH2_BTREE_H
/* ghostfs v2.1 — generyczne copy-on-write B-drzewo (klucz -> wartosc, zmienna dlugosc).
 * Fundament wszystkich drzew v2. I/O wezlow przez gh_disk_read/write (szyfrowanie+cache,
 * BEZ txn/csum v1); sumy v2 trzymane w gh2_bptr (gh_crc32 plaintextu wezla).
 *
 * Task 1: format wezla + alokator-stub + LISC (insert/lookup/iterate, BEZ split).
 */
#include <stdint.h>
#include "v2/gh2_format.h"   /* gh2_bptr, GH2_BLOCK_SIZE */
#include "block.h"           /* struct gh_dev, gh_disk_read/write */

/* ---- klucz: leksykograficznie (objectid, type, offset) ---- */
struct gh2_key {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
};

/* ---- naglowek wezla (staly, na offsecie 0 bloku) ---- */
struct gh2_node_hdr {
    uint8_t  level;        /* 0 = lisc, >0 = wewnetrzny */
    uint8_t  _pad[3];
    uint32_t nritems;
    uint64_t generation;
    uint64_t owner;        /* objectid drzewa (diagnostyka) */
};

/* ---- lisc (level 0): tablica naglowkow itemow rosnie DO PRZODU, dane OD KONCA ---- */
struct gh2_leaf_item {
    struct gh2_key key;
    uint32_t data_off;     /* offset danych od poczatku bloku */
    uint32_t data_len;
};

/* ---- wezel wewnetrzny (level>0): tablica wskaznikow do dzieci ---- */
struct gh2_internal_ptr {
    struct gh2_key  key;   /* NAJMNIEJSZY klucz w poddrzewie child */
    struct gh2_bptr child;
};

/* Rozmiary musza byc stabilne (determinizm sumy / pojemnosci). Kompilator i tak doda
 * padding po `type` (u8) przed `offset` (u8) — jest deterministyczny i zerowany memset 0. */
_Static_assert(sizeof(struct gh2_key) == 24, "gh2_key musi miec 24 B");
_Static_assert(sizeof(struct gh2_node_hdr) == 24, "gh2_node_hdr musi miec 24 B");
_Static_assert(sizeof(struct gh2_leaf_item) == 32, "gh2_leaf_item musi miec 32 B");
_Static_assert(sizeof(struct gh2_internal_ptr) == 56, "gh2_internal_ptr musi miec 56 B");
_Static_assert(GH2_BLOCK_SIZE == 4096, "blok v2 = 4096 B");

/* przestrzen na zawartosc wezla (za naglowkiem) */
#define GH2_NODE_SPACE   (GH2_BLOCK_SIZE - (uint32_t)sizeof(struct gh2_node_hdr))

/* pojemnosc wezla wewnetrznego (stala) i prog MIN (polowa) */
#define GH2_INT_CAP      (GH2_NODE_SPACE / (uint32_t)sizeof(struct gh2_internal_ptr))
#define GH2_INT_MIN      (GH2_INT_CAP / 2u)

/* lisc: pojemnosc ZMIENNA (zalezna od sumy dlugosci wartosci). Granica: tablica itemow
 * rosnaca do przodu nie moze przeciac danych rosnacych od konca. Prog MIN po bajtach. */
#define GH2_LEAF_MIN     (GH2_NODE_SPACE / 2u)
/* MAKSYMALNA wartosc: ograniczona do polowy przestrzeni wezla (minus naglowek itemu), tak by
 * CALY item (wartosc + naglowek) <= GH2_NODE_SPACE/2. To gwarantuje, ze split liscia metoda
 * "najwiekszy prefiks <= GH2_NODE_SPACE" ZAWSZE daje OBIE polowki <= GH2_NODE_SPACE:
 * lewa > NODE_SPACE - M (M = max item), prawa < 2*M <= NODE_SPACE dla M <= NODE_SPACE/2.
 * Wieksza wartosc -> -EFBIG (duze dane plikow ida do EKSTENTOW, nie do B-drzewa). */
#define GH2_LEAF_MAX_VAL (GH2_NODE_SPACE / 2u - (uint32_t)sizeof(struct gh2_leaf_item))
/* sanity: caly item (wartosc max + naglowek) <= polowa przestrzeni wezla */
_Static_assert(GH2_LEAF_MAX_VAL + sizeof(struct gh2_leaf_item) <= GH2_NODE_SPACE / 2u,
               "max item musi byc <= GH2_NODE_SPACE/2");

/* ---- alokator (pluggable; prawdziwy w v2.2). Stub w v2.1 ---- */
struct gh2_alloc {
    int  (*alloc)(void *ctx, uint64_t *out_block);   /* nowy wolny blok */
    void (*free)(void *ctx, uint64_t block);          /* zwolnij (CoW: stary wezel) */
    void *ctx;
    int   dup_meta;   /* v2.8: 1 = zapisuj wezly w 2 kopiach (DUP metadane); 0 = jak v2.0-2.7 */
};

/* ---- stub alokatora: bump (next_free) + lista wolnych + LICZNIK w-uzyciu (wycieki) ---- */
struct gh2_bump_alloc {
    uint64_t  next_free;     /* nastepny swiezy blok */
    uint64_t  max_block;     /* gorna granica (total_blocks) */
    uint64_t *freelist;      /* recyklowane bloki (z free) */
    uint32_t  nfree, freecap;
    int64_t   in_use;        /* alloc++ / free-- ; po zrownowazonej sekwencji = 0 */
    int       oom;           /* 1 = brak pamieci na freelist (twardy blad) */
};

void gh2_bump_init(struct gh2_bump_alloc *b, uint64_t first_block, uint64_t max_block);
void gh2_bump_destroy(struct gh2_bump_alloc *b);
/* zbuduj struct gh2_alloc owijajaca dany stub */
struct gh2_alloc gh2_bump_alloc(struct gh2_bump_alloc *b);

/* ---- porownanie kluczy: -1/0/1 ---- */
int gh2_key_cmp(const struct gh2_key *a, const struct gh2_key *b);

/* ---- I/O wezlow ---- */
/* odczyt: gh_disk_read + sprawdz gh_crc32==bptr.csum; niezgodnosc -> dup_block (jesli !=0);
 * oba zle -> -EIO. buf musi miec >= GH2_BLOCK_SIZE. */
int gh2_node_read(struct gh_dev *dev, const struct gh2_bptr *bptr, void *buf);
/* zapis: alokuj blok, csum=gh_crc32(buf,4096) plaintextu, gh_disk_write, ustaw out_bptr. */
int gh2_node_write(struct gh_dev *dev, struct gh2_alloc *a, const void *buf,
                   uint64_t gen, struct gh2_bptr *out_bptr);

/* ---- API B-drzewa ---- */
int gh2_btree_create(struct gh_dev *dev, struct gh2_alloc *a, uint64_t owner, uint64_t gen,
                     struct gh2_bptr *out_root);
int gh2_btree_lookup(struct gh_dev *dev, const struct gh2_bptr *root, const struct gh2_key *key,
                     void *buf, uint32_t buflen, uint32_t *out_len);
int gh2_btree_insert(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *root,
                     uint64_t gen, const struct gh2_key *key, const void *val, uint32_t len,
                     struct gh2_bptr *out_root);
int gh2_btree_delete(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *root,
                     uint64_t gen, const struct gh2_key *key, struct gh2_bptr *out_root);
int gh2_btree_iterate(struct gh_dev *dev, const struct gh2_bptr *root,
                      int (*cb)(const struct gh2_key *, const void *, uint32_t, void *),
                      void *ctx);

/* ---- iteracja po przedziale kluczy [min_key, max_key] (in-order) ---- */
/* Odwiedza itemy o kluczu w [min_key, max_key] rosnaco. cb !=0 przerywa i propaguje. */
int gh2_btree_iterate_range(struct gh_dev *dev, const struct gh2_bptr *root,
                            const struct gh2_key *min_key, const struct gh2_key *max_key,
                            int (*cb)(const struct gh2_key *, const void *, uint32_t, void *),
                            void *ctx);

/* ---- mark-sweep: odwiedz KAZDY blok-wezel drzewa (internal+leaf) ---- */
/* DFS od korzenia w dol; woła cb(bptr.block, ctx) dla KAZDEGO wezla. Pomija block==0.
 * cb zwracajace !=0 przerywa i propaguje wartosc. Blad odczytu wezla propagowany. */
int gh2_btree_walk_nodes(struct gh_dev *dev, const struct gh2_bptr *root,
                         int (*cb)(uint64_t block, void *ctx), void *ctx);

#endif
