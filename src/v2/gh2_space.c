#define _POSIX_C_SOURCE 200809L
#include "v2/gh2_space.h"
#include "v2/gh2_ncache.h"   /* write-back cache: immediate-free brudnych wezlow this-txn */
#include "v2/gh2_format.h"   /* GH2_SB_SLOT_A/B, GH2_DATA_START */
#include "v2/gh2_fs.h"       /* GH2_EXTENT_DATA, struct gh2_extent (mark-sweep blokow danych) */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2.2 — mapa wolnej przestrzeni + alokator transakcyjny + mark-sweep.
 * ========================================================================== */

/* ============================ bitmapa ============================ */

int gh2_space_init(struct gh2_space *s, uint64_t nblocks) {
    s->bits = NULL; s->refs = NULL; s->nblocks = 0; s->hint = 0; s->nfree = 0;
    if (nblocks == 0) return -EINVAL;
    uint64_t nbytes = (nblocks + 7) / 8;
    s->bits = calloc((size_t)nbytes, 1);   /* 0 = wszystko wolne */
    if (!s->bits) return -ENOMEM;
    s->refs = calloc((size_t)nblocks, sizeof(uint16_t));   /* refcount 0 wszedzie */
    if (!s->refs) { free(s->bits); s->bits = NULL; return -ENOMEM; }
    s->nblocks = nblocks;
    s->nfree = nblocks;
    /* bloki superbloku (sloty A/B = 0,1) zajete (refcount 1); region [GH2_DATA_START..) wolny */
    gh2_space_set(s, GH2_SB_SLOT_A, 1);
    if (nblocks > GH2_SB_SLOT_B) gh2_space_set(s, GH2_SB_SLOT_B, 1);
    s->hint = GH2_DATA_START;
    return 0;
}

void gh2_space_destroy(struct gh2_space *s) {
    free(s->bits);
    free(s->refs);
    s->bits = NULL; s->refs = NULL; s->nblocks = 0; s->hint = 0; s->nfree = 0;
}

int gh2_space_is_used(const struct gh2_space *s, uint64_t blk) {
    if (blk >= s->nblocks) return 1;   /* poza zakresem traktujemy jak zajete (nie alokuj) */
    return (s->bits[blk >> 3] >> (blk & 7)) & 1u;
}

/* niskopoziomowy set: utrzymuje niezmiennik used <=> refs>0.
 * set(1) na wolnym -> refs=1; set(1) na zajetym -> bez zmiany refs (idempotentne).
 * set(0) -> refs=0 (twardo zwalnia, niezaleznie od licznika; uzywane przez dec przy 0,
 * abort/rollback alokacji, oraz hurtowy build_from_tree na swiezej mapie). */
void gh2_space_set(struct gh2_space *s, uint64_t blk, int used) {
    if (blk >= s->nblocks) return;
    int was = (s->bits[blk >> 3] >> (blk & 7)) & 1u;
    if (used) {
        if (!was) {
            s->bits[blk >> 3] |= (uint8_t)(1u << (blk & 7)); s->nfree--;
            if (s->refs && s->refs[blk] == 0) s->refs[blk] = 1;
        }
    } else {
        if (was)  { s->bits[blk >> 3] &= (uint8_t)~(1u << (blk & 7)); s->nfree++; }
        if (s->refs) s->refs[blk] = 0;
    }
}

/* ============================ refcounty (v2.7) ============================ */

uint16_t gh2_ref_get(const struct gh2_space *s, uint64_t blk) {
    if (blk >= s->nblocks || !s->refs) return 0;
    return s->refs[blk];
}

/* inc: rc++ (saturuje); ustawia used. */
void gh2_ref_inc(struct gh2_space *s, uint64_t blk) {
    if (blk >= s->nblocks || !s->refs) return;
    if (s->refs[blk] < GH2_REF_MAX) s->refs[blk]++;
    if (s->refs[blk] == 1) gh2_space_set(s, blk, 1);   /* 0->1: oznacz zajety */
}

