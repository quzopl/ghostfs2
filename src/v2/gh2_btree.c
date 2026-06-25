#define _POSIX_C_SOURCE 200809L
#include "v2/gh2_btree.h"
#include "v2/gh2_ncache.h"
#include "csum.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2.1 — CoW B-drzewo.
 * Task 1: format + alokator-stub + LISC (insert/lookup/iterate).
 * Task 2: SPLIT liscia/wezla wewnetrznego + wezly WEWNETRZNE (wielopoziomowe
 *         insert/lookup/iterate, nowy korzen). Delete = Task 3 (-ENOSYS).
 * ========================================================================== */

/* ---- porownanie kluczy: leksykograficznie (objectid, type, offset) ---- */
int gh2_key_cmp(const struct gh2_key *a, const struct gh2_key *b) {
    if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
    if (a->type     != b->type)     return a->type     < b->type     ? -1 : 1;
    if (a->offset   != b->offset)   return a->offset   < b->offset   ? -1 : 1;
    return 0;
}

/* ============================ alokator-stub ============================ */

void gh2_bump_init(struct gh2_bump_alloc *b, uint64_t first_block, uint64_t max_block) {
    b->next_free = first_block;
    b->max_block = max_block;
    b->freelist  = NULL;
    b->nfree = 0; b->freecap = 0;
    b->in_use = 0;
    b->oom = 0;
}

void gh2_bump_destroy(struct gh2_bump_alloc *b) {
    free(b->freelist);
    b->freelist = NULL; b->nfree = 0; b->freecap = 0;
}

static int bump_alloc(void *ctx, uint64_t *out_block) {
    struct gh2_bump_alloc *b = ctx;
    uint64_t blk;
    if (b->nfree > 0) {
        blk = b->freelist[--b->nfree];          /* recyklon wolny blok (LIFO) */
    } else {
        if (b->next_free >= b->max_block) return -ENOSPC;
        blk = b->next_free++;
    }
    b->in_use++;
    *out_block = blk;
    return 0;
}

static void bump_free(void *ctx, uint64_t block) {
    struct gh2_bump_alloc *b = ctx;
    if (block == 0) return;                      /* 0 = brak bloku */
    if (b->nfree == b->freecap) {
        uint32_t ncap = b->freecap ? b->freecap * 2 : 16;
        uint64_t *nl = realloc(b->freelist, (size_t)ncap * sizeof(uint64_t));
        if (!nl) { b->oom = 1; b->in_use--; return; }  /* nie mozemy zapamietac — gubimy blok,
                                                          ale licznik dekrementujemy + flaga oom */
        b->freelist = nl; b->freecap = ncap;
    }
    b->freelist[b->nfree++] = block;
    b->in_use--;
}

struct gh2_alloc gh2_bump_alloc(struct gh2_bump_alloc *b) {
    struct gh2_alloc a;
    a.alloc = bump_alloc;
    a.free  = bump_free;
    a.ctx   = b;
    a.dup_meta = 0;        /* bump-alloc (v2.1 stub/testy) bez DUP */
    return a;
}

/* ============================ I/O wezlow ============================ */

/* fwd-decl akcesorow (definicje nizej) — uzywane przez walidacje wezla */
static const struct gh2_node_hdr *node_hdr_c(const void *buf);
static const struct gh2_leaf_item *leaf_items_c(const void *buf);

/* BACKSTOP: zwaliduj strukture wezla (po sprawdzeniu csum). Broni przed bugiem ZAPISU
 * (nie tylko korupcja dysku): nritems w granicach pojemnosci, a dla liscia kazdy
 * [data_off, data_off+data_len) w obszarze danych. Naruszenie -> -EIO (nie cichy OOB). */
static int gh2_node_validate(const void *buf) {
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    uint32_t nr = h->nritems;
    if (h->level == 0) {
        if (nr > GH2_NODE_SPACE / (uint32_t)sizeof(struct gh2_leaf_item)) return -EIO;
        /* dolna granica obszaru danych: za tablica naglowkow itemow */
        uint32_t data_lo = (uint32_t)sizeof(struct gh2_node_hdr)
                           + nr * (uint32_t)sizeof(struct gh2_leaf_item);
        const struct gh2_leaf_item *it = leaf_items_c(buf);
        for (uint32_t i = 0; i < nr; i++) {
            uint32_t off = it[i].data_off, len = it[i].data_len;
            if (len > GH2_BLOCK_SIZE) return -EIO;          /* przepelnienie sumy off+len */
            if (off < data_lo || off > GH2_BLOCK_SIZE) return -EIO;
            if (off + len > GH2_BLOCK_SIZE) return -EIO;
        }
    } else {
        if (nr > GH2_INT_CAP) return -EIO;
    }
    return 0;
}

int gh2_node_read(struct gh_dev *dev, const struct gh2_bptr *bptr, void *buf) {
    if (bptr->block == 0) return -EINVAL;
    struct gh2_ncache *nc = dev->v2_ncache;
    /* WRITE-BACK: brudny wezel tej transakcji jest w cache (jeszcze nie na dysku).
     * READ-YOUR-WRITES: czytaj z pamieci. Walidacja struktury jak dla odczytu z dysku
     * (csum pomijamy — bufor w cache jest plaintextem zapisanym przez gh2_node_write). */
    if (nc) {
        if (gh2_ncache_get(nc, bptr->block, buf)) return gh2_node_validate(buf);
        if (bptr->dup_block != 0 && gh2_ncache_get(nc, bptr->dup_block, buf))
            return gh2_node_validate(buf);
    }
    int r = gh_disk_read(dev, bptr->block, buf);
    if (r == 0 && gh_crc32(buf, GH2_BLOCK_SIZE) == bptr->csum)
        return gh2_node_validate(buf);
    /* niezgodnosc lub blad I/O -> sprobuj duplikatu (jesli istnieje) */
    if (bptr->dup_block != 0) {
        r = gh_disk_read(dev, bptr->dup_block, buf);
        if (r == 0 && gh_crc32(buf, GH2_BLOCK_SIZE) == bptr->csum) {
            int vr = gh2_node_validate(buf);
            if (vr == 0) {
                /* READ-REPAIR (v2.8, best-effort): kopia `block` byla zla, `dup_block` dobra.
                 * Przepisz dobra tresc do zepsutej kopii (naprawa bitrotu). Ignoruj blad zapisu
                 * — odczyt i tak zwraca poprawne dane z dup. Przepisujemy TE SAMA tresc (csum
                 * niezmienione), wiec snapshot-share nienaruszony (to naprawa, nie CoW). */
                (void)gh_disk_write(dev, bptr->block, buf);
            }
            return vr;
        }
    }
    return -EIO;
}

int gh2_node_write(struct gh_dev *dev, struct gh2_alloc *a, const void *buf,
                   uint64_t gen, struct gh2_bptr *out_bptr) {
    struct gh2_ncache *nc = dev->v2_ncache;
    uint64_t blk;
    int r = a->alloc(a->ctx, &blk);
    if (r) return r;
    /* WRITE-BACK: wstaw bufor (kopia 4096B) do cache zamiast gh_disk_write. Flush -> commit. */
    if (nc) r = gh2_ncache_put(nc, blk, buf);
    else    r = gh_disk_write(dev, blk, buf);
    if (r) { a->free(a->ctx, blk); return r; }
    uint64_t blk2 = 0;
    if (a->dup_meta) {
        /* DUP metadane (v2.8): zapisz druga, identyczna kopie wezla. Blad alokacji/zapisu
         * 2. kopii -> zwolnij OBA bloki i zwroc blad (atomowo: brak polowicznego wezla). */
        r = a->alloc(a->ctx, &blk2);
        if (r) { a->free(a->ctx, blk); return r; }
        if (nc) r = gh2_ncache_put(nc, blk2, buf);
        else    r = gh_disk_write(dev, blk2, buf);
        if (r) { a->free(a->ctx, blk2); a->free(a->ctx, blk); return r; }
    }
    memset(out_bptr, 0, sizeof(*out_bptr));
    out_bptr->block = blk;
    out_bptr->dup_block = blk2;                       /* DUP v2.8 (0 gdy dup_meta=0) */
    out_bptr->csum = gh_crc32(buf, GH2_BLOCK_SIZE);   /* plaintext (ta sama suma dla obu kopii) */
    out_bptr->generation = gen;
    return 0;
}

