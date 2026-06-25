#ifndef GH2_SPACE_H
#define GH2_SPACE_H
/* ghostfs v2.2 — alokator: mapa wolnej przestrzeni (bitmapa w pamieci, 1 bit/blok) +
 * transakcyjny alokator z CoW deferred-free i rollback (abort).
 *
 * Model (v2.2–v2.6, bez snapshotow): blok jest zajety <=> osiagalny z zatwierdzonego
 * superbloku. Stan jest NIEJAWNY w strukturze drzew; mape budujemy przy mount mark-sweep
 * (gh2_space_build_from_tree -> gh2_btree_walk_nodes). Alloc/free dzialaja na mapie w pamieci.
 *
 * CoW deferred-free: nowe bloki alokowane NATYCHMIAST (oznaczane zajete, by kolejne
 * alokacje w tej transakcji ich nie wzialy); stare bloki sciezki CoW NIE sa zwalniane od razu
 * (zywe drzewo wciaz na nie wskazuje) — trafiaja na liste defer_free. Commit -> zwalnia
 * defer_free. Abort -> zwalnia txn_alloced (przywraca mape).
 */
#include <stdint.h>
#include "v2/gh2_btree.h"   /* struct gh2_alloc, struct gh2_bptr, struct gh_dev */

/* ---- mapa wolnej przestrzeni (bitmapa) + refcounty (v2.7) ---- */
/* v2.7: refcount(blok) = liczba referencji ze WSZYSTKICH drzew subwolumenow (odbudowywalny
 * mark-sweep przy mount, jak mapa). Niezmiennik: used(blk) <=> refs[blk] > 0. Bez wspoldzielenia
 * (refcount <= 1) zachowanie identyczne jak v2.2: free->dec->0->wolny == dotychczasowy free. */
#define GH2_REF_MAX  UINT16_MAX   /* saturacja licznika */

struct gh2_space {
    uint8_t *bits;       /* 1 bit/blok; 1 = zajety (== refs>0) */
    uint16_t *refs;      /* refcount per blok (saturuje na GH2_REF_MAX); used <=> refs>0 */
    uint64_t nblocks;    /* liczba blokow urzadzenia */
    uint64_t hint;       /* podpowiedz first-fit (skad zaczynac szukac) */
    uint64_t nfree;      /* liczba wolnych blokow (spojna z bits) */
};

int  gh2_space_init(struct gh2_space *s, uint64_t nblocks);   /* bloki 0,1 zajete (rc1); reszta wolna */
void gh2_space_destroy(struct gh2_space *s);
int  gh2_space_is_used(const struct gh2_space *s, uint64_t blk);
void gh2_space_set(struct gh2_space *s, uint64_t blk, int used);
int  gh2_space_alloc_one(struct gh2_space *s, uint64_t *out);  /* first-fit od hint; -ENOSPC */

/* ---- refcounty (v2.7) ---- */
uint16_t gh2_ref_get(const struct gh2_space *s, uint64_t blk);
/* inc: rc++ (saturuje na GH2_REF_MAX); ustawia used. */
void     gh2_ref_inc(struct gh2_space *s, uint64_t blk);
/* dec: rc-- (clamp na 0); przy 0 -> gh2_space_set(blk, 0) (zwolnij). Bez efektu gdy rc==0. */
void     gh2_ref_dec(struct gh2_space *s, uint64_t blk);

/* ---- alokator transakcyjny (implementuje gh2_alloc z v2.1) ---- */
/* v2.7: free vtable -> DEFER dec (nie defer-free). Commit: defer_inc (ref_inc) potem defer_dec
 * (ref_dec; przy 0 zwalnia). alloc -> alloc_one + refcount=1. defer_inc obsluzy snapshot (Task 3).
 * Bez wspoldzielenia (refcount<=1): defer_dec==stary defer_free (dec 1->0 zwalnia). */
struct gh2_txn_alloc {
    struct gh2_space *space;
    uint64_t *defer_dec;   uint32_t ndd, ddcap;   /* bloki CoW/zwalniane: dec po commicie */
    uint64_t *defer_inc;   uint32_t ndi, dicap;   /* bloki do inc po commicie (snapshot, Task 3) */
    uint64_t *txn_alloced; uint32_t nta, tacap;   /* zaalokowane w txn (rc=1): cofnac przy abort */
    /* v2-ncache redukcja: bloki cached-z-POPRZEDNIEJ-op (this txn) CoW-zastapione w BIEZACEJ op.
     * Wciaz w cache (rollback-safety: stary fs_root moze na nie wskazac). Po SUKCESIE biezacej op
     * (gh2_txn_alloc_op_commit, wolane przy nastepnym mark/commit) -> usun z cache (NIE flush) +
     * zwolnij blok. Po ROLLBACK biezacej op -> porzuc liste (wezly zostaja w cache). */
    uint64_t *superseded;  uint32_t nss, sscap;   /* superseded prior-op cached nodes biezacej op */
    int oom;                                       /* 1 = ENOMEM realloc (alloc zwroci blad) */
    int dup_meta;                                  /* v2.8: przekazywane do gh2_alloc (DUP metadane) */
    void *ncache;  /* write-back cache brudnych wezlow (gh2_ncache*; NULL=off). Free vtable:
                    * blok W cache I alokowany W BIEZACEJ operacji (>= op_floor) -> usun z cache
                    * + immediate free (reuse); inaczej (committed / cached-z-poprzedniej-op-tej-
                    * txn / dane) -> defer_dec. Cached blok z POPRZEDNIEJ op NIE moze byc
                    * immediate-free (rollback biezacej op musi go odtworzyc). */
    uint32_t op_floor;  /* indeks txn_alloced na poczatku biezacej operacji (savepoint.nta) */
};