/* dec: rc-- (clamp 0); przy 0 -> zwolnij w mapie. Bez efektu gdy rc==0. */
void gh2_ref_dec(struct gh2_space *s, uint64_t blk) {
    if (blk >= s->nblocks || !s->refs) return;
    if (s->refs[blk] == 0) return;
    /* saturacja jest nieodwracalna — nie dekrementuj od GH2_REF_MAX (stracilismy dokladnosc) */
    if (s->refs[blk] == GH2_REF_MAX) return;
    s->refs[blk]--;
    if (s->refs[blk] == 0) gh2_space_set(s, blk, 0);   /* ostatnia referencja -> wolny */
}

/* first-fit od hint z zawijaniem; oznacz zajety; -ENOSPC gdy brak wolnych */
int gh2_space_alloc_one(struct gh2_space *s, uint64_t *out) {
    if (s->nfree == 0) return -ENOSPC;
    uint64_t start = s->hint;
    if (start < GH2_DATA_START) start = GH2_DATA_START;
    if (start >= s->nblocks) start = GH2_DATA_START;
    /* przeszukaj [start..nblocks) potem [GH2_DATA_START..start) */
    for (uint64_t i = 0; i < s->nblocks; i++) {
        uint64_t blk = start + i;
        if (blk >= s->nblocks) blk = GH2_DATA_START + (blk - s->nblocks);
        if (blk >= s->nblocks) continue;   /* zabezpieczenie */
        if (!gh2_space_is_used(s, blk)) {
            gh2_space_set(s, blk, 1);
            s->hint = blk + 1;
            *out = blk;
            return 0;
        }
    }
    return -ENOSPC;   /* nfree != 0 ale nic nie znaleziono — niespojnosc; bezpieczny fallback */
}

/* ============================ alokator transakcyjny ============================ */

int gh2_txn_alloc_init(struct gh2_txn_alloc *t, struct gh2_space *s) {
    t->space = s;
    t->defer_dec = NULL; t->ndd = 0; t->ddcap = 0;
    t->defer_inc = NULL; t->ndi = 0; t->dicap = 0;
    t->txn_alloced = NULL; t->nta = 0; t->tacap = 0;
    t->superseded = NULL; t->nss = 0; t->sscap = 0;
    t->oom = 0;
    t->dup_meta = 0;        /* DUP off domyslnie; gh2_fs ustawia z flagi SB GH2_SB_DUP_META */
    t->ncache = NULL;       /* write-back off domyslnie; gh2_fs ustawia z dev->v2_ncache */
    t->op_floor = 0;        /* poczatek biezacej operacji (ustawiany przez gh2_txn_alloc_mark) */
    return 0;
}

void gh2_txn_alloc_destroy(struct gh2_txn_alloc *t) {
    free(t->defer_dec);   t->defer_dec = NULL;   t->ndd = 0; t->ddcap = 0;
    free(t->defer_inc);   t->defer_inc = NULL;   t->ndi = 0; t->dicap = 0;
    free(t->txn_alloced); t->txn_alloced = NULL; t->nta = 0; t->tacap = 0;
    free(t->superseded);  t->superseded = NULL;  t->nss = 0; t->sscap = 0;
}

/* dopisz blk do dynamicznej tablicy; 0 OK, -ENOMEM przy braku pamieci */
static int push_block(uint64_t **arr, uint32_t *n, uint32_t *cap, uint64_t blk) {
    if (*n == *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 32;
        uint64_t *na = realloc(*arr, (size_t)ncap * sizeof(uint64_t));
        if (!na) return -ENOMEM;
        *arr = na; *cap = ncap;
    }
    (*arr)[(*n)++] = blk;
    return 0;
}

/* alloc = alloc_one (ustawia refcount 1) + push txn_alloced. Przy OOM na liscie: zwolnij blok
 * z mapy (rc->0) i zwroc blad, by mutacja B-drzewa wycofala sie czysto. */
static int txn_alloc_fn(void *ctx, uint64_t *out_block) {
    struct gh2_txn_alloc *t = ctx;
    uint64_t blk;
    int r = gh2_space_alloc_one(t->space, &blk);   /* set(1) => refcount=1 */
    if (r) return r;
    r = push_block(&t->txn_alloced, &t->nta, &t->tacap, blk);
    if (r) {
        gh2_space_set(t->space, blk, 0);   /* cofnij alokacje (rc->0, nie zaksiegowana) */
        t->oom = 1;
        return r;                          /* -ENOMEM -> mutacja drzewa sie wycofa */
    }
    *out_block = blk;
    return 0;
}