/* ============================ pomocnicy liscia ============================ */

static struct gh2_node_hdr *node_hdr(void *buf) {
    return (struct gh2_node_hdr *)buf;
}
static const struct gh2_node_hdr *node_hdr_c(const void *buf) {
    return (const struct gh2_node_hdr *)buf;
}
/* tablica itemow zaraz za naglowkiem (rosnie do przodu) */
static struct gh2_leaf_item *leaf_items(void *buf) {
    return (struct gh2_leaf_item *)((uint8_t *)buf + sizeof(struct gh2_node_hdr));
}
static const struct gh2_leaf_item *leaf_items_c(const void *buf) {
    return (const struct gh2_leaf_item *)((const uint8_t *)buf + sizeof(struct gh2_node_hdr));
}

/* znajdz pozycje klucza w lisciu (binarne). Zwraca indeks; *found=1 jesli rowny. */
static uint32_t leaf_find(const void *buf, const struct gh2_key *key, int *found) {
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    const struct gh2_leaf_item *it = leaf_items_c(buf);
    uint32_t lo = 0, hi = h->nritems;
    *found = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = gh2_key_cmp(&it[mid].key, key);
        if (c == 0) { *found = 1; return mid; }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;   /* pozycja wstawienia (pierwszy item > key) */
}

/* ============================ pomocnicy wezla wewnetrznego ============================ */

static struct gh2_internal_ptr *int_ptrs(void *buf) {
    return (struct gh2_internal_ptr *)((uint8_t *)buf + sizeof(struct gh2_node_hdr));
}
static const struct gh2_internal_ptr *int_ptrs_c(const void *buf) {
    return (const struct gh2_internal_ptr *)((const uint8_t *)buf + sizeof(struct gh2_node_hdr));
}

/* w wezle wewnetrznym: najwieksze i z key[i] <= szukany. Zwraca indeks dziecka do zejscia.
 * Klucze sa rosnace; key[0] = najmniejszy klucz calego poddrzewa, wiec dla kluczy < key[0]
 * (moga wystapic przy insert mniejszego niz dotychczasowe minimum) zwracamy 0. */
static uint32_t internal_child_idx(const void *buf, const struct gh2_key *key) {
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    const struct gh2_internal_ptr *p = int_ptrs_c(buf);
    /* binarne: znajdz pierwszy i z key[i] > szukany; dziecko = i-1 (lub 0). */
    uint32_t lo = 0, hi = h->nritems;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = gh2_key_cmp(&p[mid].key, key);
        if (c <= 0) lo = mid + 1;   /* key[mid] <= szukany -> szukaj dalej w prawo */
        else hi = mid;
    }
    /* lo = pierwszy indeks z key[lo] > szukany */
    return lo == 0 ? 0 : lo - 1;
}

/* ============================ create ============================ */

int gh2_btree_create(struct gh_dev *dev, struct gh2_alloc *a, uint64_t owner, uint64_t gen,
                     struct gh2_bptr *out_root) {
    uint8_t buf[GH2_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));            /* determinizm sumy: padding = 0 */
    struct gh2_node_hdr *h = node_hdr(buf);
    h->level = 0;
    h->nritems = 0;
    h->generation = gen;
    h->owner = owner;
    return gh2_node_write(dev, a, buf, gen, out_root);
}

/* ============================ lookup ============================ */

int gh2_btree_lookup(struct gh_dev *dev, const struct gh2_bptr *root, const struct gh2_key *key,
                     void *buf_out, uint32_t buflen, uint32_t *out_len) {
    struct gh2_bptr cur = *root;
    uint8_t buf[GH2_BLOCK_SIZE];
    for (;;) {
        int r = gh2_node_read(dev, &cur, buf);
        if (r) return r;
        const struct gh2_node_hdr *h = node_hdr_c(buf);
        if (h->level == 0) {
            int found = 0;
            uint32_t idx = leaf_find(buf, key, &found);
            if (!found) return -ENOENT;
            const struct gh2_leaf_item *it = leaf_items_c(buf);
            uint32_t len = it[idx].data_len;
            if (out_len) *out_len = len;
            if (buf_out) {
                uint32_t n = len < buflen ? len : buflen;
                memcpy(buf_out, (const uint8_t *)buf + it[idx].data_off, n);
            }
            return 0;
        }
        /* wezel wewnetrzny: zejdz do wlasciwego dziecka */
        if (h->nritems == 0) return -ENOENT;          /* nie powinno wystapic */
        uint32_t ci = internal_child_idx(buf, key);
        cur = int_ptrs_c(buf)[ci].child;
    }
}

/* ============================ insert / update (rekurencyjny CoW) ============================ */

/* Wynik rekurencyjnego insertu w poddrzewie:
 *   split == 0: poddrzewo zmiescilo wstawienie. `node` = nowy bptr (CoW) tego poddrzewa.
 *   split == 1: poddrzewo sie podzielilo. `node` = lewy (nowy) bptr; `right` = prawy nowy
 *               bptr; `up_key` = NAJMNIEJSZY klucz prawego poddrzewa do promocji w rodzicu. */
struct ins_res {
    int            split;
    struct gh2_bptr node;     /* lewy / jedyny */
    struct gh2_bptr right;    /* prawy (tylko gdy split) */
    struct gh2_key  up_key;   /* klucz promowany (tylko gdy split) */
};

/* zwolnij CoW stary blok (i ewentualny duplikat) */
static void cow_free(struct gh2_alloc *a, const struct gh2_bptr *old) {
    a->free(a->ctx, old->block);
    if (old->dup_block) a->free(a->ctx, old->dup_block);
}

/* Zbuduj lisc z N itemow podanych jako pary (key, src_ptr, len) i zapisz. Zaklada ze itemy
 * sa juz POSORTOWANE i ze suma sie miesci w bloku. */
struct leaf_item_src { struct gh2_key key; const uint8_t *data; uint32_t len; };

static int leaf_build_write(struct gh_dev *dev, struct gh2_alloc *a, uint64_t gen, uint64_t owner,
                            const struct leaf_item_src *items, uint32_t n,
                            struct gh2_bptr *out) {
    uint8_t nb[GH2_BLOCK_SIZE];
    memset(nb, 0, sizeof(nb));
    struct gh2_node_hdr *nh = node_hdr(nb);
    nh->level = 0; nh->nritems = n; nh->generation = gen; nh->owner = owner;
    struct gh2_leaf_item *nit = leaf_items(nb);
    /* BACKSTOP: zwaliduj ze WSZYSTKIE itemy mieszcza sie ZANIM zaczniemy zapis (bez tego
     * niedoszacowany split robilby `cur -= dlen` underflow -> memcpy OOB). Sprawdzamy ze
     * tablica naglowkow (rosnaca do przodu) NIE przetnie danych (rosnacych od konca). */
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) total += items[i].len;
    uint32_t need = (uint32_t)sizeof(struct gh2_node_hdr)
                    + n * (uint32_t)sizeof(struct gh2_leaf_item) + total;
    if (need > GH2_BLOCK_SIZE) return -EINVAL;   /* bug wywolujacego, nie cichy OOB */
    uint32_t cur = GH2_BLOCK_SIZE;
    for (uint32_t i = 0; i < n; i++) {
        nit[i].key = items[i].key;
        uint32_t dlen = items[i].len;
        cur -= dlen;
        nit[i].data_off = cur;
        nit[i].data_len = dlen;
        if (dlen) memcpy(nb + cur, items[i].data, dlen);
    }
    return gh2_node_write(dev, a, nb, gen, out);
}