int  gh2_txn_alloc_init(struct gh2_txn_alloc *t, struct gh2_space *s);
void gh2_txn_alloc_destroy(struct gh2_txn_alloc *t);
struct gh2_alloc gh2_txn_alloc_vtable(struct gh2_txn_alloc *t);  /* {alloc,free,ctx} dla B-drzewa */
/* odroz inc refcount bloku do commitu (snapshot wspoldzielenia; Task 3). */
void gh2_txn_alloc_defer_inc(struct gh2_txn_alloc *t, uint64_t blk);
/* odroz dec refcount bloku do commitu (subvol-delete; Task 4). Przy rc 0 -> zwolni przy commit.
 * Symetryczne do defer_inc; objete savepoint/rollback (porzucone przy rollback). */
void gh2_txn_alloc_defer_dec(struct gh2_txn_alloc *t, uint64_t blk);
int  gh2_txn_alloc_commit(struct gh2_txn_alloc *t);   /* defer_inc (inc) potem defer_dec (dec); wyczysc */
void gh2_txn_alloc_abort(struct gh2_txn_alloc *t);    /* cofnij txn_alloced (rc->0); porzuc defer */

/* v2-ncache redukcja: op SUKCES — sfinalizuj superseded prior-op cached nodes biezacej op:
 * usun je z cache (NIE flushuj — nie sa czescia finalnego drzewa) + zwolnij blok (rc->0; bezpieczne,
 * bo op sukces => fs_root NIE wskazuje na nie, a disk-committed drzewo tez nie). Czysci liste.
 * Wolane na granicy operacji (nastepny mark / commit) — dotarcie tu => poprzednia op sie udala
 * (porazka wola rollback, ktory PORZUCA liste bez free). Idempotentne gdy lista pusta. */
void gh2_txn_alloc_op_commit(struct gh2_txn_alloc *t);

/* ---- savepoint/rollback: atomowosc per-operacja FS (wiele insert/delete na 1 alloc) ---- */
/* Operacja FS robi wiele mutacji dzielac jeden alloc; przy bledzie w srodku trzeba cofnac
 * TYLKO bloki tej operacji (nie hurtowo cala liste txn_alloced jak abort), bo wczesniejsze
 * operacje/transakcje moga miec zywe bloki. Savepoint = znacznik (nta,ndf). */
struct gh2_txn_savepoint { uint32_t nta; uint32_t ndd; uint32_t ndi; };

/* zapamietaj biezacy stan list (przed operacja). */
struct gh2_txn_savepoint gh2_txn_alloc_mark(struct gh2_txn_alloc *t);

/* cofnij do savepointu: cofnij refcount bloki txn_alloced[sp.nta..nta) (rc->0->wolny; byly
 * zaalokowane przez te operacje); obetnij nta=sp.nta. Obetnij defer_dec/defer_inc do
 * (sp.ndd, sp.ndi) (PORZUC odroczone dec/inc tej operacji — stary fs_root niezmieniony). */
void gh2_txn_alloc_rollback(struct gh2_txn_alloc *t, struct gh2_txn_savepoint sp);

/* ---- mark-sweep: zbuduj mape z B-drzewa (oznacz wezly jako zajete) ---- */
int  gh2_space_build_from_tree(struct gh_dev *dev, struct gh2_space *s,
                               const struct gh2_bptr *root);

/* ---- mark-sweep refcountow (v2.7): zbuduj refs[] ZLICZAJAC referencje ---- */
/* Uogolnienie gh2_space_build_from_tree: zamiast tylko set(1), ref_inc dla kazdej referencji.
 * v2.7 (Task 2): `root_tree` to DRZEWO KORZENI (gh2_btree z wpisami GH2_ROOT_ITEM). Liczy:
 *   - ref_inc per wezel DRZEWA KORZENI,
 *   - dla KAZDEGO wpisu GH2_ROOT_ITEM (subwolumenu): walk jego fs_root (ref_inc per wezel)
 *     + dla kazdego EXTENT_DATA ref_inc na disk_block (+dup_block).
 * used==(rc>0). Dla 1 subwolumenu bez wspoldzielenia wszystkie refs==1. Zeruje refs+mape.
 * Blok wspoldzielony przez >1 subwolumen (snapshot, Task 3) dostanie rc>1. */
int  gh2_refmap_build_from_roots(struct gh_dev *dev, struct gh2_space *s,
                                 const struct gh2_bptr *root_tree);

#endif