/* czy `block` zostal zaalokowany w BIEZACEJ operacji (txn_alloced[op_floor..nta))? Tylko taki
 * brudny wezel mozna immediate-free: rollback biezacej op i tak go cofnie (nie jest czescia
 * stanu sprzed operacji). Brudny wezel z POPRZEDNIEJ op (tej samej txn) jest czescia stanu
 * sprzed op -> rollback musi go odtworzyc -> defer (zostaje w cache, blok niereuzyty). */
static int block_is_this_op(const struct gh2_txn_alloc *t, uint64_t block) {
    for (uint32_t i = t->op_floor; i < t->nta; i++)
        if (t->txn_alloced[i] == block) return 1;
    return 0;
}

/* free vtable. Trzy sciezki:
 *   (a) WRITE-BACK + blok W cache + alokowany w BIEZACEJ operacji (this-op fresh): usun z cache
 *       (bufor wezla NIGDY nie trafi na dysk) + zwolnij blok NATYCHMIAST (rc->0 -> reuse w tej
 *       txn). Eliminuje zapis wezlow posrednich ORAZ pozwala reuzyc ich bloki. Bezpieczne:
 *       rollback biezacej op cofa te alokacje; stan sprzed op nie wskazuje na ten blok.
 *   (b) WRITE-BACK + blok W cache, ale z POPRZEDNIEJ op tej samej txn (superseded prior-op): wezel
 *       ZOSTAJE w cache NA RAZIE (rollback-safety: rollback biezacej op przywroci *root sprzed op,
 *       ktory moze nan wskazywac — bufor musi byc odczytywalny), a blok NIE jest reuzywany.
 *       Dodaj do listy `superseded`. Po SUKCESIE biezacej op (gh2_txn_alloc_op_commit): usun z
 *       cache (NIE flush — nie czesc finalnego drzewa) + zwolnij blok. Po ROLLBACK: porzuc (zostaje).
 *       KLUCZ REDUKCJI: bez tego superseded korzenie/wezly akumulowaly sie w cache i byly flushowane
 *       przy commicie -> brak redukcji zapisow. Teraz finalny flush pisze tylko ZYWE wezly.
 *   (c) inaczej (committed wezel / dane / brak cache): defer_dec (jak dotad).
 * Przy OOM listy: blok ZOSTAJE (wyciek, nie korupcja), flaga oom. */
static void txn_free_fn(void *ctx, uint64_t block) {
    struct gh2_txn_alloc *t = ctx;
    if (block == 0) return;   /* 0 = brak bloku */
    struct gh2_ncache *nc = t->ncache;
    if (nc && gh2_ncache_has(nc, block)) {
        if (block_is_this_op(t, block)) {
            gh2_ncache_remove(nc, block);      /* wezel posredni biezacej op: nigdy na dysk */
            gh2_space_set(t->space, block, 0); /* immediate free (rc->0): reuse w tej txn */
            return;
        }
        /* (b) superseded prior-op cached node: zostaw w cache (rollback-safety), zapisz na liste.
         * NIE defer_dec (op_commit zwolni blok po sukcesie; rollback porzuci). Blok pozostaje
         * zajety do op_commit -> stan spojny niezaleznie od wyniku op. */
        if (push_block(&t->superseded, &t->nss, &t->sscap, block) != 0)
            t->oom = 1;        /* nie pamietamy -> wezel zostanie sflushowany (brak redukcji, nie korupcja) */
        return;
    }
    /* committed wezel LUB dane LUB brak cache: defer dec (blok zostaje do commit). Commit: ref_dec. */
    if (push_block(&t->defer_dec, &t->ndd, &t->ddcap, block) != 0)
        t->oom = 1;            /* nie pamietamy -> blok wyciekl (wciaz zajety), ale spojnie */
}

struct gh2_alloc gh2_txn_alloc_vtable(struct gh2_txn_alloc *t) {
    struct gh2_alloc a;
    a.alloc = txn_alloc_fn;
    a.free  = txn_free_fn;
    a.ctx   = t;
    a.dup_meta = t->dup_meta;   /* v2.8: przepisz flage DUP do vtable B-drzewa */
    return a;
}