/* Wybierz punkt podzialu liscia: NAJWIEKSZY prefiks itemow [0..sp), ktorego sumaryczny
 * rozmiar (dane + naglowki itemow) <= GH2_NODE_SPACE, ZOSTAWIAJAC >=1 item po prawej i
 * gwarantujac >=1 item po lewej. Dzieki ograniczeniu GH2_LEAF_MAX_VAL (kazdy item <=
 * GH2_NODE_SPACE/2) prawa strona [sp..n) ma rozmiar < 2*max_item <= GH2_NODE_SPACE, wiec
 * OBIE polowki mieszcza sie w wezle. Zaklada n >= 2 oraz ze pojedynczy item zawsze sie miesci
 * (gwarantowane przez limit wartosci). */
static uint32_t leaf_split_point(const struct leaf_item_src *items, uint32_t n) {
    uint32_t acc = 0, sp = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t isz = (uint32_t)sizeof(struct gh2_leaf_item) + items[i].len;
        /* prefiks [0..i] musi sie miescic ORAZ zostawic >=1 item po prawej (i+1 < n) */
        if (i + 1 < n && acc + isz <= GH2_NODE_SPACE) {
            acc += isz;
            sp = i + 1;
        } else {
            break;
        }
    }
    if (sp == 0) sp = 1;          /* >=1 po lewej (item[0] zawsze sam sie miesci) */
    if (sp >= n) sp = n - 1;      /* >=1 po prawej */
    return sp;
}

/* Wstaw/zaktualizuj (key,val,len) w lisciu o bptr `node`. Wynik w `res`. CoW: stary lisc free.
 * Split na dwa ~rowne po sumie rozmiarow DANYCH gdy nie miesci sie w jednym bloku. */
static int leaf_insert(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                       uint64_t gen, const struct gh2_key *key, const void *val, uint32_t len,
                       struct ins_res *res) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    const struct gh2_leaf_item *it = leaf_items_c(buf);
    uint32_t nr = h->nritems;
    uint64_t owner = h->owner;

    int found = 0;
    uint32_t idx = leaf_find(buf, key, &found);

    /* Zbierz docelowy zbior itemow (posortowany) w tablicy zrodel. */
    struct leaf_item_src src[GH2_NODE_SPACE / sizeof(struct gh2_leaf_item) + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nr; i++) {
        if (found && i == idx) {
            src[n].key = *key; src[n].data = (const uint8_t *)val; src[n].len = len; n++;
        } else {
            if (!found && i == idx) {     /* miejsce na nowy klucz przed itemem idx */
                src[n].key = *key; src[n].data = (const uint8_t *)val; src[n].len = len; n++;
            }
            src[n].key = it[i].key;
            src[n].data = (const uint8_t *)buf + it[i].data_off;
            src[n].len = it[i].data_len;
            n++;
        }
    }
    if (!found && idx == nr) {            /* nowy klucz na koncu */
        src[n].key = *key; src[n].data = (const uint8_t *)val; src[n].len = len; n++;
    }

    /* Czy calosc miesci sie w jednym lisciu? */
    uint32_t total_data = 0;
    for (uint32_t i = 0; i < n; i++) total_data += src[i].len;
    uint32_t need = n * (uint32_t)sizeof(struct gh2_leaf_item) + total_data;
    if (need <= GH2_NODE_SPACE) {
        struct gh2_bptr nb;
        r = leaf_build_write(dev, a, gen, owner, src, n, &nb);
        if (r) return r;
        cow_free(a, node);
        res->split = 0; res->node = nb;
        return 0;
    }

    /* SPLIT: punkt podzialu = najwiekszy prefiks mieszczacy sie w GH2_NODE_SPACE, >=1 item po
     * kazdej stronie. Dzieki limitowi GH2_LEAF_MAX_VAL (item <= NODE_SPACE/2) OBIE polowki
     * gwarantowanie <= GH2_NODE_SPACE (walidacja w leaf_build_write to backstop, nie powinna
     * sie nigdy odpalic). */
    uint32_t sp = leaf_split_point(src, n);

    struct gh2_bptr lb, rb;
    r = leaf_build_write(dev, a, gen, owner, src, sp, &lb);
    if (r) return r;
    r = leaf_build_write(dev, a, gen, owner, src + sp, n - sp, &rb);
    if (r) { cow_free(a, &lb); return r; }
    cow_free(a, node);
    res->split = 1; res->node = lb; res->right = rb; res->up_key = src[sp].key;
    return 0;
}

/* Zbuduj wezel wewnetrzny z n wskaznikow (key,child) — posortowanych — i zapisz. */
static int internal_build_write_lvl(struct gh_dev *dev, struct gh2_alloc *a, uint64_t gen,
                                    uint64_t owner, uint8_t level,
                                    const struct gh2_internal_ptr *ptrs, uint32_t n,
                                    struct gh2_bptr *out) {
    uint8_t nb[GH2_BLOCK_SIZE];
    memset(nb, 0, sizeof(nb));
    struct gh2_node_hdr *nh = node_hdr(nb);
    nh->level = level; nh->nritems = n; nh->generation = gen; nh->owner = owner;
    struct gh2_internal_ptr *p = int_ptrs(nb);
    for (uint32_t i = 0; i < n; i++) p[i] = ptrs[i];
    return gh2_node_write(dev, a, nb, gen, out);
}

/* rekurencyjny insert do poddrzewa o bptr `node`. */
static int node_insert(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                       uint64_t gen, const struct gh2_key *key, const void *val, uint32_t len,
                       struct ins_res *res) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);

    if (h->level == 0)
        return leaf_insert(dev, a, node, gen, key, val, len, res);

    /* wezel wewnetrzny */
    uint8_t level = h->level;
    uint64_t owner = h->owner;
    uint32_t nr = h->nritems;
    const struct gh2_internal_ptr *p = int_ptrs_c(buf);
    uint32_t ci = internal_child_idx(buf, key);

    struct gh2_bptr child = p[ci].child;
    struct ins_res cres;
    r = node_insert(dev, a, &child, gen, key, val, len, &cres);
    if (r) return r;

    /* Skopiuj wskazniki, podmieniajac dziecko ci na nowe (CoW). */
    struct gh2_internal_ptr np[GH2_INT_CAP + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nr; i++) np[n++] = p[i];
    /* zaktualizuj wskaznik dziecka ci (nowy bptr po CoW). Klucz dziecka ci moze sie zmienic
     * gdy wstawilismy klucz mniejszy niz dotychczasowe minimum poddrzewa: ustaw key[ci]
     * na min(key[ci], wstawiany_klucz) tylko gdy nie bylo splitu (split nie zmienia min lewego
     * inaczej niz przez sam insert; obejmiemy to ponizej). */
    np[ci].child = cres.node;
    /* jesli wstawiany klucz < dotychczasowy key[ci], to minimum poddrzewa zmalalo. */
    if (gh2_key_cmp(key, &np[ci].key) < 0) np[ci].key = *key;

    if (cres.split) {
        /* wstaw nowy wskaznik (up_key -> right) tuz za ci */
        for (uint32_t i = n; i > ci + 1; i--) np[i] = np[i - 1];
        np[ci + 1].key = cres.up_key;
        np[ci + 1].child = cres.right;
        n++;
    }

    cow_free(a, node);

    if (n <= GH2_INT_CAP) {
        struct gh2_bptr nb;
        r = internal_build_write_lvl(dev, a, gen, owner, level, np, n, &nb);
        if (r) return r;
        res->split = 0; res->node = nb;
        return 0;
    }

    /* SPLIT wezla wewnetrznego: promuj SRODKOWY klucz w gore. Lewy: [0..mid), prawy: (mid..n).
     * up_key = np[mid].key; prawy zaczyna sie od np[mid+1]. Btree klasyczny: srodkowy klucz
     * idzie w gore (nie zostaje w zadnym dziecku, bo to klucz-rozdzielajacy). */
    uint32_t mid = n / 2;
    struct gh2_key up = np[mid].key;
    struct gh2_bptr lb, rb;
    r = internal_build_write_lvl(dev, a, gen, owner, level, np, mid, &lb);
    if (r) return r;
    r = internal_build_write_lvl(dev, a, gen, owner, level, np + mid, n - mid, &rb);
    if (r) { cow_free(a, &lb); return r; }
    res->split = 1; res->node = lb; res->right = rb; res->up_key = up;
    return 0;
}

int gh2_btree_insert(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *root,
                     uint64_t gen, const struct gh2_key *key, const void *val, uint32_t len,
                     struct gh2_bptr *out_root) {
    /* wartosc wieksza niz mieszczaca sie w pustym lisciu -> -EFBIG (ekstenty: v2.4) */
    if (len > GH2_LEAF_MAX_VAL) return -EFBIG;

    struct ins_res res;
    int r = node_insert(dev, a, root, gen, key, val, len, &res);
    if (r) return r;

    if (!res.split) { *out_root = res.node; return 0; }

    /* korzen sie podzielil -> nowy korzen o poziom wyzej. Poziom = poziom dzieci + 1.
     * Odczytaj poziom lewego dziecka (nowego). */
    uint8_t cb[GH2_BLOCK_SIZE];
    r = gh2_node_read(dev, &res.node, cb);
    if (r) return r;
    uint8_t child_level = node_hdr_c(cb)->level;
    uint64_t owner = node_hdr_c(cb)->owner;

    struct gh2_internal_ptr np[2];
    /* klucz lewego = najmniejszy klucz lewego poddrzewa. Dla korzenia uzywamy minimalnego
     * klucza calego drzewa; bezpiecznie: znajdz min lewego dziecka. */
    struct gh2_key lmin;
    if (child_level == 0) {
        lmin = leaf_items_c(cb)[0].key;
    } else {
        lmin = int_ptrs_c(cb)[0].key;
    }
    np[0].key = lmin; np[0].child = res.node;
    np[1].key = res.up_key; np[1].child = res.right;

    struct gh2_bptr nroot;
    r = internal_build_write_lvl(dev, a, gen, owner, child_level + 1, np, 2, &nroot);
    if (r) return r;
    *out_root = nroot;
    return 0;
}

/* ============================ bulk-insert (v2.10) ============================
 *
 * gh2_btree_insert_run: wstaw POSORTOWANY rosnaco UNIKALNY ciag itemow CoW-ujac sciezke RAZ
 * na lisc (nie raz na item). Wynik MUSI byc IDENTYCZNY (bajt-exact zawartosc + struktura) co
 * `n` kolejnych gh2_btree_insert. Aby to zagwarantowac, ODTWARZAMY DOKLADNIE semantyke single-
 * insertu: itemy wstawiane po JEDNYM do biezacego "roboczego" wezla; split DOKLADNIE jak w
 * single-insercie (lisc: leaf_split_point — najwiekszy prefiks; wezel wewn.: mid=n/2). Roznica
 * tylko taka, ze CoW starego wezla i zapis nowych dzieje sie RAZ na (roboczy) wezel, a nie raz
 * na item — itemy nalezace do tego samego liscia nie CoW-uja sciezki wielokrotnie.
 *
 * Mechanizm: rekurencja jak node_insert, ale dla SLICE itemow [lo,hi). Zwraca LISTE "kawalkow"
 * (pieces) — po splitcie wezel rozpada sie na >1 kawalek. Kazdy kawalek: bptr + min_key (klucz
 * do promocji w rodzicu). Stary wezel CoW-free RAZ. Rodzic wstawia kawalki dzieci po jednym,
 * dzielac sie DOKLADNIE jak single-insert (mid=n/2 przy nadmiarze). */

struct run_piece { struct gh2_bptr bptr; struct gh2_key min_key; };

/* fwd-decl rekurencji */
static int run_insert_node(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                           uint64_t gen, const struct gh2_kv *items, uint32_t lo, uint32_t hi,
                           struct run_piece *out, uint32_t *pnout);

/* przetworz LISC `node` dla itemow [lo,hi). KROK 1: sklej PELNY posortowany zbior = istniejace
 * itemy liscia + slice [lo,hi) (insert lub UPDATE gdy klucz istnieje — wartosc/dlugosc zmieniona).
 * Itemy `items` sa rosnace+unikalne, istniejace tez; scalanie listowe O(nr + slice). KROK 2:
 * PAKUJ greedy w liscie (leaf_split_point — najwiekszy prefiks <= NODE_SPACE) — IDENTYCZNIE jak
 * sekwencja single-insertow (kazdy emitowany lisc = najwiekszy prefiks mieszczacy sie). Emituj
 * kazdy lisc jako kawalek. Stary lisc CoW-free RAZ. */
static int run_insert_leaf(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                           uint64_t gen, const struct gh2_kv *items, uint32_t lo, uint32_t hi,
                           struct run_piece *out, uint32_t *pnout) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    const struct gh2_leaf_item *it = leaf_items_c(buf);
    uint32_t nr = h->nritems;
    uint64_t owner = h->owner;

    /* KROK 1: scalanie posortowane istniejace (it[]) + slice (items[lo,hi)). Dane istniejacych
     * wskazuja do `buf` (zywy przez cala funkcje), nowych do items[].val. Maks: nr + (hi-lo). */
    uint32_t cap = nr + (hi - lo);
    struct leaf_item_src *merged = malloc((size_t)cap * sizeof(*merged));
    if (!merged) return -ENOMEM;
    uint32_t mn = 0, ei = 0, si = lo;
    while (ei < nr || si < hi) {
        int take_existing;
        if (ei >= nr) take_existing = 0;
        else if (si >= hi) take_existing = 1;
        else {
            int c = gh2_key_cmp(&it[ei].key, &items[si].key);
            if (c < 0) take_existing = 1;
            else if (c > 0) take_existing = 0;
            else {                                  /* UPDATE: klucz istnieje -> bierz nowa wartosc */
                merged[mn].key = items[si].key;
                merged[mn].data = (const uint8_t *)items[si].val;
                merged[mn].len = items[si].len;
                mn++; ei++; si++; continue;
            }
        }
        if (take_existing) {
            merged[mn].key = it[ei].key;
            merged[mn].data = (const uint8_t *)buf + it[ei].data_off;
            merged[mn].len = it[ei].data_len;
            mn++; ei++;
        } else {
            merged[mn].key = items[si].key;
            merged[mn].data = (const uint8_t *)items[si].val;
            merged[mn].len = items[si].len;
            mn++; si++;
        }
    }

    /* KROK 2: pakuj greedy. Emituj liscie [0..sp1)[sp1..sp2)... az do konca. */
    uint32_t start = 0;
    while (start < mn) {
        uint32_t remaining = mn - start;
        uint32_t sp;
        /* czy reszta [start..mn) miesci sie w jednym lisciu? */
        uint32_t total = 0;
        for (uint32_t i = start; i < mn; i++) total += merged[i].len;
        uint32_t need = remaining * (uint32_t)sizeof(struct gh2_leaf_item) + total;
        if (need <= GH2_NODE_SPACE) {
            sp = remaining;                          /* caly ogon do jednego liscia */
        } else {
            sp = leaf_split_point(merged + start, remaining);  /* najwiekszy prefiks */
        }
        struct gh2_bptr lb;
        r = leaf_build_write(dev, a, gen, owner, merged + start, sp, &lb);
        if (r) { free(merged); return r; }
        out[*pnout].bptr = lb; out[*pnout].min_key = merged[start].key; (*pnout)++;
        start += sp;
    }
    free(merged);
    cow_free(a, node);
    return 0;
}