/* odroz inc refcount bloku do commitu (snapshot wspoldzielenia; Task 3). Przy OOM: flaga. */
void gh2_txn_alloc_defer_inc(struct gh2_txn_alloc *t, uint64_t blk) {
    if (blk == 0) return;
    if (push_block(&t->defer_inc, &t->ndi, &t->dicap, blk) != 0)
        t->oom = 1;
}

/* odroz dec refcount bloku do commitu (subvol-delete; Task 4). Identyczne z free vtable, lecz
 * wywolywane jawnie (nie przez B-drzewo). Przy commit: ref_dec (przy rc 0 -> zwolnij blok). */
void gh2_txn_alloc_defer_dec(struct gh2_txn_alloc *t, uint64_t blk) {
    if (blk == 0) return;
    if (push_block(&t->defer_dec, &t->ndd, &t->ddcap, blk) != 0)
        t->oom = 1;
}

/* commit: zastosuj defer_inc (ref_inc) POTEM defer_dec (ref_dec; przy rc 0 zwalnia); wyczysc listy.
 * Kolejnosc inc-przed-dec: blok jednoczesnie inc i dec (snapshot wspolnego bloku) nie spadnie
 * blednie do 0 miedzy operacjami. txn_alloced juz maja rc=1 (z alloc) -> tylko wyczysc. */
int gh2_txn_alloc_commit(struct gh2_txn_alloc *t) {
    for (uint32_t i = 0; i < t->ndi; i++)
        gh2_ref_inc(t->space, t->defer_inc[i]);
    for (uint32_t i = 0; i < t->ndd; i++)
        gh2_ref_dec(t->space, t->defer_dec[i]);
    t->ndi = 0;
    t->ndd = 0;
    t->nta = 0;
    t->nss = 0;   /* superseded juz sfinalizowane (op_commit przed flushem); defensywnie wyczysc */
    return 0;
}

/* cofnij blok zaalokowany w txn: zwolnij w mapie (rc->0). WRITE-BACK: usun tez ewentualny
 * brudny bufor z cache — blok wraca do puli wolnych i moze byc REUZYTY (np. jako blok danych
 * pisany bezposrednio na dysk); stary bufor wezla w cache zostalby blednie sflushowany na
 * commit, nadpisujac reuzyta tresc. */
static void txn_undo_alloc(struct gh2_txn_alloc *t, uint64_t blk) {
    gh2_space_set(t->space, blk, 0);
    if (t->ncache) gh2_ncache_remove(t->ncache, blk);
}

/* abort: cofnij alokacje tej transakcji (rc->0, przywroc mape); PORZUC defer_dec i defer_inc.
 * superseded: porzuc liste — wezly ZOSTAJA w cache (abort przywraca caly stan transakcji). */
void gh2_txn_alloc_abort(struct gh2_txn_alloc *t) {
    for (uint32_t i = 0; i < t->nta; i++)
        txn_undo_alloc(t, t->txn_alloced[i]);   /* rc->0 + usun z cache */
    t->nta = 0;
    t->ndd = 0;
    t->ndi = 0;
    t->nss = 0;
}

/* op SUKCES: sfinalizuj superseded prior-op cached nodes biezacej op. Dla kazdego bloku:
 * usun bufor z cache (NIGDY nie trafi na dysk — nie jest czescia finalnego drzewa) + zwolnij
 * blok (set 0 -> rc->0). Bezpieczne bo: op sie udala (fs_root=nowy, NIE wskazuje na superseded);
 * wezel byl tylko w cache (nigdy nieflushowany) -> nie na dysku; disk-committed drzewo (sprzed
 * tej op, z poprzedniego gh2_fs_commit) tez go nie wskazuje (nowszy niz disk). => brak referencji
 * => wolny. Wyczysc liste. Wolane na granicy operacji gdy poprzednia op sie UDALA. */