/* Wstaw POJEDYNCZY wskaznik-dziecko (key,child) do roboczej tablicy `np` (n wskaznikow) w
 * pozycji posortowanej; jesli klucz istnieje (ten sam separator) -> PODMIEN dziecko (update
 * sciezki). Jesli po wstawieniu n > INT_CAP -> split mid=n/2 jak single-insert: emituj LEWY
 * kawalek do out, zostaw PRAWY jako roboczy. */
static int run_int_add(struct gh_dev *dev, struct gh2_alloc *a, uint64_t gen, uint64_t owner,
                       uint8_t level, struct gh2_internal_ptr *np, uint32_t *pn,
                       const struct gh2_key *key, const struct gh2_bptr *child,
                       struct run_piece *out, uint32_t *pnout) {
    uint32_t n = *pn;
    /* pozycja: pierwszy i z key[i] >= key */
    uint32_t pos = 0;
    while (pos < n && gh2_key_cmp(&np[pos].key, key) < 0) pos++;
    if (pos < n && gh2_key_cmp(&np[pos].key, key) == 0) {
        np[pos].child = *child;     /* ten sam separator -> podmien dziecko (CoW sciezki) */
    } else {
        for (uint32_t i = n; i > pos; i--) np[i] = np[i - 1];
        np[pos].key = *key; np[pos].child = *child;
        n++;
    }
    if (n <= GH2_INT_CAP) { *pn = n; return 0; }

    /* SPLIT wezla wewnetrznego DOKLADNIE jak single-insert: lewy [0..mid), prawy [mid..n),
     * up_key = klucz prawego[0] (min prawego poddrzewa). Jak w node_insert split: srodkowy
     * separator idzie w gore jako min prawego kawalka. */
    uint32_t mid = n / 2;
    struct gh2_bptr lb;
    int r = internal_build_write_lvl(dev, a, gen, owner, level, np, mid, &lb);
    if (r) return r;
    out[*pnout].bptr = lb; out[*pnout].min_key = np[0].key; (*pnout)++;
    for (uint32_t i = 0; i + mid < n; i++) np[i] = np[mid + i];
    *pn = n - mid;
    return 0;
}

/* przetworz WEZEL WEWNETRZNY `node` dla itemow [lo,hi). KROK 1: dla kazdego dziecka schodzimy
 * z pod-slice itemow, dostajemy LISTE kawalkow (dziecko moglo sie rozpasc na >1). Sklejamy
 * pelna POSORTOWANA liste zastepczych wskaznikow `merged` (dzieci nietkniete + zastapione
 * kawalkami). KROK 2: ODTWARZAMY single-insert — wkladamy `merged` po JEDNYM do roboczego
 * wezla, split mid=n/2 przy nadmiarze (jak node_insert). To gwarantuje IDENTYCZNA strukture
 * co N single-insertow (promocje ida w gore w kolejnosci rosnacej, dokladnie jak pojedyncze
 * inserty). Stary wezel CoW-free RAZ. */
static int run_insert_internal(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                               uint64_t gen, const struct gh2_kv *items, uint32_t lo, uint32_t hi,
                               struct run_piece *out, uint32_t *pnout) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    uint8_t level = h->level;
    uint64_t owner = h->owner;
    uint32_t nr = h->nritems;
    /* skopiuj wskazniki (rekurencja nadpisze buf) */
    struct gh2_internal_ptr p[GH2_INT_CAP + 2];
    const struct gh2_internal_ptr *pc = int_ptrs_c(buf);
    for (uint32_t i = 0; i < nr; i++) p[i] = pc[i];

    /* KROK 1: pelna posortowana lista zastepczych wskaznikow. Maks: nr dzieci, z ktorych kazde
     * moglo sie rozpasc; ale tylko dzieci z itemami sie rozpadaja. Bezpieczna gorna granica:
     * dla kazdego itemu max 1 dodatkowy split lisca + propagacja — w praktyce << ale alokujemy
     * dynamicznie wg liczby itemow + dzieci. */
    uint32_t cap = nr + (hi - lo) + 4;
    struct gh2_internal_ptr *merged = malloc((size_t)cap * sizeof(*merged));
    if (!merged) return -ENOMEM;
    uint32_t mn = 0;

    uint32_t i = lo;
    uint32_t ci_prev = 0;     /* sledzi ostatnio przetworzone dziecko, by skopiowac nietkniete */
    int started = 0;
    while (i < hi) {
        uint32_t ci = internal_child_idx(buf, &items[i].key);
        /* skopiuj dzieci [ci_prev_next .. ci) bez itemow (nietkniete) */
        uint32_t from = started ? ci_prev + 1 : 0;
        for (uint32_t c = from; c < ci; c++) merged[mn++] = p[c];

        struct gh2_key sep_hi; int has_hi = (ci + 1 < nr);
        if (has_hi) sep_hi = p[ci + 1].key;
        uint32_t j = i + 1;
        while (j < hi && (!has_hi || gh2_key_cmp(&items[j].key, &sep_hi) < 0)) j++;

        /* dziecko (lisc) moze rozpasc sie na <= (istniejace itemy liscia) + (itemy slice) + 1
         * kawalkow. Gorna granica istniejacych itemow liscia = GH2_NODE_SPACE/sizeof(item). */
        uint32_t kcap = (j - i) + (GH2_NODE_SPACE / (uint32_t)sizeof(struct gh2_leaf_item)) + 2;
        struct run_piece *kids = malloc((size_t)kcap * sizeof(*kids));
        if (!kids) { free(merged); return -ENOMEM; }
        uint32_t nk = 0;
        r = run_insert_node(dev, a, &p[ci].child, gen, items, i, j, kids, &nk);
        if (r) { free(kids); free(merged); return r; }
        for (uint32_t k = 0; k < nk; k++) {
            merged[mn].key = kids[k].min_key;
            merged[mn].child = kids[k].bptr;
            mn++;
        }
        free(kids);
        ci_prev = ci; started = 1;
        i = j;
    }
    /* skopiuj pozostale dzieci za ostatnim przetworzonym (nietkniete) */
    for (uint32_t c = ci_prev + 1; c < nr; c++) merged[mn++] = p[c];

    /* KROK 2: odtworz single-insert — wkladaj merged[] po jednym, split mid=n/2 przy nadmiarze */
    struct gh2_internal_ptr np[GH2_INT_CAP + 4];
    uint32_t cn = 0;
    for (uint32_t k = 0; k < mn; k++) {
        r = run_int_add(dev, a, gen, owner, level, np, &cn,
                        &merged[k].key, &merged[k].child, out, pnout);
        if (r) { free(merged); return r; }
    }
    free(merged);

    struct gh2_bptr nb;
    r = internal_build_write_lvl(dev, a, gen, owner, level, np, cn, &nb);
    if (r) return r;
    out[*pnout].bptr = nb; out[*pnout].min_key = np[0].key; (*pnout)++;
    cow_free(a, node);
    return 0;
}

static int run_insert_node(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                           uint64_t gen, const struct gh2_kv *items, uint32_t lo, uint32_t hi,
                           struct run_piece *out, uint32_t *pnout) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    if (node_hdr_c(buf)->level == 0)
        return run_insert_leaf(dev, a, node, gen, items, lo, hi, out, pnout);
    return run_insert_internal(dev, a, node, gen, items, lo, hi, out, pnout);
}

int gh2_btree_insert_run(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *root,
                         uint64_t gen, const struct gh2_kv *items, uint32_t n,
                         struct gh2_bptr *out_root) {
    if (n == 0) { *out_root = *root; return 0; }
    /* wartosc za duza -> -EFBIG ZANIM cokolwiek wstawimy (atomowo: *out_root nietkniety) */
    for (uint32_t i = 0; i < n; i++)
        if (items[i].len > GH2_LEAF_MAX_VAL) return -EFBIG;
#ifndef NDEBUG
    /* debug: caller gwarantuje posortowanie rosnace + unikalnosc */
    for (uint32_t i = 1; i < n; i++) {
        int c = gh2_key_cmp(&items[i - 1].key, &items[i].key);
        if (c >= 0) return -EINVAL;
    }
#endif

    /* bufor kawalkow korzenia: gdy korzen to lisc, moze rozpasc sie na <= (itemy liscia)+(n)+1
     * kawalkow; gdy wewnetrzny — mniej. Gorna granica bezpieczna. */
    uint32_t pcap = n + (GH2_NODE_SPACE / (uint32_t)sizeof(struct gh2_leaf_item)) + 2;
    struct run_piece *pieces = malloc((size_t)pcap * sizeof(*pieces));
    if (!pieces) return -ENOMEM;
    uint32_t npieces = 0;
    int r = run_insert_node(dev, a, root, gen, items, 0, n, pieces, &npieces);
    if (r) { free(pieces); return r; }

    /* zbuduj nowe korzenie az do jednego (jak single-insert: korzen split -> nowy poziom). */
    while (npieces > 1) {
        /* poziom nowego korzenia = poziom kawalkow + 1 */
        uint8_t cb[GH2_BLOCK_SIZE];
        r = gh2_node_read(dev, &pieces[0].bptr, cb);
        if (r) { free(pieces); return r; }
        uint8_t child_level = node_hdr_c(cb)->level;
        uint64_t owner = node_hdr_c(cb)->owner;

        /* wstaw kawalki po jednym do roboczej tablicy korzenia (split mid=n/2 jak single).
         * Liczba wynikowych kawalkow tego poziomu <= npieces (maleje przy kazdym poziomie). */
        struct run_piece *out = malloc((size_t)(npieces + 1) * sizeof(*out));
        if (!out) { free(pieces); return -ENOMEM; }
        uint32_t nout = 0;
        struct gh2_internal_ptr np[GH2_INT_CAP + 4];
        uint32_t cn = 0;
        for (uint32_t k = 0; k < npieces; k++) {
            r = run_int_add(dev, a, gen, owner, (uint8_t)(child_level + 1), np, &cn,
                            &pieces[k].min_key, &pieces[k].bptr, out, &nout);
            if (r) { free(out); free(pieces); return r; }
        }
        struct gh2_bptr nb;
        r = internal_build_write_lvl(dev, a, gen, owner, (uint8_t)(child_level + 1), np, cn, &nb);
        if (r) { free(out); free(pieces); return r; }
        out[nout].bptr = nb; out[nout].min_key = np[0].key; nout++;

        for (uint32_t k = 0; k < nout; k++) pieces[k] = out[k];
        npieces = nout;
        free(out);
    }

    struct gh2_bptr final = pieces[0].bptr;
    free(pieces);
    *out_root = final;
    return 0;
}

/* ============================ delete (Task 3) ============================
 *
 * CoW delete z pelnym rebalansowaniem (borrow/merge). Rekurencja schodzi do liscia
 * (kopiujac sciezke), usuwa item, a nastepnie — gdy dziecko spada ponizej MIN —
 * naprawia je na poziomie RODZICA (rodzic widzi sasiadow): borrow od bogatego sasiada
 * lub merge z sasiadem. Po naprawie rodzic moze sam spasc <MIN → propagacja w gore.
 * Na koniec, gdy korzen wewnetrzny ma 1 dziecko → obnizenie wysokosci.
 *
 * Wynik rekurencyjnego delete (del_res):
 *   found == 0: klucza nie bylo w poddrzewie (zadnej zmiany, `node` = stary bptr).
 *   found == 1: usunieto. `node` = nowy bptr (CoW) tego poddrzewa.
 *               underfull == 1 gdy poddrzewo spadlo ponizej progu MIN (rodzic naprawi).
 *               min_key = aktualny NAJMNIEJSZY klucz poddrzewa (rodzic aktualizuje key[ci]).
 */
struct del_res {
    int            found;
    int            underfull;
    struct gh2_bptr node;
    struct gh2_key  min_key;
};


/* usun item `key` z liscia `node` (CoW). Zaklada, ze key ISTNIEJE w lisciu.
 * Zwraca nowy bptr w res->node; underfull gdy used < GH2_LEAF_MIN. */
static int leaf_delete(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                       uint64_t gen, const struct gh2_key *key, struct del_res *res) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    const struct gh2_leaf_item *it = leaf_items_c(buf);
    uint32_t nr = h->nritems;
    uint64_t owner = h->owner;

    int found = 0;
    uint32_t idx = leaf_find(buf, key, &found);
    if (!found) { res->found = 0; res->node = *node; return 0; }

    /* zbierz wszystkie itemy oprocz idx */
    struct leaf_item_src src[GH2_NODE_SPACE / sizeof(struct gh2_leaf_item) + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nr; i++) {
        if (i == idx) continue;
        src[n].key = it[i].key;
        src[n].data = (const uint8_t *)buf + it[i].data_off;
        src[n].len = it[i].data_len;
        n++;
    }

    struct gh2_bptr nb;
    r = leaf_build_write(dev, a, gen, owner, src, n, &nb);
    if (r) return r;
    cow_free(a, node);

    res->found = 1;
    res->node = nb;
    /* zuzyte bajty po usunieciu */
    uint32_t used = n * (uint32_t)sizeof(struct gh2_leaf_item);
    for (uint32_t i = 0; i < n; i++) used += src[i].len;
    res->underfull = (used < GH2_LEAF_MIN);
    if (n > 0) res->min_key = src[0].key;
    return 0;
}