void gh2_txn_alloc_op_commit(struct gh2_txn_alloc *t) {
    struct gh2_ncache *nc = t->ncache;
    for (uint32_t i = 0; i < t->nss; i++) {
        uint64_t blk = t->superseded[i];
        if (nc) gh2_ncache_remove(nc, blk);   /* nie flushuj martwego wezla */
        gh2_space_set(t->space, blk, 0);      /* zwolnij blok (rc->0) */
    }
    t->nss = 0;
}

/* savepoint: zapamietaj biezace dlugosci list (granica miedzy operacjami). Ustaw op_floor =
 * nta -> bloki alokowane OD TERAZ sa "this-op fresh" (immediate-free dozwolony przy write-back).
 * NIE finalizuje superseded — atomowosc per-op: finalizacja superseded nalezy do SUKCESU WLASNEJ
 * operacji (gh2_txn_alloc_op_commit wolane TUZ PO fs->fs_root=root w gh2_fs.c), NIE do startu
 * nastepnej. Dzieki temu NIEUDANA op (ktora i tak robi mark) NIE zwalnia superseded poprzedniej
 * udanej op -> mapa/fs_root niezmienione przy bledzie (atomowosc). */
struct gh2_txn_savepoint gh2_txn_alloc_mark(struct gh2_txn_alloc *t) {
    struct gh2_txn_savepoint sp = { t->nta, t->ndd, t->ndi };
    t->op_floor = t->nta;
    return sp;
}

/* rollback do savepointu: cofnij (rc->0) TYLKO bloki zaalokowane od savepointu
 * (txn_alloced[sp.nta..nta)); porzuc odroczone dec/inc od savepointu (stary fs_root niezmieniony).
 * Wczesniejsze bloki (przed sp) pozostaja nietkniete. */
void gh2_txn_alloc_rollback(struct gh2_txn_alloc *t, struct gh2_txn_savepoint sp) {
    for (uint32_t i = sp.nta; i < t->nta; i++)
        txn_undo_alloc(t, t->txn_alloced[i]);   /* rc->0 + usun z cache (reuse-safe) */
    t->nta = sp.nta;
    t->ndd = sp.ndd;
    t->ndi = sp.ndi;
    /* PORZUC superseded biezacej op: wezly ZOSTAJA w cache (NIE usunieto/zwolniono w free vtable);
     * stary fs_root (przywracany przez callera) moze nan wskazywac -> musza przezyc, czytelne. */
    t->nss = 0;
}

/* ============================ mark-sweep ============================ */

static int mark_used_cb(uint64_t block, void *ctx) {
    struct gh2_space *s = ctx;
    gh2_space_set(s, block, 1);
    return 0;
}

/* mark-sweep BLOKOW DANYCH: dla kazdego leaf-itemu typu EXTENT_DATA oznacz disk_block
 * (i dup_block jesli !=0) jako zajety. Bez tego alokacja nadpisalaby zywe dane plikow. */
static int mark_extent_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_space *s = ctx;
    if (key->type == GH2_EXTENT_COMP) {
        /* v2.9: chunk extent -> oznacz KAZDY blok z blocks[] (inaczej alloc nadpisze zywe dane) */
        struct gh2_cext_hdr hdr;
        uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
        if (gh2_cext_decode(val, len, &hdr, blocks, GH2_CEXT_MAX_BLOCKS)) return 0;
        for (uint16_t i = 0; i < hdr.nblocks; i++)
            if (blocks[i]) gh2_space_set(s, blocks[i], 1);
        return 0;
    }
    if (key->type != GH2_EXTENT_DATA) return 0;
    if (len != sizeof(struct gh2_extent)) return 0;   /* obce/uszkodzone -> pomin */
    struct gh2_extent e;
    memcpy(&e, val, sizeof(e));
    if (e.disk_block) gh2_space_set(s, e.disk_block, 1);
    if (e.dup_block)  gh2_space_set(s, e.dup_block, 1);
    return 0;
}

/* mapa = wezly drzewa (walk_nodes) + bloki danych ekstentow (walk leaf-itemow). */
int gh2_space_build_from_tree(struct gh_dev *dev, struct gh2_space *s,
                              const struct gh2_bptr *root) {
    int r = gh2_btree_walk_nodes(dev, root, mark_used_cb, s);
    if (r) return r;
    return gh2_btree_iterate(dev, root, mark_extent_cb, s);
}