/* Napraw dziecko `ci` (underfull) wezla wewnetrznego, ktorego wskazniki sa w tablicy `p`
 * (nritems = *pn). Dziecko ci JUZ jest nowym (CoW) bptr.
 *
 * Strategia "combine + redistribute" (zawsze poprawna, takze dla itemow ZMIENNEJ dlugosci):
 *   - wybierz sasiada (lewy preferowany, inaczej prawy); para = (lewy, prawy) w kolejnosci.
 *   - polacz WSZYSTKIE itemy/wskazniki pary w jedna posortowana liste.
 *   - jesli calosc miesci sie w jednym bloku -> MERGE: zapisz 1 wezel, usun 1 wskaznik z rodzica.
 *   - inaczej -> REDYSTRYBUCJA (borrow): podziel na dwa ~rowne wezly; oba >= polowa => underfull
 *     znika. Klucze-rozdzielajace w rodzicu aktualizowane z faktycznych min nowych wezlow.
 * To eliminuje przepelnienie odbiorcy (klasyczny borrow/merge zawodzi przy duzych wartosciach).
 * Wszystko CoW: stare bloki obu uczestnikow free, nowe zapisane. */
static int fixup_child(struct gh_dev *dev, struct gh2_alloc *a, uint64_t gen, uint64_t owner,
                       uint8_t child_level, struct gh2_internal_ptr *p, uint32_t *pn,
                       uint32_t ci) {
    uint32_t n = *pn;
    int has_left  = (ci > 0);
    int has_right = (ci + 1 < n);
    /* brak sasiadow (jedyne dziecko): nie da sie naprawic na tym poziomie — zostaw.
     * Moze wystapic tylko gdy rodzic jest korzeniem; obnizenie wysokosci to scali. */
    if (!has_left && !has_right) return 0;

    /* indeksy pary do polaczenia: (li, ri) = (left, right) sasiadujace, gdzie jeden to ci. */
    uint32_t li = has_left ? ci - 1 : ci;
    uint32_t ri = li + 1;

    uint8_t lbuf[GH2_BLOCK_SIZE], rbuf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, &p[li].child, lbuf);
    if (r) return r;
    r = gh2_node_read(dev, &p[ri].child, rbuf);
    if (r) return r;

    if (child_level == 0) {
        /* ---------------- LISCIE ---------------- */
        const struct gh2_leaf_item *lit = leaf_items_c(lbuf);
        const struct gh2_leaf_item *rit = leaf_items_c(rbuf);
        uint32_t ln = node_hdr_c(lbuf)->nritems;
        uint32_t rn = node_hdr_c(rbuf)->nritems;

        struct leaf_item_src all[2 * (GH2_NODE_SPACE / sizeof(struct gh2_leaf_item) + 2)];
        uint32_t tot = 0, total_data = 0;
        for (uint32_t i = 0; i < ln; i++) {
            all[tot].key = lit[i].key;
            all[tot].data = (const uint8_t *)lbuf + lit[i].data_off;
            all[tot].len = lit[i].data_len;
            total_data += lit[i].data_len; tot++;
        }
        for (uint32_t i = 0; i < rn; i++) {
            all[tot].key = rit[i].key;
            all[tot].data = (const uint8_t *)rbuf + rit[i].data_off;
            all[tot].len = rit[i].data_len;
            total_data += rit[i].data_len; tot++;
        }
        uint32_t need = tot * (uint32_t)sizeof(struct gh2_leaf_item) + total_data;

        if (need <= GH2_NODE_SPACE) {
            /* MERGE: jeden lisc na pozycji li, usun ri */
            struct gh2_bptr mb;
            r = leaf_build_write(dev, a, gen, owner, all, tot, &mb);
            if (r) return r;
            cow_free(a, &p[li].child);
            cow_free(a, &p[ri].child);
            p[li].child = mb; p[li].key = all[0].key;
            for (uint32_t i = ri; i + 1 < n; i++) p[i] = p[i + 1];
            *pn = n - 1;
            return 0;
        }
        /* REDYSTRYBUCJA: ten sam pewny punkt podzialu co przy split insertu — najwiekszy
         * prefiks <= GH2_NODE_SPACE, >=1 item po kazdej stronie. Zrodla to wazne liscie
         * (kazdy <= NODE_SPACE, kazdy item <= NODE_SPACE/2), wiec OBIE polowki <= NODE_SPACE. */
        uint32_t sp = leaf_split_point(all, tot);
        struct gh2_bptr nlb, nrb;
        r = leaf_build_write(dev, a, gen, owner, all, sp, &nlb);
        if (r) return r;
        r = leaf_build_write(dev, a, gen, owner, all + sp, tot - sp, &nrb);
        if (r) { cow_free(a, &nlb); return r; }
        cow_free(a, &p[li].child);
        cow_free(a, &p[ri].child);
        p[li].child = nlb; p[li].key = all[0].key;
        p[ri].child = nrb; p[ri].key = all[sp].key;
        return 0;
    } else {
        /* ---------------- WEZLY WEWNETRZNE ---------------- */
        const struct gh2_internal_ptr *lp = int_ptrs_c(lbuf);
        const struct gh2_internal_ptr *rp = int_ptrs_c(rbuf);
        uint32_t ln = node_hdr_c(lbuf)->nritems;
        uint32_t rn = node_hdr_c(rbuf)->nritems;

        struct gh2_internal_ptr all[2 * (GH2_INT_CAP + 2)];
        uint32_t tot = 0;
        for (uint32_t i = 0; i < ln; i++) all[tot++] = lp[i];
        for (uint32_t i = 0; i < rn; i++) all[tot++] = rp[i];

        if (tot <= GH2_INT_CAP) {
            /* MERGE: jeden wezel na pozycji li, usun ri */
            struct gh2_bptr mb;
            r = internal_build_write_lvl(dev, a, gen, owner, child_level, all, tot, &mb);
            if (r) return r;
            cow_free(a, &p[li].child);
            cow_free(a, &p[ri].child);
            p[li].child = mb; p[li].key = all[0].key;
            for (uint32_t i = ri; i + 1 < n; i++) p[i] = p[i + 1];
            *pn = n - 1;
            return 0;
        }
        /* REDYSTRYBUCJA: podziel na dwa ~rowne po liczbie wskaznikow */
        uint32_t sp = tot / 2;
        struct gh2_bptr nlb, nrb;
        r = internal_build_write_lvl(dev, a, gen, owner, child_level, all, sp, &nlb);
        if (r) return r;
        r = internal_build_write_lvl(dev, a, gen, owner, child_level, all + sp, tot - sp, &nrb);
        if (r) { cow_free(a, &nlb); return r; }
        cow_free(a, &p[li].child);
        cow_free(a, &p[ri].child);
        p[li].child = nlb; p[li].key = all[0].key;
        p[ri].child = nrb; p[ri].key = all[sp].key;
        return 0;
    }
}

/* rekurencyjny delete z poddrzewa o bptr `node`. */
static int node_delete(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *node,
                       uint64_t gen, const struct gh2_key *key, struct del_res *res) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);

    if (h->level == 0)
        return leaf_delete(dev, a, node, gen, key, res);

    /* wezel wewnetrzny */
    uint8_t level = h->level;
    uint64_t owner = h->owner;
    uint32_t nr = h->nritems;
    const struct gh2_internal_ptr *p = int_ptrs_c(buf);
    uint32_t ci = internal_child_idx(buf, key);

    struct gh2_bptr child = p[ci].child;
    struct del_res cres;
    r = node_delete(dev, a, &child, gen, key, &cres);
    if (r) return r;
    if (!cres.found) { res->found = 0; res->node = *node; return 0; }

    /* skopiuj wskazniki, podmien dziecko ci (CoW) + popraw jego klucz na nowe min poddrzewa */
    struct gh2_internal_ptr np[GH2_INT_CAP + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nr; i++) np[n++] = p[i];
    np[ci].child = cres.node;
    np[ci].key = cres.min_key;

    if (cres.underfull) {
        r = fixup_child(dev, a, gen, owner, (uint8_t)(level - 1), np, &n, ci);
        if (r) return r;
    }

    cow_free(a, node);

    struct gh2_bptr nb;
    r = internal_build_write_lvl(dev, a, gen, owner, level, np, n, &nb);
    if (r) return r;

    res->found = 1;
    res->node = nb;
    res->underfull = (n < GH2_INT_MIN);
    res->min_key = np[0].key;
    return 0;
}

int gh2_btree_delete(struct gh_dev *dev, struct gh2_alloc *a, const struct gh2_bptr *root,
                     uint64_t gen, const struct gh2_key *key, struct gh2_bptr *out_root) {
    struct del_res res;
    int r = node_delete(dev, a, root, gen, key, &res);
    if (r) return r;
    if (!res.found) return -ENOENT;

    /* obnizanie wysokosci: jesli nowy korzen jest wewnetrzny i ma 1 dziecko,
     * to dziecko staje sie nowym korzeniem (powtarzaj, az >1 dziecka lub lisc). */
    struct gh2_bptr cur = res.node;
    for (;;) {
        uint8_t buf[GH2_BLOCK_SIZE];
        r = gh2_node_read(dev, &cur, buf);
        if (r) return r;
        const struct gh2_node_hdr *h = node_hdr_c(buf);
        if (h->level == 0 || h->nritems != 1) break;
        struct gh2_bptr only = int_ptrs_c(buf)[0].child;
        cow_free(a, &cur);
        cur = only;
    }
    *out_root = cur;
    return 0;
}

/* ============================ iterate ============================ */

static int iterate_node(struct gh_dev *dev, const struct gh2_bptr *node,
                        int (*cb)(const struct gh2_key *, const void *, uint32_t, void *),
                        void *ctx) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    if (h->level == 0) {
        const struct gh2_leaf_item *it = leaf_items_c(buf);
        for (uint32_t i = 0; i < h->nritems; i++) {
            const void *val = (const uint8_t *)buf + it[i].data_off;
            int rc = cb(&it[i].key, val, it[i].data_len, ctx);
            if (rc != 0) return rc;
        }
        return 0;
    }
    /* wezel wewnetrzny: DFS in-order po dzieciach (rosnaco po kluczu). Kopiujemy wskazniki
     * do lokalnej tablicy, bo rekurencja nadpisze `buf`. */
    uint32_t nr = h->nritems;
    struct gh2_internal_ptr p[GH2_INT_CAP + 2];
    const struct gh2_internal_ptr *pc = int_ptrs_c(buf);
    for (uint32_t i = 0; i < nr; i++) p[i] = pc[i];
    for (uint32_t i = 0; i < nr; i++) {
        int rc = iterate_node(dev, &p[i].child, cb, ctx);
        if (rc != 0) return rc;
    }
    return 0;
}

int gh2_btree_iterate(struct gh_dev *dev, const struct gh2_bptr *root,
                      int (*cb)(const struct gh2_key *, const void *, uint32_t, void *),
                      void *ctx) {
    return iterate_node(dev, root, cb, ctx);
}

/* ============================ iterate_range ============================ */

/* In-order po poddrzewie `node`, emituj tylko itemy o kluczu w [min,max].
 * Przycinanie: w wezle wewnetrznym pomijamy dzieci, ktorych caly zakres lezy poza [min,max].
 * Dziecko i obejmuje klucze [key[i], key[i+1]); ostatnie [key[nr-1], +inf). */
static int iterate_range_node(struct gh_dev *dev, const struct gh2_bptr *node,
                              const struct gh2_key *min_key, const struct gh2_key *max_key,
                              int (*cb)(const struct gh2_key *, const void *, uint32_t, void *),
                              void *ctx) {
    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    if (h->level == 0) {
        const struct gh2_leaf_item *it = leaf_items_c(buf);
        for (uint32_t i = 0; i < h->nritems; i++) {
            if (gh2_key_cmp(&it[i].key, min_key) < 0) continue;
            if (gh2_key_cmp(&it[i].key, max_key) > 0) break;   /* posortowane: dalej tylko wieksze */
            const void *val = (const uint8_t *)buf + it[i].data_off;
            int rc = cb(&it[i].key, val, it[i].data_len, ctx);
            if (rc != 0) return rc;
        }
        return 0;
    }
    uint32_t nr = h->nritems;
    struct gh2_internal_ptr p[GH2_INT_CAP + 2];
    const struct gh2_internal_ptr *pc = int_ptrs_c(buf);
    for (uint32_t i = 0; i < nr; i++) p[i] = pc[i];
    for (uint32_t i = 0; i < nr; i++) {
        /* dolna granica dziecka i = p[i].key; gorna = p[i+1].key (lub +inf) */
        if (i + 1 < nr && gh2_key_cmp(&p[i + 1].key, min_key) <= 0) continue;  /* caly < min */
        if (gh2_key_cmp(&p[i].key, max_key) > 0) break;                         /* caly > max */
        int rc = iterate_range_node(dev, &p[i].child, min_key, max_key, cb, ctx);
        if (rc != 0) return rc;
    }
    return 0;
}

int gh2_btree_iterate_range(struct gh_dev *dev, const struct gh2_bptr *root,
                            const struct gh2_key *min_key, const struct gh2_key *max_key,
                            int (*cb)(const struct gh2_key *, const void *, uint32_t, void *),
                            void *ctx) {
    if (gh2_key_cmp(min_key, max_key) > 0) return 0;
    return iterate_range_node(dev, root, min_key, max_key, cb, ctx);
}

/* ============================ walk_nodes (mark-sweep) ============================ */

/* DFS po WSZYSTKICH wezlach (internal+leaf). Woła cb(block) dla biezacego wezla, potem
 * (jesli wewnetrzny) schodzi rekurencyjnie do dzieci. Pomija block==0. */
static int walk_node(struct gh_dev *dev, const struct gh2_bptr *node,
                     int (*cb)(uint64_t, void *), void *ctx) {
    if (node->block == 0) return 0;
    int rc = cb(node->block, ctx);
    if (rc != 0) return rc;
    /* DUP v2.8: 2. kopia wezla jest osobnym blokiem fizycznym referencyjnym przez ten sam
     * bptr -> mark-sweep/refcount MUSZA ja policzyc (inaczej alloc nadpisalby zywa kopie). */
    if (node->dup_block != 0) {
        rc = cb(node->dup_block, ctx);
        if (rc != 0) return rc;
    }

    uint8_t buf[GH2_BLOCK_SIZE];
    int r = gh2_node_read(dev, node, buf);
    if (r) return r;
    const struct gh2_node_hdr *h = node_hdr_c(buf);
    if (h->level == 0) return 0;   /* lisc: brak dzieci */

    /* wezel wewnetrzny: skopiuj wskazniki (rekurencja nadpisze buf), zejdz do dzieci */
    uint32_t nr = h->nritems;
    struct gh2_internal_ptr p[GH2_INT_CAP + 2];
    const struct gh2_internal_ptr *pc = int_ptrs_c(buf);
    for (uint32_t i = 0; i < nr; i++) p[i] = pc[i];
    for (uint32_t i = 0; i < nr; i++) {
        rc = walk_node(dev, &p[i].child, cb, ctx);
        if (rc != 0) return rc;
    }
    return 0;
}

int gh2_btree_walk_nodes(struct gh_dev *dev, const struct gh2_bptr *root,
                         int (*cb)(uint64_t block, void *ctx), void *ctx) {
    return walk_node(dev, root, cb, ctx);
}