/* ============================ mark-sweep refcountow (v2.7) ============================ */

/* ZLICZAJ referencje (ref_inc) zamiast tylko set — dla wielu korzeni (snapshoty) blok wspoldzielony
 * dostanie rc>1. Na razie z 1 korzenia => wszystkie rc==1. */
static int count_node_cb(uint64_t block, void *ctx) {
    struct gh2_space *s = ctx;
    gh2_ref_inc(s, block);
    return 0;
}

static int count_extent_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_space *s = ctx;
    if (key->type == GH2_EXTENT_COMP) {
        /* v2.9: chunk extent -> ref_inc KAZDY blok z blocks[] (refcount/snapshot obejmuja chunki) */
        struct gh2_cext_hdr hdr;
        uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
        if (gh2_cext_decode(val, len, &hdr, blocks, GH2_CEXT_MAX_BLOCKS)) return 0;
        for (uint16_t i = 0; i < hdr.nblocks; i++)
            if (blocks[i]) gh2_ref_inc(s, blocks[i]);
        return 0;
    }
    if (key->type != GH2_EXTENT_DATA) return 0;
    if (len != sizeof(struct gh2_extent)) return 0;
    struct gh2_extent e;
    memcpy(&e, val, sizeof(e));
    if (e.disk_block) gh2_ref_inc(s, e.disk_block);
    if (e.dup_block)  gh2_ref_inc(s, e.dup_block);
    return 0;
}

/* zlicz referencje dla JEDNEGO fs_root subwolumenu: wezly drzewa + bloki danych ekstentow. */
static int count_one_subvol(struct gh_dev *dev, struct gh2_space *s,
                            const struct gh2_bptr *fs_root) {
    int r = gh2_btree_walk_nodes(dev, fs_root, count_node_cb, s);
    if (r) return r;
    return gh2_btree_iterate(dev, fs_root, count_extent_cb, s);
}

/* iteracja wpisow drzewa korzeni: dla kazdego GH2_ROOT_ITEM zlicz referencje jego fs_root. */
struct refmap_root_ctx { struct gh_dev *dev; struct gh2_space *s; int rc; };
static int count_subvol_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct refmap_root_ctx *c = ctx;
    if (key->type != GH2_ROOT_ITEM) return 0;
    if (len != sizeof(struct gh2_subvol_item)) return 0;
    struct gh2_subvol_item sv;
    memcpy(&sv, val, sizeof(sv));
    int r = count_one_subvol(c->dev, c->s, &sv.fs_root);
    if (r) { c->rc = r; return r; }
    return 0;
}

/* zeruj refs+mape (poza SB), nastepnie ZLICZ referencje z DRZEWA KORZENI: ref_inc per wezel
 * drzewa korzeni + dla kazdego subwolumenu (GH2_ROOT_ITEM) jego fs_root (wezly + bloki danych).
 * Niezmiennik used==(rc>0). SB (0,1) ustawiane na refcount 1 (jak init). */
int gh2_refmap_build_from_roots(struct gh_dev *dev, struct gh2_space *s,
                                const struct gh2_bptr *root_tree) {
    /* wyzeruj caly stan (refs+bitmapa+nfree), potem ustaw SB jak w init */
    memset(s->refs, 0, (size_t)s->nblocks * sizeof(uint16_t));
    memset(s->bits, 0, (size_t)((s->nblocks + 7) / 8));
    s->nfree = s->nblocks;
    s->hint = GH2_DATA_START;
    gh2_space_set(s, GH2_SB_SLOT_A, 1);   /* rc=1 */
    if (s->nblocks > GH2_SB_SLOT_B) gh2_space_set(s, GH2_SB_SLOT_B, 1);

    /* 1) wezly samego drzewa korzeni */
    int r = gh2_btree_walk_nodes(dev, root_tree, count_node_cb, s);
    if (r) return r;
    /* 2) dla kazdego subwolumenu — jego fs_root (wezly + bloki danych) */
    struct refmap_root_ctx c = { dev, s, 0 };
    r = gh2_btree_iterate(dev, root_tree, count_subvol_cb, &c);
    if (r) return r;
    return c.rc;
}
