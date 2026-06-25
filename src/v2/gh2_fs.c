#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "v2/gh2_fs.h"
#include "csum.h"
#include "crypto.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>

/* ============================================================================
 * ghostfs v2.3 — drzewo FS (i-wezly + katalogi). Task 1: szkielet + create/mkdir/
 * lookup/readdir + commit + persystencja. Task 2 (unlink/rmdir/link/symlink/rename/
 * mknod/meta/truncate) -> osobno.
 * ========================================================================== */

/* maks. wartosc itemu B-drzewa (limit pakowania DIR_ITEM) */
#define GH2_FS_MAX_VAL  GH2_LEAF_MAX_VAL

/* ============================ klucze itemow ============================ */

static struct gh2_key inode_key(uint64_t ino) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = ino; k.type = GH2_INODE_ITEM; k.offset = 0;
    return k;
}
static struct gh2_key dir_key(uint64_t parent, uint64_t name_hash) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = parent; k.type = GH2_DIR_ITEM; k.offset = name_hash;
    return k;
}
static struct gh2_key symlink_key(uint64_t ino) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = ino; k.type = GH2_SYMLINK_DATA; k.offset = 0;
    return k;
}
static struct gh2_key extent_key(uint64_t ino, uint64_t file_off) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = ino; k.type = GH2_EXTENT_DATA; k.offset = file_off;
    return k;
}
/* v2.9: klucz chunk extentu (ino, GH2_EXTENT_COMP, chunk_aligned_off) */
static struct gh2_key comp_key(uint64_t ino, uint64_t chunk_off) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = ino; k.type = GH2_EXTENT_COMP; k.offset = chunk_off;
    return k;
}
/* klucz wpisu subwolumenu w drzewie korzeni: (subvol_id, GH2_ROOT_ITEM, 0) */
static struct gh2_key root_item_key(uint64_t subvol_id) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = subvol_id; k.type = GH2_ROOT_ITEM; k.offset = 0;
    return k;
}

/* ---- enkod/dekod wpisu subwolumenu (memcpy; padding wyzerowany) ---- */
static void subvol_encode(const struct gh2_subvol_item *sv, uint8_t *buf) {
    struct gh2_subvol_item tmp;
    memset(&tmp, 0, sizeof(tmp));        /* wyzeruj ogon name[] / _pad bptr dla determinizmu */
    tmp.fs_root = sv->fs_root;
    tmp.flags = sv->flags;
    size_t nl = strnlen(sv->name, GH2_SUBVOL_NAME_MAX - 1);
    memcpy(tmp.name, sv->name, nl);
    memcpy(buf, &tmp, sizeof(tmp));
}
static int subvol_decode(const uint8_t *buf, uint32_t len, struct gh2_subvol_item *out) {
    if (len != sizeof(struct gh2_subvol_item)) return -EIO;
    memcpy(out, buf, sizeof(*out));
    out->name[GH2_SUBVOL_NAME_MAX - 1] = '\0';   /* gwarancja NUL */
    return 0;
}

/* ============================ FNV-1a 64-bit ============================ */

static uint64_t fnv1a64(const char *name, size_t len) {
    uint64_t h = 1469598103934665603ULL;          /* offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 1099511628211ULL;                     /* prime */
    }
    return h;
}

/* ============================ enkod/dekod i-wezla ============================ */
/* memcpy struktury (deterministyczny; padding wyzerowany przed kopia). */

static void inode_encode(const struct gh2_inode *in, uint8_t *buf) {
    struct gh2_inode tmp;
    memset(&tmp, 0, sizeof(tmp));                  /* wyzeruj _pad dla determinizmu */
    tmp = *in;
    tmp._pad = 0;
    memcpy(buf, &tmp, sizeof(tmp));
}
static int inode_decode(const uint8_t *buf, uint32_t len, struct gh2_inode *out) {
    if (len != sizeof(struct gh2_inode)) return -EIO;
    memcpy(out, buf, sizeof(*out));
    return 0;
}

/* ============================ pomocnicy fs-tree (CoW + txn) ============================ */
/* ATOMOWOSC PER-OPERACJA: helpery operuja na PRZEKAZANYM lokalnym `root` (in/out) i NIE
 * dotykaja fs->fs_root ani NIE wolaja abort/rollback. Kazda publiczna operacja gh2_fs_*
 * robi: root=fs->fs_root; sp=mark; (mutacje na &root); przy bledzie rollback+return (fs_root
 * nietkniety); przy sukcesie fs->fs_root=root. */

/* wstaw item w lokalny root (CoW). Sukces -> *root=nowy. Blad: *root NIETKNIETY (caller robi
 * rollback calej operacji). */
static int fs_insert(struct gh2_fs *fs, struct gh2_bptr *root, const struct gh2_key *key,
                     const void *val, uint32_t len) {
    struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
    struct gh2_bptr nroot;
    int r = gh2_btree_insert(&fs->dev, &a, root, fs->sb.generation,
                             key, val, len, &nroot);
    if (r) return r;
    *root = nroot;
    return 0;
}

/* usun item z lokalnego root (CoW). Sukces -> *root=nowy. Blad: *root NIETKNIETY. */
static int fs_delete(struct gh2_fs *fs, struct gh2_bptr *root, const struct gh2_key *key) {
    struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
    struct gh2_bptr nroot;
    int r = gh2_btree_delete(&fs->dev, &a, root, fs->sb.generation, key, &nroot);
    if (r) return r;
    *root = nroot;
    return 0;
}

/* odczytaj i-wezel po ino z PODANEGO root (lokalny widok operacji) */
static int fs_read_inode_at(struct gh2_fs *fs, const struct gh2_bptr *root, uint64_t ino,
                            struct gh2_inode *out) {
    uint8_t buf[sizeof(struct gh2_inode)];
    uint32_t len = 0;
    struct gh2_key k = inode_key(ino);
    int r = gh2_btree_lookup(&fs->dev, root, &k, buf, sizeof(buf), &len);
    if (r) return r;
    return inode_decode(buf, len, out);
}

/* odczytaj i-wezel po ino (z zatwierdzonego fs->fs_root) — API/odczyty poza operacja */
int gh2_fs_read_inode(struct gh2_fs *fs, uint64_t ino, struct gh2_inode *out) {
    return fs_read_inode_at(fs, &fs->fs_root, ino, out);
}

/* zapisz (insert/update) i-wezel w lokalny root */
static int fs_write_inode(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino,
                          const struct gh2_inode *in) {
    uint8_t buf[sizeof(struct gh2_inode)];
    inode_encode(in, buf);
    struct gh2_key k = inode_key(ino);
    return fs_insert(fs, root, &k, buf, sizeof(buf));
}

/* atomowy zapis pojedynczego i-wezla (chmod/chown/utimens/truncate): savepoint+rollback. */
static int fs_commit_one_inode(struct gh2_fs *fs, uint64_t ino, const struct gh2_inode *in) {
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
    int r = fs_write_inode(fs, &root, ino, in);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    fs->fs_root = root;
    return 0;
}

/* ============================ v2.4: dane plikow (ekstenty) ============================ */
/* Model: 1 item EXTENT_DATA / blok 4 KB. Klucz (ino, GH2_EXTENT_DATA, file_off). Wartosc =
 * struct gh2_extent (memcpy). Dziura = brak itemu -> zera. CoW: nadpisanie alokuje NOWY blok,
 * stary -> defer_free. Czesciowy blok -> read-modify-write. Csum = crc32 plaintextu bloku. */

#define GH2_DATA_BLK  GH2_BLOCK_SIZE   /* rozmiar bloku danych (= blok urzadzenia) */

static void extent_encode(const struct gh2_extent *e, uint8_t *buf) {
    struct gh2_extent tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp = *e;
    memcpy(buf, &tmp, sizeof(tmp));
}
static int extent_decode(const uint8_t *buf, uint32_t len, struct gh2_extent *out) {
    if (len != sizeof(struct gh2_extent)) return -EIO;
    memcpy(out, buf, sizeof(*out));
    return 0;
}

/* lookup ekstentu bloku file_off w lokalnym root. 0=jest (out wypelniony), -ENOENT=dziura. */
static int extent_lookup_at(struct gh2_fs *fs, const struct gh2_bptr *root, uint64_t ino,
                            uint64_t file_off, struct gh2_extent *out) {
    uint8_t buf[sizeof(struct gh2_extent)];
    uint32_t len = 0;
    struct gh2_key k = extent_key(ino, file_off);
    int r = gh2_btree_lookup(&fs->dev, root, &k, buf, sizeof(buf), &len);
    if (r) return r;
    return extent_decode(buf, len, out);
}

/* odczytaj plaintext bloku danego ekstentu do `buf` (GH2_DATA_BLK B; ogon poza raw_len = 0).
 * Weryfikuj csum -> niezgodnosc: dup_block jesli !=0, inaczej -EIO. */
static int data_block_read(struct gh2_fs *fs, const struct gh2_extent *e, uint8_t *buf) {
    int r = gh_disk_read(&fs->dev, e->disk_block, buf);
    if (r) return r;
    uint32_t rl = e->raw_len; if (rl > GH2_DATA_BLK) rl = GH2_DATA_BLK;
    if (gh_crc32(buf, rl) != e->csum) {
        if (e->dup_block) {
            r = gh_disk_read(&fs->dev, e->dup_block, buf);
            if (r) return r;
            if (gh_crc32(buf, rl) != e->csum) return -EIO;
        } else {
            return -EIO;
        }
    }
    if (rl < GH2_DATA_BLK) memset(buf + rl, 0, GH2_DATA_BLK - rl);
    return 0;
}

/* CoW zapis fragmentu [boff, boff+n) bloku file_off pliku ino w lokalnym root.
 * - alokuj NOWY blok danych (CoW); gdy zapis czesciowy -> read-modify-write (odczytaj stary
 *   ekstent/zera, nalozenie src), inaczej zera + src.
 * - gh_disk_write(plaintext), csum=crc32(raw_len), insert/update EXTENT_DATA.
 * - stary disk_block (jesli ekstent istnial) -> alloc free (defer).
 * raw_len ekstentu = max(stary raw_len, boff+n) (ostatni czesciowy blok). */
static int write_block(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino, uint64_t file_off,
                       const uint8_t *src, uint32_t n, uint32_t boff) {
    if (boff + n > GH2_DATA_BLK) return -EINVAL;

    struct gh2_extent old; int have_old = (extent_lookup_at(fs, root, ino, file_off, &old) == 0);

    uint8_t blk[GH2_DATA_BLK];
    uint32_t old_raw = 0;
    if (boff != 0 || n != GH2_DATA_BLK) {
        /* czesciowy -> RMW: zacznij od starej zawartosci (lub zer dla dziury) */
        if (have_old) {
            int r = data_block_read(fs, &old, blk);   /* zwraca pelne 4 KB (ogon=0) */
            if (r) return r;
            old_raw = old.raw_len; if (old_raw > GH2_DATA_BLK) old_raw = GH2_DATA_BLK;
        } else {
            memset(blk, 0, GH2_DATA_BLK);
        }
    }
    /* nalozenie nowych bajtow */
    if (boff != 0 || n != GH2_DATA_BLK) memcpy(blk + boff, src, n);
    else memcpy(blk, src, GH2_DATA_BLK);

    uint32_t raw_len = boff + n;
    if (raw_len < old_raw) raw_len = old_raw;          /* nie skracaj istniejacych danych */
    if (raw_len > GH2_DATA_BLK) raw_len = GH2_DATA_BLK;

    /* alokuj NOWY blok danych przez vtable alokatora (defer-free przy CoW) */
    struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
    uint64_t nblk = 0;
    int r = a.alloc(a.ctx, &nblk);
    if (r) return r;
    r = gh_disk_write(&fs->dev, nblk, blk);
    if (r) { a.free(a.ctx, nblk); return r; }

    struct gh2_extent ne;
    memset(&ne, 0, sizeof(ne));
    ne.disk_block = nblk;
    ne.dup_block = 0;
    ne.csum = gh_crc32(blk, raw_len);
    ne.comp_algo = 0;
    ne.flags = 0;
    ne.raw_len = raw_len;
    ne.comp_len = raw_len;

    uint8_t ebuf[sizeof(struct gh2_extent)];
    extent_encode(&ne, ebuf);
    struct gh2_key k = extent_key(ino, file_off);
    r = fs_insert(fs, root, &k, ebuf, sizeof(ebuf));
    if (r) { a.free(a.ctx, nblk); return r; }          /* nowy blok -> defer (rollback cofnie) */

    /* stary blok danych CoW -> defer_free */
    if (have_old && old.disk_block) a.free(a.ctx, old.disk_block);
    return 0;
}

/* zwolnij WSZYSTKIE ekstenty pliku ino (disk_block defer-free + usun itemy) w lokalnym root.
 * Uzywane przez unlink (nlink->0) i truncate. */
struct free_ext_ctx { struct gh2_fs *fs; struct gh2_bptr *root; uint64_t ino; int rc;
                      uint64_t *offs; uint32_t n, cap; uint64_t *blks; };
static int collect_ext_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct free_ext_ctx *c = ctx;
    if (key->type != GH2_EXTENT_DATA) return 0;
    struct gh2_extent e;
    if (extent_decode(val, len, &e)) return 0;
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 32;
        uint64_t *no = realloc(c->offs, (size_t)nc * sizeof(uint64_t));
        uint64_t *nb = realloc(c->blks, (size_t)nc * sizeof(uint64_t));
        if (!no || !nb) { free(no ? no : c->offs); free(nb ? nb : c->blks);
                          c->offs = NULL; c->blks = NULL; c->rc = -ENOMEM; return -ENOMEM; }
        c->offs = no; c->blks = nb; c->cap = nc;
    }
    c->offs[c->n] = key->offset;
    c->blks[c->n] = e.disk_block;
    c->n++;
    return 0;
}

static int gh2_inode_free_chunks(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino);

static int gh2_inode_free_extents(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino) {
    /* v2.9: kontener --compress trzyma dane w chunk extentach (GH2_EXTENT_COMP), nie per-blok. */
    if (fs->compress) return gh2_inode_free_chunks(fs, root, ino);
    struct free_ext_ctx c; memset(&c, 0, sizeof(c));
    c.fs = fs; c.root = root; c.ino = ino;
    struct gh2_key min = extent_key(ino, 0);
    struct gh2_key max = extent_key(ino, UINT64_MAX);
    int r = gh2_btree_iterate_range(&fs->dev, root, &min, &max, collect_ext_cb, &c);
    if (r) { free(c.offs); free(c.blks); return c.rc ? c.rc : r; }

    struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
    for (uint32_t i = 0; i < c.n; i++) {
        struct gh2_key k = extent_key(ino, c.offs[i]);
        r = fs_delete(fs, root, &k);
        if (r) { free(c.offs); free(c.blks); return r; }
        if (c.blks[i]) a.free(a.ctx, c.blks[i]);       /* defer-free bloku danych */
    }
    free(c.offs); free(c.blks);
    return 0;
}

/* ============================ v2.9: chunk extenty (kompresja) ============================ */
/* Model dla kontenera --compress: 1 item GH2_EXTENT_COMP / chunk (GH2_COMP_CHUNK*4096 = 32 KB
 * logicznych). Wartosc = gh2_cext_hdr + uint64_t blocks[nblocks]. comp_algo: 0=raw, 1=zlib.
 * Dziura = brak itemu -> zera. CoW: nadpisanie chunku alokuje NOWE bloki, stare -> defer_free.
 * csum = crc32 ZAPISANYCH (comp_len) bajtow. Bloki nieciagle (alloc pojedynczo). */

#define GH2_CHUNK_BYTES  ((uint64_t)GH2_COMP_CHUNK * GH2_DATA_BLK)   /* 32768 B logicznych */

/* enkod naglowka + blocks[]; deterministyczny (padding hdr wyzerowany). buf >= GH2_CEXT_MAX_VAL.
 * Zwraca dlugosc zakodowanej wartosci. */
static uint32_t cext_encode(const struct gh2_cext_hdr *hdr, const uint64_t *blocks, uint8_t *buf) {
    struct gh2_cext_hdr tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.comp_algo = hdr->comp_algo;
    tmp.flags = hdr->flags;
    tmp.nblocks = hdr->nblocks;
    tmp.raw_len = hdr->raw_len;
    tmp.comp_len = hdr->comp_len;
    tmp.csum = hdr->csum;
    memcpy(buf, &tmp, sizeof(tmp));
    uint32_t off = (uint32_t)sizeof(tmp);
    for (uint16_t i = 0; i < hdr->nblocks; i++) {
        memcpy(buf + off, &blocks[i], sizeof(uint64_t));
        off += (uint32_t)sizeof(uint64_t);
    }
    return off;
}

/* dekod naglowka + blocks[] (eksponowany dla mark-sweep w gh2_space.c). */
int gh2_cext_decode(const uint8_t *buf, uint32_t len, struct gh2_cext_hdr *hdr,
                    uint64_t *blocks, uint16_t max_blocks) {
    if (len < sizeof(struct gh2_cext_hdr)) return -EIO;
    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->nblocks > max_blocks) return -EIO;
    if (len != sizeof(struct gh2_cext_hdr) + (uint32_t)hdr->nblocks * sizeof(uint64_t))
        return -EIO;
    if (blocks) {
        uint32_t off = (uint32_t)sizeof(struct gh2_cext_hdr);
        for (uint16_t i = 0; i < hdr->nblocks; i++) {
            memcpy(&blocks[i], buf + off, sizeof(uint64_t));
            off += (uint32_t)sizeof(uint64_t);
        }
    }
    return 0;
}

/* lookup chunk extentu (ino, GH2_EXTENT_COMP, chunk_off). 0=jest, -ENOENT=dziura. */
static int comp_lookup_at(struct gh2_fs *fs, const struct gh2_bptr *root, uint64_t ino,
                          uint64_t chunk_off, struct gh2_cext_hdr *hdr, uint64_t *blocks) {
    uint8_t buf[GH2_CEXT_MAX_VAL];
    uint32_t len = 0;
    struct gh2_key k = comp_key(ino, chunk_off);
    int r = gh2_btree_lookup(&fs->dev, root, &k, buf, sizeof(buf), &len);
    if (r) return r;
    return gh2_cext_decode(buf, len, hdr, blocks, GH2_CEXT_MAX_BLOCKS);
}

/* odczytaj chunk `chunk_off` pliku ino do rawbuf (>= GH2_CHUNK_BYTES), wypelniajac *raw_len_out
 * waznymi bajtami; ogon poza raw_len wyzerowany. Dziura -> raw_len 0 (zera). Niezgodnosc csum -> -EIO. */
static int chunk_read(struct gh2_fs *fs, const struct gh2_bptr *root, uint64_t ino,
                      uint64_t chunk_off, uint8_t *rawbuf, uint32_t *raw_len_out) {
    struct gh2_cext_hdr hdr;
    uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
    int r = comp_lookup_at(fs, root, ino, chunk_off, &hdr, blocks);
    if (r == -ENOENT) {
        memset(rawbuf, 0, GH2_CHUNK_BYTES);
        *raw_len_out = 0;
        return 0;
    }
    if (r) return r;
    if (hdr.raw_len > GH2_CHUNK_BYTES || hdr.comp_len > GH2_CHUNK_BYTES) return -EIO;

    /* sklej comp_len bajtow z blokow */
    uint8_t cbuf[GH2_CHUNK_BYTES];
    uint32_t got = 0;
    for (uint16_t i = 0; i < hdr.nblocks; i++) {
        uint8_t blk[GH2_DATA_BLK];
        r = gh_disk_read(&fs->dev, blocks[i], blk);
        if (r) return r;
        uint32_t take = hdr.comp_len - got;
        if (take > GH2_DATA_BLK) take = GH2_DATA_BLK;
        memcpy(cbuf + got, blk, take);
        got += take;
    }
    if (got != hdr.comp_len) return -EIO;
    if (gh_crc32(cbuf, hdr.comp_len) != hdr.csum) return -EIO;

    memset(rawbuf, 0, GH2_CHUNK_BYTES);
    if (hdr.comp_algo == 1) {
        uLongf dl = hdr.raw_len;
        if (uncompress(rawbuf, &dl, cbuf, hdr.comp_len) != Z_OK || dl != hdr.raw_len)
            return -EIO;
    } else {
        if (hdr.comp_len != hdr.raw_len) return -EIO;
        memcpy(rawbuf, cbuf, hdr.raw_len);
    }
    *raw_len_out = hdr.raw_len;
    return 0;
}

/* zapisz chunk `chunk_off` pliku ino z rawbuf[0..raw_len) (CoW). Stare bloki -> defer_free.
 * Probuje zlib; jesli skompresowane mieszcza sie w MNIEJ blokach niz raw -> zapisuje skompresowane,
 * inaczej raw (comp_algo=0). Alokuje bloki pojedynczo (nieciagle). */
static int chunk_write(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino,
                       uint64_t chunk_off, const uint8_t *rawbuf, uint32_t raw_len) {
    if (raw_len == 0 || raw_len > GH2_CHUNK_BYTES) return -EINVAL;

    /* stary chunk (do CoW free) */
    struct gh2_cext_hdr ohdr;
    uint64_t oblocks[GH2_CEXT_MAX_BLOCKS];
    int have_old = (comp_lookup_at(fs, root, ino, chunk_off, &ohdr, oblocks) == 0);

    uint32_t raw_nblocks = (raw_len + GH2_DATA_BLK - 1) / GH2_DATA_BLK;

    /* sprobuj kompresji zlib (bufor wg compressBound — worst case dla nieskompresowalnych) */
    uint8_t comp[GH2_CHUNK_BYTES + GH2_CHUNK_BYTES / 1000 + 64];
    uLongf clen = sizeof(comp);
    int comp_ok = (compress2(comp, &clen, rawbuf, raw_len, Z_DEFAULT_COMPRESSION) == Z_OK);
    uint32_t comp_nblocks = comp_ok ? (uint32_t)((clen + GH2_DATA_BLK - 1) / GH2_DATA_BLK) : raw_nblocks;

    /* wybierz: skompresowane tylko gdy oszczedza >=1 blok */
    uint8_t comp_algo;
    const uint8_t *src;
    uint32_t comp_len, nblocks;
    if (comp_ok && comp_nblocks < raw_nblocks) {
        comp_algo = 1; src = comp; comp_len = (uint32_t)clen; nblocks = comp_nblocks;
    } else {
        comp_algo = 0; src = rawbuf; comp_len = raw_len; nblocks = raw_nblocks;
    }
    if (nblocks > GH2_CEXT_MAX_BLOCKS) return -EIO;   /* nie powinno: comp/raw <= chunk */

    /* alokuj nblocks pojedynczo i zapisz (ostatni blok dopelniony zerami) */
    struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
    uint64_t nblk[GH2_CEXT_MAX_BLOCKS];
    uint16_t alloced = 0;
    int r = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        r = a.alloc(a.ctx, &nblk[i]);
        if (r) goto undo_alloc;
        alloced++;
        uint8_t blk[GH2_DATA_BLK];
        uint32_t off = i * GH2_DATA_BLK;
        uint32_t take = comp_len - off;
        if (take > GH2_DATA_BLK) take = GH2_DATA_BLK;
        memcpy(blk, src + off, take);
        if (take < GH2_DATA_BLK) memset(blk + take, 0, GH2_DATA_BLK - take);
        r = gh_disk_write(&fs->dev, nblk[i], blk);
        if (r) goto undo_alloc;
    }

    struct gh2_cext_hdr nhdr;
    memset(&nhdr, 0, sizeof(nhdr));
    nhdr.comp_algo = comp_algo;
    nhdr.flags = 0;
    nhdr.nblocks = (uint16_t)nblocks;
    nhdr.raw_len = raw_len;
    nhdr.comp_len = comp_len;
    nhdr.csum = gh_crc32(src, comp_len);

    uint8_t ebuf[GH2_CEXT_MAX_VAL];
    uint32_t elen = cext_encode(&nhdr, nblk, ebuf);
    struct gh2_key k = comp_key(ino, chunk_off);
    r = fs_insert(fs, root, &k, ebuf, elen);
    if (r) goto undo_alloc;

    /* stare bloki chunku CoW -> defer_free */
    if (have_old) {
        for (uint16_t i = 0; i < ohdr.nblocks; i++)
            if (oblocks[i]) a.free(a.ctx, oblocks[i]);
    }
    return 0;

undo_alloc:
    for (uint16_t i = 0; i < alloced; i++) a.free(a.ctx, nblk[i]);   /* nowe -> defer (rollback cofnie) */
    return r;
}

/* zwolnij WSZYSTKIE chunk extenty pliku ino (bloki defer-free + usun itemy) w lokalnym root. */
struct free_comp_ctx { uint64_t *offs; uint64_t *blks; uint16_t *nblk; uint32_t n, cap; int rc; };
static int collect_comp_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct free_comp_ctx *c = ctx;
    if (key->type != GH2_EXTENT_COMP) return 0;
    struct gh2_cext_hdr hdr;
    uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
    if (gh2_cext_decode(val, len, &hdr, blocks, GH2_CEXT_MAX_BLOCKS)) return 0;
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 32;
        uint64_t *no = realloc(c->offs, (size_t)nc * sizeof(uint64_t));
        uint64_t *nb = realloc(c->blks, (size_t)nc * GH2_CEXT_MAX_BLOCKS * sizeof(uint64_t));
        uint16_t *nn = realloc(c->nblk, (size_t)nc * sizeof(uint16_t));
        if (!no || !nb || !nn) { free(no ? no : c->offs); free(nb ? nb : c->blks);
                                 free(nn ? nn : c->nblk);
                                 c->offs = NULL; c->blks = NULL; c->nblk = NULL;
                                 c->rc = -ENOMEM; return -ENOMEM; }
        c->offs = no; c->blks = nb; c->nblk = nn; c->cap = nc;
    }
    c->offs[c->n] = key->offset;
    c->nblk[c->n] = hdr.nblocks;
    memcpy(&c->blks[(size_t)c->n * GH2_CEXT_MAX_BLOCKS], blocks,
           (size_t)hdr.nblocks * sizeof(uint64_t));
    c->n++;
    return 0;
}

static int gh2_inode_free_chunks(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino) {
    struct free_comp_ctx c; memset(&c, 0, sizeof(c));
    struct gh2_key min = comp_key(ino, 0);
    struct gh2_key max = comp_key(ino, UINT64_MAX);
    int r = gh2_btree_iterate_range(&fs->dev, root, &min, &max, collect_comp_cb, &c);
    if (r) { free(c.offs); free(c.blks); free(c.nblk); return c.rc ? c.rc : r; }

    struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
    for (uint32_t i = 0; i < c.n; i++) {
        struct gh2_key k = comp_key(ino, c.offs[i]);
        r = fs_delete(fs, root, &k);
        if (r) { free(c.offs); free(c.blks); free(c.nblk); return r; }
        for (uint16_t j = 0; j < c.nblk[i]; j++) {
            uint64_t b = c.blks[(size_t)i * GH2_CEXT_MAX_BLOCKS + j];
            if (b) a.free(a.ctx, b);
        }
    }
    free(c.offs); free(c.blks); free(c.nblk);
    return 0;
}

/* ============================ DIR_ITEM pack/unpack ============================ */
/* Wartosc DIR_ITEM = sekwencja wpisow [gh2_dirent | name[name_len]] doklejanych. Kolizje
 * hash -> wiele wpisow w jednej wartosci. Lookup skanuje po dokladna nazwe. */

/* znajdz wpis o nazwie w spakowanej wartosci; zwraca offset wpisu lub -1 */
static long dir_find_entry(const uint8_t *val, uint32_t len, const char *name, uint16_t nlen) {
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, val + off, sizeof(de));
        uint32_t enlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + enlen;
        if (off + rec > len) break;                /* uszkodzone — przerwij */
        if (enlen == nlen && memcmp(val + off + sizeof(struct gh2_dirent), name, nlen) == 0)
            return (long)off;
        off += rec;
    }
    return -1;
}

/* rdzen: dodaj wpis pod JAWNYM hashem (test moze wymusic kolizje). Operuje na lokalnym root. */
static int dir_add_entry_hashed(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t parent,
                                uint64_t hash, const char *name, uint16_t nlen,
                                uint64_t ino, uint8_t ftype) {
    struct gh2_key k = dir_key(parent, hash);

    uint8_t cur[GH2_FS_MAX_VAL];
    uint32_t curlen = 0;
    int r = gh2_btree_lookup(&fs->dev, root, &k, cur, sizeof(cur), &curlen);
    if (r && r != -ENOENT) return r;
    if (r == -ENOENT) curlen = 0;

    if (curlen > 0 && dir_find_entry(cur, curlen, name, nlen) >= 0)
        return -EEXIST;

    uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
    if (curlen + rec > GH2_FS_MAX_VAL) return -ENOSPC;   /* za duzo kolizji/dlugie nazwy */

    uint8_t nv[GH2_FS_MAX_VAL];
    if (curlen) memcpy(nv, cur, curlen);
    struct gh2_dirent de;
    memset(&de, 0, sizeof(de));
    de.ino = ino; de.ftype = ftype; de._pad = 0; de.name_len = nlen;
    memcpy(nv + curlen, &de, sizeof(de));
    memcpy(nv + curlen + sizeof(de), name, nlen);

    return fs_insert(fs, root, &k, nv, curlen + rec);
}

/* dodaj wpis (name, ino, ftype) do katalogu parent. -EEXIST gdy nazwa juz jest. */
static int dir_add_entry(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t parent,
                         const char *name, uint16_t nlen, uint64_t ino, uint8_t ftype) {
    return dir_add_entry_hashed(fs, root, parent, fnv1a64(name, nlen), name, nlen, ino, ftype);
}

/* rdzen: usun wpis (name) z DIR_ITEM pod JAWNYM hashem. Gdy to ostatni wpis -> usun caly item;
 * inaczej przepakuj. -ENOENT gdy brak. */
static int dir_remove_entry_hashed(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t parent,
                                   uint64_t hash, const char *name, uint16_t nlen) {
    struct gh2_key k = dir_key(parent, hash);

    uint8_t cur[GH2_FS_MAX_VAL];
    uint32_t curlen = 0;
    int r = gh2_btree_lookup(&fs->dev, root, &k, cur, sizeof(cur), &curlen);
    if (r) return r;                               /* -ENOENT przepuszczone */
    long off = dir_find_entry(cur, curlen, name, nlen);
    if (off < 0) return -ENOENT;

    struct gh2_dirent de;
    memcpy(&de, cur + off, sizeof(de));
    uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + de.name_len;

    if (curlen == rec) {
        /* jedyny wpis -> usun caly DIR_ITEM */
        return fs_delete(fs, root, &k);
    }
    /* przepakuj: usun [off, off+rec) */
    uint8_t nv[GH2_FS_MAX_VAL];
    uint32_t nlen2 = 0;
    if (off > 0) { memcpy(nv, cur, (size_t)off); nlen2 = (uint32_t)off; }
    uint32_t tail_off = (uint32_t)off + rec;
    if (tail_off < curlen) {
        memcpy(nv + nlen2, cur + tail_off, curlen - tail_off);
        nlen2 += curlen - tail_off;
    }
    return fs_insert(fs, root, &k, nv, nlen2);
}

/* usun wpis (name) z katalogu parent (hash = fnv1a64 nazwy). -ENOENT gdy brak. */
static int dir_remove_entry(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t parent,
                            const char *name, uint16_t nlen) {
    return dir_remove_entry_hashed(fs, root, parent, fnv1a64(name, nlen), name, nlen);
}

/* rdzen: lookup pod JAWNYM hashem (z podanego root). */
static int dir_lookup_hashed_at(struct gh2_fs *fs, const struct gh2_bptr *root, uint64_t parent,
                                uint64_t hash, const char *name, uint16_t nlen,
                                uint64_t *out_ino, uint8_t *out_ftype) {
    struct gh2_key k = dir_key(parent, hash);
    uint8_t cur[GH2_FS_MAX_VAL];
    uint32_t curlen = 0;
    int r = gh2_btree_lookup(&fs->dev, root, &k, cur, sizeof(cur), &curlen);
    if (r) return r;                               /* -ENOENT przepuszczone */
    long off = dir_find_entry(cur, curlen, name, nlen);
    if (off < 0) return -ENOENT;
    struct gh2_dirent de;
    memcpy(&de, cur + off, sizeof(de));
    if (out_ino) *out_ino = de.ino;
    if (out_ftype) *out_ftype = de.ftype;
    return 0;
}

/* znajdz wpis nazwa w katalogu parent -> ino, ftype (z podanego root). -ENOENT gdy brak. */
static int dir_lookup_at(struct gh2_fs *fs, const struct gh2_bptr *root, uint64_t parent,
                         const char *name, uint16_t nlen,
                         uint64_t *out_ino, uint8_t *out_ftype) {
    return dir_lookup_hashed_at(fs, root, parent, fnv1a64(name, nlen), name, nlen,
                                out_ino, out_ftype);
}

/* warianty na zatwierdzonym fs->fs_root (odczyty poza operacja: resolve/readdir/ancestor) */
static int dir_lookup(struct gh2_fs *fs, uint64_t parent, const char *name, uint16_t nlen,
                      uint64_t *out_ino, uint8_t *out_ftype) {
    return dir_lookup_at(fs, &fs->fs_root, parent, name, nlen, out_ino, out_ftype);
}

/* Test seam: wstaw/odczytaj wpis pod WYMUSZONYM hashem (forsowanie kolizji DIR_ITEM bez
 * polegania na naturalnej kolizji FNV). Dowodzi pakowania wielu wpisow w jeden DIR_ITEM. */
int gh2_fs_test_dir_add(struct gh2_fs *fs, uint64_t parent, uint64_t hash,
                        const char *name, uint16_t nlen, uint64_t ino, uint8_t ftype) {
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
    int r = dir_add_entry_hashed(fs, &root, parent, hash, name, nlen, ino, ftype);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    fs->fs_root = root;
    return 0;
}
int gh2_fs_test_dir_lookup(struct gh2_fs *fs, uint64_t parent, uint64_t hash,
                           const char *name, uint16_t nlen, uint64_t *out_ino) {
    return dir_lookup_hashed_at(fs, &fs->fs_root, parent, hash, name, nlen, out_ino, NULL);
}

/* Test seam: usun (REALNY) wpis `name` z katalogu `parent` BEZ zwalniania i-wezla — uzywane
 * przez fsck --repair test do tworzenia sieroty (INODE_ITEM bez osiagalnego DIR_ITEM). */
int gh2_fs_test_dir_remove(struct gh2_fs *fs, uint64_t parent, const char *name, uint16_t nlen) {
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
    int r = dir_remove_entry(fs, &root, parent, name, nlen);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    fs->fs_root = root;
    return 0;
}

/* Test seam: nadpisz pole nlink i-wezla `ino` na `nlink` (wymuszenie zlej wartosci dla fsck). */
int gh2_fs_test_set_nlink(struct gh2_fs *fs, uint64_t ino, uint32_t nlink) {
    struct gh2_inode in;
    int r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    in.nlink = nlink;
    return fs_commit_one_inode(fs, ino, &in);
}

/* ============================ XATTR_ITEM pack/unpack ============================ */
/* Wartosc XATTR_ITEM = sekwencja wpisow [xattr_hdr | name[name_len] | value[value_len]]
 * doklejanych. Kolizje hash -> wiele wpisow w jednej wartosci. Lookup po dokladnej nazwie.
 * Naglowek deterministyczny: pola u16/u32 kopiowane, BRAK paddingu (sizeof == 6). */

struct gh2_xattr_hdr {
    uint16_t name_len;
    uint32_t value_len;
} __attribute__((packed));
_Static_assert(sizeof(struct gh2_xattr_hdr) == 6, "xattr hdr bez paddingu");

static struct gh2_key xattr_key(uint64_t ino, uint64_t name_hash) {
    struct gh2_key k; memset(&k, 0, sizeof(k));
    k.objectid = ino; k.type = GH2_XATTR_ITEM; k.offset = name_hash;
    return k;
}

/* znajdz wpis o nazwie w spakowanej wartosci; zwraca offset wpisu lub -1.
 * Wypelnia (gdy znaleziono i wskazniki != NULL) *vlen_out i *voff_out (offset value w val). */
static long xattr_find_entry(const uint8_t *val, uint32_t len, const char *name, uint16_t nlen,
                             uint32_t *vlen_out, uint32_t *voff_out) {
    uint32_t off = 0;
    while (off + sizeof(struct gh2_xattr_hdr) <= len) {
        struct gh2_xattr_hdr h;
        memcpy(&h, val + off, sizeof(h));
        uint32_t rec = (uint32_t)sizeof(h) + h.name_len + h.value_len;
        if (off + rec > len) break;               /* uszkodzone — przerwij */
        if (h.name_len == nlen &&
            memcmp(val + off + sizeof(h), name, nlen) == 0) {
            if (vlen_out) *vlen_out = h.value_len;
            if (voff_out) *voff_out = off + (uint32_t)sizeof(h) + h.name_len;
            return (long)off;
        }
        off += rec;
    }
    return -1;
}

/* zwolnij WSZYSTKIE itemy xattr (ino, GH2_XATTR_ITEM, *) w lokalnym root (range-scan + delete).
 * Uzywane przez unlink (nlink->0) i rmdir — brak osieroconych xattr. */
struct free_xattr_ctx { uint64_t *offs; uint32_t n, cap; int rc; };
static int collect_xattr_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)val; (void)len;
    struct free_xattr_ctx *c = ctx;
    if (key->type != GH2_XATTR_ITEM) return 0;
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 16;
        uint64_t *no = realloc(c->offs, (size_t)nc * sizeof(uint64_t));
        if (!no) { free(c->offs); c->offs = NULL; c->rc = -ENOMEM; return -ENOMEM; }
        c->offs = no; c->cap = nc;
    }
    c->offs[c->n++] = key->offset;
    return 0;
}

static int gh2_inode_free_xattrs(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino) {
    struct free_xattr_ctx c; memset(&c, 0, sizeof(c));
    struct gh2_key min = xattr_key(ino, 0);
    struct gh2_key max = xattr_key(ino, UINT64_MAX);
    int r = gh2_btree_iterate_range(&fs->dev, root, &min, &max, collect_xattr_cb, &c);
    if (r) { free(c.offs); return c.rc ? c.rc : r; }
    for (uint32_t i = 0; i < c.n; i++) {
        struct gh2_key k = xattr_key(ino, c.offs[i]);
        r = fs_delete(fs, root, &k);
        if (r) { free(c.offs); return r; }
    }
    free(c.offs);
    return 0;
}

/* zwolnij WSZYSTKIE itemy DIR_ITEM (ino, GH2_DIR_ITEM, *) w lokalnym root (range-scan + delete).
 * Uzywane przez fsck --repair przy zwalnianiu sieroty bedacej katalogiem (jego wlasne wpisy). */
struct free_dir_ctx { uint64_t *offs; uint32_t n, cap; int rc; };
static int collect_dir_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)val; (void)len;
    struct free_dir_ctx *c = ctx;
    if (key->type != GH2_DIR_ITEM) return 0;
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 16;
        uint64_t *no = realloc(c->offs, (size_t)nc * sizeof(uint64_t));
        if (!no) { free(c->offs); c->offs = NULL; c->rc = -ENOMEM; return -ENOMEM; }
        c->offs = no; c->cap = nc;
    }
    c->offs[c->n++] = key->offset;
    return 0;
}
static int gh2_inode_free_dir_items(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino) {
    struct free_dir_ctx c; memset(&c, 0, sizeof(c));
    struct gh2_key min = dir_key(ino, 0);
    struct gh2_key max = dir_key(ino, UINT64_MAX);
    int r = gh2_btree_iterate_range(&fs->dev, root, &min, &max, collect_dir_cb, &c);
    if (r) { free(c.offs); return c.rc ? c.rc : r; }
    for (uint32_t i = 0; i < c.n; i++) {
        struct gh2_key k = dir_key(ino, c.offs[i]);
        r = fs_delete(fs, root, &k);
        if (r) { free(c.offs); return r; }
    }
    free(c.offs);
    return 0;
}

/* Test seam: wstaw/odczytaj xattr pod WYMUSZONYM hashem (forsowanie kolizji XATTR_ITEM bez
 * polegania na naturalnej kolizji FNV). Dowodzi pakowania wielu wpisow w jeden XATTR_ITEM.
 * Atomowo per-op (savepoint/rollback). Zadeklarowane w gh2_fs.h tylko gdy testy ich potrzebuja. */
int gh2_fs_test_xattr_set_hashed(struct gh2_fs *fs, uint64_t ino, uint64_t hash,
                                 const char *name, const void *value, size_t size) {
    size_t nlen_sz = strlen(name);
    if (nlen_sz > UINT16_MAX) return -ERANGE;
    if (size > UINT32_MAX) return -E2BIG;
    uint16_t nlen = (uint16_t)nlen_sz;
    struct gh2_key k = xattr_key(ino, hash);

    uint8_t cur[GH2_FS_MAX_VAL]; uint32_t curlen = 0;
    int r = gh2_btree_lookup(&fs->dev, &fs->fs_root, &k, cur, sizeof(cur), &curlen);
    if (r && r != -ENOENT) return r;
    if (r == -ENOENT) curlen = 0;
    if (curlen > 0 && xattr_find_entry(cur, curlen, name, nlen, NULL, NULL) >= 0) return -EEXIST;

    uint32_t newrec = (uint32_t)sizeof(struct gh2_xattr_hdr) + nlen + (uint32_t)size;
    if ((uint64_t)curlen + newrec > GH2_FS_MAX_VAL) return -E2BIG;
    uint8_t nv[GH2_FS_MAX_VAL]; uint32_t nl = curlen;
    if (curlen) memcpy(nv, cur, curlen);
    struct gh2_xattr_hdr h; h.name_len = nlen; h.value_len = (uint32_t)size;
    memcpy(nv + nl, &h, sizeof(h));
    memcpy(nv + nl + sizeof(h), name, nlen);
    if (size) memcpy(nv + nl + sizeof(h) + nlen, value, size);
    nl += newrec;

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
    r = fs_insert(fs, &root, &k, nv, nl);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    fs->fs_root = root;
    return 0;
}
ssize_t gh2_fs_test_xattr_get_hashed(struct gh2_fs *fs, uint64_t ino, uint64_t hash,
                                     const char *name, void *buf, size_t size) {
    size_t nlen_sz = strlen(name);
    if (nlen_sz > UINT16_MAX) return -ERANGE;
    uint16_t nlen = (uint16_t)nlen_sz;
    struct gh2_key k = xattr_key(ino, hash);
    uint8_t cur[GH2_FS_MAX_VAL]; uint32_t curlen = 0;
    int r = gh2_btree_lookup(&fs->dev, &fs->fs_root, &k, cur, sizeof(cur), &curlen);
    if (r == -ENOENT) return -ENODATA;
    if (r) return r;
    uint32_t vlen = 0, voff = 0;
    long off = xattr_find_entry(cur, curlen, name, nlen, &vlen, &voff);
    if (off < 0) return -ENODATA;
    if (size == 0) return (ssize_t)vlen;
    if (size < vlen) return -ERANGE;
    memcpy(buf, cur + voff, vlen);
    return (ssize_t)vlen;
}

/* ============================ format ============================ */

int gh2_fs_format_key(struct gh_dev *dev, uint64_t total_blocks, uint32_t flags,
                      const char *passphrase) {
    /* v2enc: opcjonalne szyfrowanie. Wyprowadz klucz, ustaw dev->cipher PRZED tworzeniem drzew,
     * dolaczajac sol+weryfikator do SB (pisany RAW -> nieszyfrowany). */
    struct gh_cipher *cipher = NULL;
    uint8_t enc_salt[16];
    uint8_t enc_verifier[32];
    if (passphrase != NULL) {
        if (passphrase[0] == '\0') return -EINVAL;          /* puste haslo = blad (jak v1) */
        cipher = malloc(sizeof(*cipher));
        if (!cipher) return -ENOMEM;
        if (gh_crypto_random(enc_salt, sizeof(enc_salt))) { free(cipher); return -EIO; }
        if (gh_crypto_derive(passphrase, enc_salt, GH2_KDF_ITERS, cipher->key)) {
            gh_crypto_wipe(cipher); free(cipher); return -EIO;
        }
        gh_crypto_verifier(cipher->key, enc_salt, enc_verifier);
        flags |= GH2_SB_ENCRYPTED;
    }

    int r = gh2_format(dev, total_blocks, flags);
    if (r) { if (cipher) { gh_crypto_wipe(cipher); free(cipher); } return r; }

    struct gh2_superblock sb;
    r = gh2_mount(dev, &sb);
    if (r) { if (cipher) { gh_crypto_wipe(cipher); free(cipher); } return r; }

    /* dolacz sol+weryfikator do SB i wlacz cipher PRZED tworzeniem drzew (wezly+dane szyfrowane).
     * SB utrwalany przez gh2_commit_super (RAW pwrite) -> nieszyfrowany, trzyma sol. */
    if (cipher) {
        memcpy(sb.enc_salt, enc_salt, sizeof(sb.enc_salt));
        memcpy(sb.enc_verifier, enc_verifier, sizeof(sb.enc_verifier));
        dev->cipher = cipher;
    }

    /* alokator transakcyjny na czas formatu (mapa: wszystko wolne poza SB) */
    struct gh2_space space;
    r = gh2_space_init(&space, total_blocks);
    if (r) return r;
    struct gh2_txn_alloc t;
    r = gh2_txn_alloc_init(&t, &space);
    if (r) { gh2_space_destroy(&space); return r; }
    t.dup_meta = (sb.flags & GH2_SB_DUP_META) ? 1 : 0;   /* v2.8: DUP metadane z flagi SB */
    struct gh2_alloc a = gh2_txn_alloc_vtable(&t);

    /* utworz pusty fs-tree */
    struct gh2_bptr root;
    r = gh2_btree_create(dev, &a, GH2_ROOT_INO, sb.generation, &root);
    if (r) goto out;

    /* i-wezel korzenia: ino 1, DIR, nlink 2 (. + ..), mode 0755 */
    {
        struct gh2_inode ri;
        memset(&ri, 0, sizeof(ri));
        ri.type = GH2_FT_DIR;
        ri.mode = 0755;
        ri.nlink = 2;
        ri.size = 0;
        uint64_t now = (uint64_t)time(NULL);
        ri.atime = ri.mtime = ri.ctime = now;
        ri.generation = sb.generation;

        uint8_t ibuf[sizeof(struct gh2_inode)];
        inode_encode(&ri, ibuf);
        struct gh2_key ik = inode_key(GH2_ROOT_INO);
        struct gh2_bptr nroot;
        r = gh2_btree_insert(dev, &a, &root, sb.generation, &ik, ibuf, sizeof(ibuf), &nroot);
        if (r) goto out;
        root = nroot;
    }

    /* utworz DRZEWO KORZENI z JEDNYM wpisem: (1, GH2_ROOT_ITEM, 0) -> {fs_root=root, "default"} */
    struct gh2_bptr root_tree;
    r = gh2_btree_create(dev, &a, GH2_SUBVOL_DEFAULT, sb.generation, &root_tree);
    if (r) goto out;
    {
        struct gh2_subvol_item sv;
        memset(&sv, 0, sizeof(sv));
        sv.fs_root = root;
        sv.flags = 0;
        memcpy(sv.name, "default", 7);

        uint8_t svbuf[sizeof(struct gh2_subvol_item)];
        subvol_encode(&sv, svbuf);
        struct gh2_key rk = root_item_key(GH2_SUBVOL_DEFAULT);
        struct gh2_bptr nrt;
        r = gh2_btree_insert(dev, &a, &root_tree, sb.generation, &rk, svbuf, sizeof(svbuf), &nrt);
        if (r) goto out;
        root_tree = nrt;
    }

    /* commit: fsync danych -> SB.root_tree=drzewo korzeni + reserved[0]=next_ino(2) -> commit_super */
    if (fsync(dev->fd)) { r = -EIO; goto out; }
    sb.root_tree = root_tree;
    sb.reserved[0] = GH2_ROOT_INO + 1;             /* next_ino = 2 */
    r = gh2_commit_super(dev, &sb);
    if (r) goto out;
    r = gh2_txn_alloc_commit(&t);

out:
    gh2_txn_alloc_destroy(&t);
    gh2_space_destroy(&space);
    if (r && cipher) {   /* blad formatu: nie zostawiaj polowicznego ciphera na dev */
        dev->cipher = NULL;
        gh_crypto_wipe(cipher);
        free(cipher);
    }
    /* sukces: cipher (jesli byl) zostaje na dev->cipher — caller zarzadza wipe. */
    return r;
}

/* wrapper bez szyfrowania (zachowanie sprzed v2enc). */
int gh2_fs_format(struct gh_dev *dev, uint64_t total_blocks, uint32_t flags) {
    return gh2_fs_format_key(dev, total_blocks, flags, NULL);
}

/* ============================ mount / unmount ============================ */

/* odczytaj wpis subwolumenu (subvol_id) z drzewa korzeni. -ENOENT gdy brak. */
static int subvol_lookup(struct gh2_fs *fs, uint64_t subvol_id, struct gh2_subvol_item *out) {
    uint8_t buf[sizeof(struct gh2_subvol_item)];
    uint32_t len = 0;
    struct gh2_key k = root_item_key(subvol_id);
    int r = gh2_btree_lookup(&fs->dev, &fs->root_tree, &k, buf, sizeof(buf), &len);
    if (r) return r;
    return subvol_decode(buf, len, out);
}

int gh2_fs_mount_key(struct gh2_fs *fs, struct gh_dev *dev, const char *passphrase) {
    memset(fs, 0, sizeof(*fs));
    fs->dev = *dev;
    fs->dev.cipher = NULL;
    int r = gh2_mount(&fs->dev, &fs->sb);    /* SB raw (blok 0/1) -> dziala bez ciphera */
    if (r) return r;

    /* v2enc: jesli kontener zaszyfrowany, wyprowadz+zweryfikuj klucz i wlacz cipher PRZED
     * jakimkolwiek odczytem wezlow/danych (mark-sweep nizej deszyfruje). */
    if (fs->sb.flags & GH2_SB_ENCRYPTED) {
        if (!passphrase || passphrase[0] == '\0') return -EACCES;
        struct gh_cipher *c = malloc(sizeof(*c));
        if (!c) return -ENOMEM;
        if (gh_crypto_derive(passphrase, fs->sb.enc_salt, GH2_KDF_ITERS, c->key)) {
            gh_crypto_wipe(c); free(c); return -EIO;
        }
        uint8_t v[32];
        gh_crypto_verifier(c->key, fs->sb.enc_salt, v);
        if (memcmp(v, fs->sb.enc_verifier, 32) != 0) {   /* zle haslo */
            gh_crypto_wipe(c); free(c); return -EACCES;
        }
        fs->dev.cipher = c;
    }

    /* sb.root_tree wskazuje DRZEWO KORZENI; aktywny subwolumen (domyslnie 1) -> jego fs_root */
    fs->root_tree = fs->sb.root_tree;
    fs->active_subvol = GH2_SUBVOL_DEFAULT;
    struct gh2_subvol_item sv;
    r = subvol_lookup(fs, fs->active_subvol, &sv);
    if (r) goto fail_cipher;
    fs->fs_root = sv.fs_root;
    fs->next_ino = fs->sb.reserved[0];
    if (fs->next_ino < GH2_ROOT_INO + 1) fs->next_ino = GH2_ROOT_INO + 1;

    r = gh2_space_init(&fs->space, fs->sb.total_blocks);
    if (r) goto fail_cipher;
    /* mark-sweep refcountow: zlicz referencje ze WSZYSTKICH subwolumenow drzewa korzeni
     * (+ wezly samego drzewa korzeni). Dla 1 subwolumenu == dotychczasowy build_from_tree. */
    r = gh2_refmap_build_from_roots(&fs->dev, &fs->space, &fs->root_tree);
    if (r) { gh2_space_destroy(&fs->space); goto fail_cipher; }
    r = gh2_txn_alloc_init(&fs->alloc, &fs->space);
    if (r) { gh2_space_destroy(&fs->space); goto fail_cipher; }
    fs->alloc.dup_meta = (fs->sb.flags & GH2_SB_DUP_META) ? 1 : 0;   /* v2.8: DUP z flagi SB */
    fs->compress = (fs->sb.flags & GH2_SB_COMPRESS) ? 1 : 0;          /* v2.9: sciezka chunk extentow */
    return 0;

fail_cipher:
    if (fs->dev.cipher) {   /* nieudany mount zaszyfrowanego -> nie wyciekaj klucza */
        gh_crypto_wipe(fs->dev.cipher);
        free(fs->dev.cipher);
        fs->dev.cipher = NULL;
    }
    return r;
}

/* wrapper bez klucza: zaszyfrowany kontener -> -EACCES. */
int gh2_fs_mount(struct gh2_fs *fs, struct gh_dev *dev) {
    return gh2_fs_mount_key(fs, dev, NULL);
}

void gh2_fs_unmount(struct gh2_fs *fs) {
    gh2_txn_alloc_destroy(&fs->alloc);
    gh2_space_destroy(&fs->space);
    if (fs->dev.cipher) {   /* v2enc: wymaz klucz przy odmontowaniu (brak wycieku) */
        gh_crypto_wipe(fs->dev.cipher);
        free(fs->dev.cipher);
        fs->dev.cipher = NULL;
    }
    memset(&fs->fs_root, 0, sizeof(fs->fs_root));
    memset(&fs->root_tree, 0, sizeof(fs->root_tree));
}

/* ============================ commit (atomowy, crash-safe) ============================ */
/* NIEZMIENNIK ATOMOWOSCI: superblok pisany OSTATNI, po trwalosci wszystkich nowych blokow.
 * Kazdy prefiks zapisow commitu pozostawia stan STARY albo NOWY — nigdy czesciowy:
 *   - awaria przed faza 2 -> stary SB wazny -> STARY stan (nowe bloki nieosiagalne);
 *   - awaria w trakcie fazy 2 (rozdarcie SB) -> drugi slot (stary) wazny -> STARY stan;
 *   - awaria po fazie 2 -> NOWY trwaly stan.
 * Dowiedzione sweepem awarii (tests/test_v2crash.c) dla KAZDEGO N. */
int gh2_fs_commit(struct gh2_fs *fs) {
    /* --- Faza 0: zapisz aktywny fs_root do wpisu (active_subvol, GH2_ROOT_ITEM, 0) w DRZEWIE
       KORZENI (CoW -> nowy root_tree). Czesc tej samej transakcji (savepoint/rollback): przy
       bledzie cofnij i NIE podmieniaj SB (stan STARY trwa). */
    struct gh2_subvol_item sv;
    int r = subvol_lookup(fs, fs->active_subvol, &sv);
    if (r) return r;
    sv.fs_root = fs->fs_root;            /* zaktualizuj korzen fs-tree subwolumenu */

    struct gh2_bptr root_tree = fs->root_tree;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
    {
        uint8_t svbuf[sizeof(struct gh2_subvol_item)];
        subvol_encode(&sv, svbuf);
        struct gh2_key rk = root_item_key(fs->active_subvol);
        struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
        struct gh2_bptr nrt;
        r = gh2_btree_insert(&fs->dev, &a, &root_tree, fs->sb.generation,
                             &rk, svbuf, sizeof(svbuf), &nrt);
        if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
        root_tree = nrt;
    }

    /* --- Faza 1: bariera danych --- nowe bloki (wezly fs-tree + drzewa korzeni + bloki danych)
       trwale na dysku PRZED zapisem superbloku. */
    if (fsync(fs->dev.fd)) { gh2_txn_alloc_rollback(&fs->alloc, sp); return -EIO; }

    /* --- Faza 2: atomowa podmiana korzenia --- SB wskazuje NOWE drzewo korzeni. Zapis w slot
       gen&1 (ping-pong) z csum + read-back verify + fsync. JEDYNY punkt atomowy. */
    fs->sb.root_tree = root_tree;
    fs->sb.reserved[0] = fs->next_ino;
    r = gh2_commit_super(&fs->dev, &fs->sb);
    if (r) {                    /* SB nie podmieniony -> cofnij (stan STARY trwa) */
        gh2_txn_alloc_rollback(&fs->alloc, sp);
        return r;
    }
    fs->root_tree = root_tree;  /* utrwalone -> przyjmij nowy korzen drzewa korzeni */

    /* --- Faza 3: zwolnienie starych blokow CoW --- teraz nieosiagalne; tylko w pamieci
       (mark-sweep przy mount i tak odbuduje mape). Po udanej fazie 2 -> stan NOWY trwaly. */
    return gh2_txn_alloc_commit(&fs->alloc);
}

/* ============================ v2.7 (Task 3): snapshoty ============================ */

/* znajdz maksymalny istniejacy subvol_id w drzewie korzeni (do wyboru nastepnego). */
struct max_subvol_ctx { uint64_t max_id; };
static int max_subvol_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)val; (void)len;
    struct max_subvol_ctx *c = ctx;
    if (key->type != GH2_ROOT_ITEM) return 0;
    if (key->objectid > c->max_id) c->max_id = key->objectid;
    return 0;
}

/* sprawdz, czy nazwa subwolumenu juz istnieje (unikalnosc nazw snapshotow). */
struct name_exists_ctx { const char *name; int found; };
static int name_exists_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct name_exists_ctx *c = ctx;
    if (key->type != GH2_ROOT_ITEM) return 0;
    if (len != sizeof(struct gh2_subvol_item)) return 0;
    struct gh2_subvol_item sv;
    memcpy(&sv, val, sizeof(sv));
    sv.name[GH2_SUBVOL_NAME_MAX - 1] = '\0';
    if (strcmp(sv.name, c->name) == 0) { c->found = 1; return 1; }
    return 0;
}

/* inc refcount wszystkich blokow osiagalnych z fs_root (defer do commit): wezly + bloki danych.
 * Uzywa gh2_txn_alloc_defer_inc — zastosowane przy gh2_txn_alloc_commit; przy rollback porzucone. */
static int defer_inc_node_cb(uint64_t block, void *ctx) {
    struct gh2_txn_alloc *t = ctx;
    gh2_txn_alloc_defer_inc(t, block);
    return 0;
}
static int defer_inc_extent_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_txn_alloc *t = ctx;
    if (key->type == GH2_EXTENT_COMP) {
        /* v2.9: chunk extent -> inc refcount KAZDEGO bloku chunku (snapshot wspoldzieli bloki).
         * Bez tego snapshot skompresowanego pliku NIE inkrementuje rc -> premature-free przy CoW. */
        struct gh2_cext_hdr hdr;
        uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
        if (gh2_cext_decode(val, len, &hdr, blocks, GH2_CEXT_MAX_BLOCKS)) return 0;
        for (uint16_t i = 0; i < hdr.nblocks; i++)
            if (blocks[i]) gh2_txn_alloc_defer_inc(t, blocks[i]);
        return 0;
    }
    if (key->type != GH2_EXTENT_DATA) return 0;
    if (len != sizeof(struct gh2_extent)) return 0;
    struct gh2_extent e;
    memcpy(&e, val, sizeof(e));
    if (e.disk_block) gh2_txn_alloc_defer_inc(t, e.disk_block);
    if (e.dup_block)  gh2_txn_alloc_defer_inc(t, e.dup_block);
    return 0;
}
static int snapshot_defer_inc_all(struct gh2_fs *fs, const struct gh2_bptr *fs_root) {
    int r = gh2_btree_walk_nodes(&fs->dev, fs_root, defer_inc_node_cb, &fs->alloc);
    if (r) return r;
    return gh2_btree_iterate(&fs->dev, fs_root, defer_inc_extent_cb, &fs->alloc);
}

int gh2_fs_snapshot(struct gh2_fs *fs, const char *name) {
    if (!name || name[0] == '\0') return -EINVAL;
    if (strlen(name) >= GH2_SUBVOL_NAME_MAX) return -ENAMETOOLONG;

    /* nazwa musi byc unikalna (cb przerywa zwracajac 1 gdy znajdzie -> to NIE blad) */
    struct name_exists_ctx ne = { name, 0 };
    int r = gh2_btree_iterate(&fs->dev, &fs->root_tree, name_exists_cb, &ne);
    if (r < 0) return r;
    if (ne.found) return -EEXIST;

    /* wybierz nowy subvol_id = max istniejacy + 1 */
    struct max_subvol_ctx mc = { 0 };
    r = gh2_btree_iterate(&fs->dev, &fs->root_tree, max_subvol_cb, &mc);
    if (r) return r;
    uint64_t new_id = mc.max_id + 1;

    /* --- atomowosc per-op: lokalny root_tree + savepoint --- */
    struct gh2_bptr root_tree = fs->root_tree;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    /* 1) nowy wpis (new_id, GH2_ROOT_ITEM, 0) -> {fs_root = aktywny fs_root, name} */
    struct gh2_subvol_item sv;
    memset(&sv, 0, sizeof(sv));
    sv.fs_root = fs->fs_root;       /* TEN SAM bptr — wspoldzielenie */
    sv.flags = 0;
    memcpy(sv.name, name, strlen(name));

    {
        uint8_t svbuf[sizeof(struct gh2_subvol_item)];
        subvol_encode(&sv, svbuf);
        struct gh2_key rk = root_item_key(new_id);
        struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
        struct gh2_bptr nrt;
        r = gh2_btree_insert(&fs->dev, &a, &root_tree, fs->sb.generation,
                             &rk, svbuf, sizeof(svbuf), &nrt);
        if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
        root_tree = nrt;
    }

    /* 2) inc refcount WSZYSTKICH blokow osiagalnych z fs_root (defer; zastosowane przy commit).
     *    KRYTYCZNE: po commicie kazdy wspoldzielony blok ma rc==2 (default + snapshot). */
    r = snapshot_defer_inc_all(fs, &fs->fs_root);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    if (fs->alloc.oom) { gh2_txn_alloc_rollback(&fs->alloc, sp); return -ENOMEM; }

    /* sukces: przyjmij nowy root_tree i commituj (zapis SB + zastosowanie defer_inc/dec) */
    fs->root_tree = root_tree;
    return gh2_fs_commit(fs);
}

/* ---- lista subwolumenow ---- */
struct subvol_list_ctx { gh2_subvol_cb cb; void *user; int rc; };
static int subvol_list_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct subvol_list_ctx *c = ctx;
    if (key->type != GH2_ROOT_ITEM) return 0;
    if (len != sizeof(struct gh2_subvol_item)) return 0;
    struct gh2_subvol_item sv;
    memcpy(&sv, val, sizeof(sv));
    sv.name[GH2_SUBVOL_NAME_MAX - 1] = '\0';
    int r = c->cb(key->objectid, sv.name, c->user);
    if (r) { c->rc = r; return r; }
    return 0;
}
int gh2_fs_subvol_list(struct gh2_fs *fs, gh2_subvol_cb cb, void *ctx) {
    struct subvol_list_ctx c = { cb, ctx, 0 };
    int r = gh2_btree_iterate(&fs->dev, &fs->root_tree, subvol_list_cb, &c);
    if (r) return r;
    return c.rc;
}

/* ---- usun subwolumen: dec refcount blokow wylacznych (wspoldzielone zostaja) ---- */
/* dec refcount wszystkich blokow osiagalnych z fs_root subwolumenu (defer do commit):
 * wezly drzewa + bloki danych ekstentow. Bloki wspoldzielone (rc>1) tylko zejda o 1 (zostaja),
 * wylaczne (rc==1) zwolnione przy commit. Symetria do snapshot_defer_inc_all. */
static int defer_dec_node_cb(uint64_t block, void *ctx) {
    struct gh2_txn_alloc *t = ctx;
    gh2_txn_alloc_defer_dec(t, block);
    return 0;
}
static int defer_dec_extent_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_txn_alloc *t = ctx;
    if (key->type == GH2_EXTENT_COMP) {
        /* v2.9: chunk extent -> dec refcount KAZDEGO bloku chunku (delete zwalnia tylko wylaczne;
         * wspoldzielone z innym subwolumenem tylko zejda o 1). Symetria do defer_inc_extent_cb. */
        struct gh2_cext_hdr hdr;
        uint64_t blocks[GH2_CEXT_MAX_BLOCKS];
        if (gh2_cext_decode(val, len, &hdr, blocks, GH2_CEXT_MAX_BLOCKS)) return 0;
        for (uint16_t i = 0; i < hdr.nblocks; i++)
            if (blocks[i]) gh2_txn_alloc_defer_dec(t, blocks[i]);
        return 0;
    }
    if (key->type != GH2_EXTENT_DATA) return 0;
    if (len != sizeof(struct gh2_extent)) return 0;
    struct gh2_extent e;
    memcpy(&e, val, sizeof(e));
    if (e.disk_block) gh2_txn_alloc_defer_dec(t, e.disk_block);
    if (e.dup_block)  gh2_txn_alloc_defer_dec(t, e.dup_block);
    return 0;
}
static int subvol_defer_dec_all(struct gh2_fs *fs, const struct gh2_bptr *fs_root) {
    int r = gh2_btree_walk_nodes(&fs->dev, fs_root, defer_dec_node_cb, &fs->alloc);
    if (r) return r;
    return gh2_btree_iterate(&fs->dev, fs_root, defer_dec_extent_cb, &fs->alloc);
}

int gh2_fs_subvol_delete(struct gh2_fs *fs, uint64_t subvol_id) {
    /* nie usuwaj domyslnego (id 1) ani aktywnego subwolumenu */
    if (subvol_id == GH2_SUBVOL_DEFAULT) return -EINVAL;
    if (subvol_id == fs->active_subvol) return -EBUSY;

    /* musi istniec; pobierz jego fs_root */
    struct gh2_subvol_item sv;
    int r = subvol_lookup(fs, subvol_id, &sv);
    if (r) return r;   /* -ENOENT gdy brak */

    /* --- atomowosc per-op: lokalny root_tree + savepoint --- */
    struct gh2_bptr root_tree = fs->root_tree;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    /* 1) usun wpis (subvol_id, GH2_ROOT_ITEM, 0) z drzewa korzeni (CoW) */
    {
        struct gh2_key rk = root_item_key(subvol_id);
        struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
        struct gh2_bptr nrt;
        r = gh2_btree_delete(&fs->dev, &a, &root_tree, fs->sb.generation, &rk, &nrt);
        if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
        root_tree = nrt;
    }

    /* 2) dec refcount WSZYSTKICH blokow osiagalnych z fs_root subwolumenu (defer; przy commit
     *    bloki wylaczne rc 1->0 zwolnione, wspoldzielone tylko zejda o 1). */
    r = subvol_defer_dec_all(fs, &sv.fs_root);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    if (fs->alloc.oom) { gh2_txn_alloc_rollback(&fs->alloc, sp); return -ENOMEM; }

    /* sukces: przyjmij nowy root_tree i commituj (zapis SB + zastosowanie defer_dec) */
    fs->root_tree = root_tree;
    return gh2_fs_commit(fs);
}

/* ---- dostep READ do wybranego subwolumenu (read-only, na jego fs_root) ---- */

/* rozwiaz sciezke do ino w kontekscie podanego root (read-only; `.`/`..`). */
static int path_resolve_at(struct gh2_fs *fs, const struct gh2_bptr *root, const char *path,
                           uint64_t *out_ino) {
    if (!path || path[0] != '/') return -EINVAL;
    uint64_t cur = GH2_ROOT_INO;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        if (clen > 0xFFFF) return -ENAMETOOLONG;
        if (clen == 1 && start[0] == '.') continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (cur == GH2_ROOT_INO) continue;
            uint64_t parent = 0; uint8_t ft = 0;
            int r = dir_lookup_at(fs, root, cur, "..", 2, &parent, &ft);
            if (r) return r;
            cur = parent;
            continue;
        }
        struct gh2_inode ci;
        int r = fs_read_inode_at(fs, root, cur, &ci);
        if (r) return r;
        if (ci.type != GH2_FT_DIR) return -ENOTDIR;
        uint64_t next = 0; uint8_t ft = 0;
        r = dir_lookup_at(fs, root, cur, start, (uint16_t)clen, &next, &ft);
        if (r) return r;
        cur = next;
    }
    if (out_ino) *out_ino = cur;
    return 0;
}

int gh2_fs_getattr_subvol(struct gh2_fs *fs, uint64_t subvol_id, const char *path,
                          struct gh2_inode *out, uint64_t *out_ino) {
    struct gh2_subvol_item sv;
    int r = subvol_lookup(fs, subvol_id, &sv);
    if (r) return r;
    uint64_t ino = 0;
    r = path_resolve_at(fs, &sv.fs_root, path, &ino);
    if (r) return r;
    if (out_ino) *out_ino = ino;
    return fs_read_inode_at(fs, &sv.fs_root, ino, out);
}

ssize_t gh2_fs_read_subvol(struct gh2_fs *fs, uint64_t subvol_id, const char *path,
                           void *buf, size_t len, uint64_t off) {
    struct gh2_subvol_item sv;
    int r = subvol_lookup(fs, subvol_id, &sv);
    if (r) return r;
    uint64_t ino = 0;
    r = path_resolve_at(fs, &sv.fs_root, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = fs_read_inode_at(fs, &sv.fs_root, ino, &in);
    if (r) return r;
    if (in.type == GH2_FT_DIR) return -EISDIR;
    if (in.type != GH2_FT_FILE) return -EINVAL;
    if (len == 0) return 0;
    if (off >= in.size) return 0;

    uint64_t avail = in.size - off;
    if (len > avail) len = (size_t)avail;

    uint8_t *dst = buf;
    uint64_t pos = off;
    size_t remaining = len;

    if (fs->compress) {
        /* v2.9: snapshot kontenera --compress trzyma dane w chunk extentach (GH2_EXTENT_COMP).
         * Czytaj caly chunk z fs_root SNAPSHOTU, kopiuj podzakres. */
        while (remaining > 0) {
            uint64_t chunk_off = (pos / GH2_CHUNK_BYTES) * GH2_CHUNK_BYTES;
            uint32_t coff = (uint32_t)(pos - chunk_off);
            uint32_t n = (uint32_t)GH2_CHUNK_BYTES - coff;
            if (n > remaining) n = (uint32_t)remaining;

            uint8_t rawbuf[GH2_CHUNK_BYTES];
            uint32_t raw_len = 0;
            r = chunk_read(fs, &sv.fs_root, ino, chunk_off, rawbuf, &raw_len);
            if (r) return r;
            memcpy(dst, rawbuf + coff, n);   /* dziura/za raw_len = zera */
            dst += n; pos += n; remaining -= n;
        }
        return (ssize_t)len;
    }

    while (remaining > 0) {
        uint64_t file_off = (pos / GH2_DATA_BLK) * GH2_DATA_BLK;
        uint32_t boff = (uint32_t)(pos - file_off);
        uint32_t n = GH2_DATA_BLK - boff;
        if (n > remaining) n = (uint32_t)remaining;

        struct gh2_extent e;
        int found = (extent_lookup_at(fs, &sv.fs_root, ino, file_off, &e) == 0);
        if (found) {
            uint8_t blk[GH2_DATA_BLK];
            r = data_block_read(fs, &e, blk);
            if (r) return r;
            memcpy(dst, blk + boff, n);
        } else {
            memset(dst, 0, n);
        }
        dst += n; pos += n; remaining -= n;
    }
    return (ssize_t)len;
}

/* ============================ path resolve ============================ */

/* rozwiaz sciezke od korzenia. `.`/`..` obslugiwane; -ENOENT/-ENOTDIR. */
int gh2_path_resolve(struct gh2_fs *fs, const char *path, uint64_t *out_ino) {
    if (!path || path[0] != '/') return -EINVAL;
    uint64_t cur = GH2_ROOT_INO;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;                      /* pomijaj slashe */
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        if (clen > 0xFFFF) return -ENAMETOOLONG;
        if (clen == 1 && start[0] == '.') continue; /* `.` -> bez zmian */
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* `..`: znajdz rodzica przez jego wpis ".."? Uproszczenie: skanuj? Korzen->korzen. */
            if (cur == GH2_ROOT_INO) continue;
            uint64_t parent = 0; uint8_t ft = 0;
            int r = dir_lookup(fs, cur, "..", 2, &parent, &ft);
            if (r) return r;
            cur = parent;
            continue;
        }
        /* bieżący musi byc katalogiem */
        struct gh2_inode ci;
        int r = gh2_fs_read_inode(fs, cur, &ci);
        if (r) return r;
        if (ci.type != GH2_FT_DIR) return -ENOTDIR;
        uint64_t next = 0; uint8_t ft = 0;
        r = dir_lookup(fs, cur, start, (uint16_t)clen, &next, &ft);
        if (r) return r;                            /* -ENOENT przepuszczone */
        cur = next;
    }
    if (out_ino) *out_ino = cur;
    return 0;
}

/* ============================ getattr ============================ */

int gh2_fs_getattr(struct gh2_fs *fs, const char *path, struct gh2_inode *out, uint64_t *out_ino) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    if (out_ino) *out_ino = ino;
    return gh2_fs_read_inode(fs, ino, out);
}

/* ============================ split sciezki na (parent, basename) ============================ */

/* rozdziel path na katalog-rodzic (rozwiazany do ino) i ostatni komponent (nazwa).
 * Zwraca -ENOENT/-ENOTDIR dla rodzica; -EINVAL dla "/" lub pustej nazwy. */
static int split_parent(struct gh2_fs *fs, const char *path, uint64_t *parent_ino,
                        const char **base, size_t *base_len) {
    if (!path || path[0] != '/') return -EINVAL;
    /* znajdz ostatni komponent */
    const char *end = path + strlen(path);
    while (end > path && end[-1] == '/') end--;     /* przytnij trailing slash */
    if (end == path) return -EINVAL;                /* sama "/" */
    const char *bstart = end;
    while (bstart > path && bstart[-1] != '/') bstart--;
    size_t blen = (size_t)(end - bstart);
    if (blen == 0 || blen > 0xFFFF) return -EINVAL;

    /* prefiks katalogu rodzica: [path .. bstart) */
    char dirbuf[4096];
    size_t dlen = (size_t)(bstart - path);
    if (dlen == 0) {                                /* rodzic = korzen */
        *parent_ino = GH2_ROOT_INO;
    } else {
        if (dlen >= sizeof(dirbuf)) return -ENAMETOOLONG;
        memcpy(dirbuf, path, dlen);
        dirbuf[dlen] = '\0';
        int r = gh2_path_resolve(fs, dirbuf, parent_ino);
        if (r) return r;
        struct gh2_inode pi;
        r = gh2_fs_read_inode(fs, *parent_ino, &pi);
        if (r) return r;
        if (pi.type != GH2_FT_DIR) return -ENOTDIR;
    }
    *base = bstart;
    *base_len = blen;
    return 0;
}

/* ============================ create / mkdir ============================ */

/* wspolny szkielet: alokuj ino, zapisz INODE_ITEM, dla katalogu dodaj wpisy `.`/`..`,
 * dodaj DIR_ITEM w rodzicu, dla mkdir nlink rodzica +1. */
static int fs_make(struct gh2_fs *fs, const char *path, uint16_t mode, uint8_t ftype) {
    uint64_t parent = 0;
    const char *base = NULL; size_t blen = 0;
    int r = split_parent(fs, path, &parent, &base, &blen);
    if (r) return r;

    /* -EEXIST? */
    uint64_t exist = 0; uint8_t eft = 0;
    r = dir_lookup(fs, parent, base, (uint16_t)blen, &exist, &eft);
    if (r == 0) return -EEXIST;
    if (r != -ENOENT) return r;

    uint64_t ino = fs->next_ino;

    /* nowy i-wezel */
    struct gh2_inode ni;
    memset(&ni, 0, sizeof(ni));
    ni.type = ftype;
    ni.mode = mode;
    ni.nlink = (ftype == GH2_FT_DIR) ? 2 : 1;
    ni.size = 0;
    uint64_t now = (uint64_t)time(NULL);
    ni.atime = ni.mtime = ni.ctime = now;
    ni.generation = fs->sb.generation;

    /* --- atomowosc per-op: lokalny root + savepoint; przy bledzie rollback --- */
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    r = fs_write_inode(fs, &root, ino, &ni);
    if (r) goto fail;

    if (ftype == GH2_FT_DIR) {
        /* wpisy `.`(self) i `..`(parent) w nowym katalogu */
        r = dir_add_entry(fs, &root, ino, ".", 1, ino, GH2_FT_DIR);
        if (r) goto fail;
        r = dir_add_entry(fs, &root, ino, "..", 2, parent, GH2_FT_DIR);
        if (r) goto fail;
    }

    /* wpis w katalogu rodzica */
    r = dir_add_entry(fs, &root, parent, base, (uint16_t)blen, ino, ftype);
    if (r) goto fail;

    {
        struct gh2_inode pi;
        r = fs_read_inode_at(fs, &root, parent, &pi);
        if (r) goto fail;
        if (ftype == GH2_FT_DIR) pi.nlink += 1;   /* za nowy `..` */
        pi.mtime = pi.ctime = now;
        r = fs_write_inode(fs, &root, parent, &pi);
        if (r) goto fail;
    }

    fs->fs_root = root;
    fs->next_ino = ino + 1;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

int gh2_fs_create(struct gh2_fs *fs, const char *path, uint16_t mode) {
    return fs_make(fs, path, mode, GH2_FT_FILE);
}
int gh2_fs_mkdir(struct gh2_fs *fs, const char *path, uint16_t mode) {
    return fs_make(fs, path, mode, GH2_FT_DIR);
}

/* ============================ readdir ============================ */

struct readdir_ctx {
    gh2_readdir_cb cb;
    void *user;
    int rc;
};

static int readdir_item_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)key;
    struct readdir_ctx *rc = ctx;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *ename = (const char *)v + off + sizeof(struct gh2_dirent);
        /* pomin `.` i `..` — emitowane jawnie przez readdir (unikamy duplikatow) */
        int is_dot = (nlen == 1 && ename[0] == '.');
        int is_dotdot = (nlen == 2 && ename[0] == '.' && ename[1] == '.');
        if (!is_dot && !is_dotdot) {
            char namebuf[256];
            uint16_t emit = nlen;
            if (emit > sizeof(namebuf) - 1) emit = sizeof(namebuf) - 1;
            memcpy(namebuf, ename, emit);
            namebuf[emit] = '\0';
            int r = rc->cb(namebuf, de.name_len, de.ino, de.ftype, rc->user);
            if (r != 0) { rc->rc = r; return r; }
        }
        off += rec;
    }
    return 0;
}

int gh2_fs_readdir(struct gh2_fs *fs, const char *path, gh2_readdir_cb cb, void *ctx) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode di;
    r = gh2_fs_read_inode(fs, ino, &di);
    if (r) return r;
    if (di.type != GH2_FT_DIR) return -ENOTDIR;

    /* emituj `.` i `..` (zawsze; uproszczenie: `..` korzenia -> korzen) */
    r = cb(".", 1, ino, GH2_FT_DIR, ctx);
    if (r != 0) return r;
    {
        uint64_t parent = ino; uint8_t ft = GH2_FT_DIR;
        if (ino != GH2_ROOT_INO)
            dir_lookup(fs, ino, "..", 2, &parent, &ft);
        r = cb("..", 2, parent, GH2_FT_DIR, ctx);
        if (r != 0) return r;
    }

    /* range-scan DIR_ITEM rodzica: klucze (ino, GH2_DIR_ITEM, 0..MAX), pomijajac `.`/`..` */
    struct readdir_ctx rctx = { cb, ctx, 0 };
    struct gh2_key min = dir_key(ino, 0);
    struct gh2_key max = dir_key(ino, UINT64_MAX);
    /* opakuj cb, by pominac wpisy `.` i `..` z DIR_ITEM (sa rowniez spakowane) */
    return gh2_btree_iterate_range(&fs->dev, &fs->fs_root, &min, &max,
                                   readdir_item_cb, &rctx);
}

/* ============================ Task 2: pomocnicy ============================ */

/* zlicz REALNE wpisy katalogu ino (z pominieciem `.` i `..`). */
struct count_ctx { uint64_t n; };
static int count_item_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    (void)key;
    struct count_ctx *c = ctx;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *ename = (const char *)v + off + sizeof(struct gh2_dirent);
        int is_dot = (nlen == 1 && ename[0] == '.');
        int is_dotdot = (nlen == 2 && ename[0] == '.' && ename[1] == '.');
        if (!is_dot && !is_dotdot) c->n++;
        off += rec;
    }
    return 0;
}

/* liczba realnych wpisow w katalogu ino (bez `.`/`..`). */
static int dir_count_entries(struct gh2_fs *fs, uint64_t ino, uint64_t *out) {
    struct count_ctx c = { 0 };
    struct gh2_key min = dir_key(ino, 0);
    struct gh2_key max = dir_key(ino, UINT64_MAX);
    int r = gh2_btree_iterate_range(&fs->dev, &fs->fs_root, &min, &max, count_item_cb, &c);
    if (r) return r;
    *out = c.n;
    return 0;
}

/* split_parent + lookup nazwy: zwroc parent, base, ino i ftype celu. -ENOENT gdy brak. */
static int resolve_for_remove(struct gh2_fs *fs, const char *path, uint64_t *parent,
                              const char **base, size_t *blen, uint64_t *ino, uint8_t *ftype) {
    int r = split_parent(fs, path, parent, base, blen);
    if (r) return r;
    return dir_lookup(fs, *parent, *base, (uint16_t)*blen, ino, ftype);
}

/* ============================ unlink ============================ */

int gh2_fs_unlink(struct gh2_fs *fs, const char *path) {
    uint64_t parent = 0, ino = 0;
    const char *base = NULL; size_t blen = 0; uint8_t ft = 0;
    int r = resolve_for_remove(fs, path, &parent, &base, &blen, &ino, &ft);
    if (r) return r;
    if (ft == GH2_FT_DIR) return -EISDIR;

    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    /* usun wpis z rodzica */
    r = dir_remove_entry(fs, &root, parent, base, (uint16_t)blen);
    if (r) goto fail;

    uint64_t now = (uint64_t)time(NULL);
    if (in.nlink > 0) in.nlink -= 1;
    if (in.nlink == 0) {
        /* usun INODE_ITEM (+ SYMLINK_DATA gdy symlink, + WSZYSTKIE EXTENT_DATA gdy plik) */
        if (in.type == GH2_FT_SYMLINK) {
            struct gh2_key sk = symlink_key(ino);
            r = fs_delete(fs, &root, &sk);
            if (r && r != -ENOENT) goto fail;
        }
        if (in.type == GH2_FT_FILE) {
            r = gh2_inode_free_extents(fs, &root, ino);   /* zwolnij bloki danych (defer) */
            if (r) goto fail;
        }
        r = gh2_inode_free_xattrs(fs, &root, ino);        /* usun WSZYSTKIE (ino,7,*) — brak sierot */
        if (r) goto fail;
        struct gh2_key ik = inode_key(ino);
        r = fs_delete(fs, &root, &ik);
        if (r) goto fail;
    } else {
        in.ctime = now;
        r = fs_write_inode(fs, &root, ino, &in);
        if (r) goto fail;
    }

    /* mtime rodzica */
    struct gh2_inode pi;
    r = fs_read_inode_at(fs, &root, parent, &pi);
    if (r) goto fail;
    pi.mtime = pi.ctime = now;
    r = fs_write_inode(fs, &root, parent, &pi);
    if (r) goto fail;

    fs->fs_root = root;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

/* ============================ rmdir ============================ */

int gh2_fs_rmdir(struct gh2_fs *fs, const char *path) {
    uint64_t parent = 0, ino = 0;
    const char *base = NULL; size_t blen = 0; uint8_t ft = 0;
    int r = resolve_for_remove(fs, path, &parent, &base, &blen, &ino, &ft);
    if (r) return r;
    if (ft != GH2_FT_DIR) return -ENOTDIR;
    if (ino == GH2_ROOT_INO) return -EBUSY;        /* nie usuwaj korzenia */

    /* musi byc pusty (brak wpisow poza `.`/`..`) */
    uint64_t nent = 0;
    r = dir_count_entries(fs, ino, &nent);
    if (r) return r;
    if (nent != 0) return -ENOTEMPTY;

    uint64_t now = (uint64_t)time(NULL);

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    /* usun wpis w rodzicu */
    r = dir_remove_entry(fs, &root, parent, base, (uint16_t)blen);
    if (r) goto fail;
    /* usun jego `..` DIR_ITEM */
    r = dir_remove_entry(fs, &root, ino, "..", 2);
    if (r && r != -ENOENT) goto fail;
    /* usun jego `.` jesli realny (moze byc) */
    r = dir_remove_entry(fs, &root, ino, ".", 1);
    if (r && r != -ENOENT) goto fail;
    /* usun WSZYSTKIE xattr katalogu (ino,7,*) — brak sierot */
    r = gh2_inode_free_xattrs(fs, &root, ino);
    if (r) goto fail;
    /* usun INODE_ITEM katalogu */
    struct gh2_key ik = inode_key(ino);
    r = fs_delete(fs, &root, &ik);
    if (r) goto fail;

    /* nlink rodzica -- (za zniknięty `..`) */
    struct gh2_inode pi;
    r = fs_read_inode_at(fs, &root, parent, &pi);
    if (r) goto fail;
    if (pi.nlink > 0) pi.nlink -= 1;
    pi.mtime = pi.ctime = now;
    r = fs_write_inode(fs, &root, parent, &pi);
    if (r) goto fail;

    fs->fs_root = root;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

/* ============================ link (hardlink) ============================ */

int gh2_fs_link(struct gh2_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, oldpath, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    if (in.type == GH2_FT_DIR) return -EPERM;       /* hardlink do katalogu zabroniony */

    uint64_t parent = 0;
    const char *base = NULL; size_t blen = 0;
    r = split_parent(fs, newpath, &parent, &base, &blen);
    if (r) return r;

    /* -EEXIST? */
    uint64_t exist = 0; uint8_t eft = 0;
    r = dir_lookup(fs, parent, base, (uint16_t)blen, &exist, &eft);
    if (r == 0) return -EEXIST;
    if (r != -ENOENT) return r;

    uint64_t now = (uint64_t)time(NULL);

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    r = dir_add_entry(fs, &root, parent, base, (uint16_t)blen, ino, in.type);
    if (r) goto fail;

    in.nlink += 1;
    in.ctime = now;
    r = fs_write_inode(fs, &root, ino, &in);
    if (r) goto fail;

    struct gh2_inode pi;
    r = fs_read_inode_at(fs, &root, parent, &pi);
    if (r) goto fail;
    pi.mtime = pi.ctime = now;
    r = fs_write_inode(fs, &root, parent, &pi);
    if (r) goto fail;

    fs->fs_root = root;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

/* ============================ symlink / readlink ============================ */

int gh2_fs_symlink(struct gh2_fs *fs, const char *target, const char *path) {
    if (!target) return -EINVAL;
    size_t tlen = strlen(target);
    if (tlen == 0 || tlen > GH2_FS_MAX_VAL) return -ENAMETOOLONG;

    uint64_t parent = 0;
    const char *base = NULL; size_t blen = 0;
    int r = split_parent(fs, path, &parent, &base, &blen);
    if (r) return r;

    uint64_t exist = 0; uint8_t eft = 0;
    r = dir_lookup(fs, parent, base, (uint16_t)blen, &exist, &eft);
    if (r == 0) return -EEXIST;
    if (r != -ENOENT) return r;

    uint64_t ino = fs->next_ino;
    uint64_t now = (uint64_t)time(NULL);

    struct gh2_inode ni;
    memset(&ni, 0, sizeof(ni));
    ni.type = GH2_FT_SYMLINK;
    ni.mode = 0777;
    ni.nlink = 1;
    ni.size = tlen;
    ni.atime = ni.mtime = ni.ctime = now;
    ni.generation = fs->sb.generation;

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    r = fs_write_inode(fs, &root, ino, &ni);
    if (r) goto fail;

    /* SYMLINK_DATA = bajty celu (bez NUL) */
    struct gh2_key sk = symlink_key(ino);
    r = fs_insert(fs, &root, &sk, target, (uint32_t)tlen);
    if (r) goto fail;

    r = dir_add_entry(fs, &root, parent, base, (uint16_t)blen, ino, GH2_FT_SYMLINK);
    if (r) goto fail;

    struct gh2_inode pi;
    r = fs_read_inode_at(fs, &root, parent, &pi);
    if (r) goto fail;
    pi.mtime = pi.ctime = now;
    r = fs_write_inode(fs, &root, parent, &pi);
    if (r) goto fail;

    fs->fs_root = root;
    fs->next_ino = ino + 1;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

int gh2_fs_readlink(struct gh2_fs *fs, const char *path, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return -EINVAL;
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    if (in.type != GH2_FT_SYMLINK) return -EINVAL;

    uint8_t tmp[GH2_FS_MAX_VAL];
    uint32_t tlen = 0;
    struct gh2_key sk = symlink_key(ino);
    r = gh2_btree_lookup(&fs->dev, &fs->fs_root, &sk, tmp, sizeof(tmp), &tlen);
    if (r) return r;
    size_t copy = tlen;
    if (copy > buflen - 1) copy = buflen - 1;
    memcpy(buf, tmp, copy);
    buf[copy] = '\0';
    return 0;
}

/* ============================ mknod ============================ */

int gh2_fs_mknod(struct gh2_fs *fs, const char *path, uint32_t mode, uint64_t rdev) {
    uint8_t ftype;
    if (S_ISFIFO(mode)) ftype = GH2_FT_FIFO;
    else if (S_ISSOCK(mode)) ftype = GH2_FT_SOCK;
    else if (S_ISCHR(mode)) ftype = GH2_FT_CHR;
    else if (S_ISBLK(mode)) ftype = GH2_FT_BLK;
    else if (S_ISREG(mode) || (mode & S_IFMT) == 0) ftype = GH2_FT_FILE;
    else return -EINVAL;

    uint64_t parent = 0;
    const char *base = NULL; size_t blen = 0;
    int r = split_parent(fs, path, &parent, &base, &blen);
    if (r) return r;

    uint64_t exist = 0; uint8_t eft = 0;
    r = dir_lookup(fs, parent, base, (uint16_t)blen, &exist, &eft);
    if (r == 0) return -EEXIST;
    if (r != -ENOENT) return r;

    uint64_t ino = fs->next_ino;
    uint64_t now = (uint64_t)time(NULL);

    struct gh2_inode ni;
    memset(&ni, 0, sizeof(ni));
    ni.type = ftype;
    ni.mode = (uint16_t)(mode & 07777);
    ni.nlink = 1;
    ni.rdev = (ftype == GH2_FT_CHR || ftype == GH2_FT_BLK) ? rdev : 0;
    ni.atime = ni.mtime = ni.ctime = now;
    ni.generation = fs->sb.generation;

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    r = fs_write_inode(fs, &root, ino, &ni);
    if (r) goto fail;
    r = dir_add_entry(fs, &root, parent, base, (uint16_t)blen, ino, ftype);
    if (r) goto fail;

    struct gh2_inode pi;
    r = fs_read_inode_at(fs, &root, parent, &pi);
    if (r) goto fail;
    pi.mtime = pi.ctime = now;
    r = fs_write_inode(fs, &root, parent, &pi);
    if (r) goto fail;

    fs->fs_root = root;
    fs->next_ino = ino + 1;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

/* ============================ rename ============================ */

/* czy `anc` jest przodkiem (lub rowny) `node` — chodzac w gore po `..`. Chroni przed petla. */
static int is_ancestor(struct gh2_fs *fs, uint64_t anc, uint64_t node, int *yes) {
    *yes = 0;
    uint64_t cur = node;
    for (int guard = 0; guard < 1000000; guard++) {
        if (cur == anc) { *yes = 1; return 0; }
        if (cur == GH2_ROOT_INO) return 0;
        uint64_t parent = 0; uint8_t ft = 0;
        int r = dir_lookup(fs, cur, "..", 2, &parent, &ft);
        if (r) return r;
        if (parent == cur) return 0;               /* bezpieczenstwo */
        cur = parent;
    }
    return -ELOOP;
}

int gh2_fs_rename(struct gh2_fs *fs, const char *oldpath, const char *newpath, uint32_t flags) {
    (void)flags;
    uint64_t old_parent = 0, src_ino = 0;
    const char *old_base = NULL; size_t old_blen = 0; uint8_t src_ft = 0;
    int r = resolve_for_remove(fs, oldpath, &old_parent, &old_base, &old_blen, &src_ino, &src_ft);
    if (r) return r;

    uint64_t new_parent = 0;
    const char *new_base = NULL; size_t new_blen = 0;
    r = split_parent(fs, newpath, &new_parent, &new_base, &new_blen);
    if (r) return r;

    /* cel istnieje? */
    uint64_t dst_ino = 0; uint8_t dst_ft = 0;
    int have_dst = dir_lookup(fs, new_parent, new_base, (uint16_t)new_blen, &dst_ino, &dst_ft);
    if (have_dst != 0 && have_dst != -ENOENT) return have_dst;

    /* no-op: ten sam wpis (old==new sciezkowo) */
    if (have_dst == 0 && dst_ino == src_ino &&
        old_parent == new_parent &&
        old_blen == new_blen && memcmp(old_base, new_base, old_blen) == 0) {
        return 0;
    }

    /* BUG #2: rename dwoch ROZNYCH dowiazan TEGO SAMEGO i-wezla (cel istnieje, dst_ino==src_ino)
     * -> POSIX no-op success: oba dowiazania pozostaja, nic nie usuwamy. */
    if (have_dst == 0 && dst_ino == src_ino) {
        return 0;
    }

    /* OCHRONA PETLI: katalog nie moze trafic do wlasnego poddrzewa */
    if (src_ft == GH2_FT_DIR) {
        int anc = 0;
        r = is_ancestor(fs, src_ino, new_parent, &anc);
        if (r) return r;
        if (anc) return -EINVAL;
    }

    /* walidacja celu PRZED jakakolwiek mutacja (bez efektow ubocznych przy bledzie) */
    if (have_dst == 0) {
        if (dst_ft == GH2_FT_DIR) {
            if (src_ft != GH2_FT_DIR) return -EISDIR;
            uint64_t nent = 0;
            r = dir_count_entries(fs, dst_ino, &nent);
            if (r) return r;
            if (nent != 0) return -ENOTEMPTY;
        } else {
            if (src_ft == GH2_FT_DIR) return -ENOTDIR;
        }
    }

    uint64_t now = (uint64_t)time(NULL);
    int parent_changed = (old_parent != new_parent);

    /* --- atomowosc per-op: lokalny root + savepoint; przy bledzie rollback --- */
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    /* jesli cel istnieje -> nadpisz (usun cel wg typu) */
    if (have_dst == 0) {
        if (dst_ft == GH2_FT_DIR) {
            /* usun cel-katalog: jego wpis + `.`/`..` + INODE; nlink new_parent-- */
            r = dir_remove_entry(fs, &root, new_parent, new_base, (uint16_t)new_blen);
            if (r) goto fail;
            r = dir_remove_entry(fs, &root, dst_ino, "..", 2);
            if (r && r != -ENOENT) goto fail;
            r = dir_remove_entry(fs, &root, dst_ino, ".", 1);
            if (r && r != -ENOENT) goto fail;
            struct gh2_key dik = inode_key(dst_ino);
            r = fs_delete(fs, &root, &dik);
            if (r) goto fail;
            struct gh2_inode np;
            r = fs_read_inode_at(fs, &root, new_parent, &np);
            if (r) goto fail;
            if (np.nlink > 0) np.nlink -= 1;
            r = fs_write_inode(fs, &root, new_parent, &np);
            if (r) goto fail;
        } else {
            /* cel-plik/symlink/dev: usun wpis + nlink--/usun INODE */
            r = dir_remove_entry(fs, &root, new_parent, new_base, (uint16_t)new_blen);
            if (r) goto fail;
            struct gh2_inode di;
            r = fs_read_inode_at(fs, &root, dst_ino, &di);
            if (r) goto fail;
            if (di.nlink > 0) di.nlink -= 1;
            if (di.nlink == 0) {
                if (di.type == GH2_FT_SYMLINK) {
                    struct gh2_key sk = symlink_key(dst_ino);
                    r = fs_delete(fs, &root, &sk);
                    if (r && r != -ENOENT) goto fail;
                }
                if (di.type == GH2_FT_FILE) {
                    r = gh2_inode_free_extents(fs, &root, dst_ino);  /* bloki danych (defer) */
                    if (r) goto fail;
                }
                struct gh2_key dik = inode_key(dst_ino);
                r = fs_delete(fs, &root, &dik);
                if (r) goto fail;
            } else {
                di.ctime = now;
                r = fs_write_inode(fs, &root, dst_ino, &di);
                if (r) goto fail;
            }
        }
    }

    /* usun stary wpis, wstaw nowy -> ten sam ino */
    r = dir_remove_entry(fs, &root, old_parent, old_base, (uint16_t)old_blen);
    if (r) goto fail;
    r = dir_add_entry(fs, &root, new_parent, new_base, (uint16_t)new_blen, src_ino, src_ft);
    if (r) goto fail;

    /* katalog zmienia rodzica -> popraw jego `..` + nlink starego/nowego rodzica */
    if (src_ft == GH2_FT_DIR && parent_changed) {
        r = dir_remove_entry(fs, &root, src_ino, "..", 2);
        if (r && r != -ENOENT) goto fail;
        r = dir_add_entry(fs, &root, src_ino, "..", 2, new_parent, GH2_FT_DIR);
        if (r) goto fail;

        struct gh2_inode op;
        r = fs_read_inode_at(fs, &root, old_parent, &op);
        if (r) goto fail;
        if (op.nlink > 0) op.nlink -= 1;
        op.mtime = op.ctime = now;
        r = fs_write_inode(fs, &root, old_parent, &op);
        if (r) goto fail;

        struct gh2_inode np;
        r = fs_read_inode_at(fs, &root, new_parent, &np);
        if (r) goto fail;
        np.nlink += 1;
        np.mtime = np.ctime = now;
        r = fs_write_inode(fs, &root, new_parent, &np);
        if (r) goto fail;
    } else {
        /* mtime obu rodzicow (moga byc rowni) */
        struct gh2_inode op;
        r = fs_read_inode_at(fs, &root, old_parent, &op);
        if (r) goto fail;
        op.mtime = op.ctime = now;
        r = fs_write_inode(fs, &root, old_parent, &op);
        if (r) goto fail;
        if (parent_changed) {
            struct gh2_inode np;
            r = fs_read_inode_at(fs, &root, new_parent, &np);
            if (r) goto fail;
            np.mtime = np.ctime = now;
            r = fs_write_inode(fs, &root, new_parent, &np);
            if (r) goto fail;
        }
    }

    fs->fs_root = root;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

/* ============================ v2.4: write / read ============================ */

ssize_t gh2_fs_write(struct gh2_fs *fs, const char *path, const void *buf, size_t len,
                     uint64_t off) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    if (in.type == GH2_FT_DIR) return -EISDIR;
    if (in.type != GH2_FT_FILE) return -EINVAL;
    if (len == 0) return 0;
    if (off + len < off) return -EFBIG;                /* overflow offsetu */

    const uint8_t *src = buf;

    /* --- atomowosc per-op: lokalny root + savepoint; przy bledzie rollback --- */
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    if (fs->compress) {
        /* ---- sciezka chunk (kontener --compress): RMW po chunkach GH2_CHUNK_BYTES ---- */
        uint64_t pos = off;
        size_t remaining = len;
        while (remaining > 0) {
            uint64_t chunk_off = (pos / GH2_CHUNK_BYTES) * GH2_CHUNK_BYTES;
            uint32_t coff = (uint32_t)(pos - chunk_off);
            uint32_t n = (uint32_t)GH2_CHUNK_BYTES - coff;
            if (n > remaining) n = (uint32_t)remaining;

            /* RMW: wczytaj istniejacy chunk (lub zera), nalozenie nowych bajtow */
            uint8_t rawbuf[GH2_CHUNK_BYTES];
            uint32_t cur_raw = 0;
            r = chunk_read(fs, &root, ino, chunk_off, rawbuf, &cur_raw);
            if (r) goto fail;
            memcpy(rawbuf + coff, src, n);
            uint32_t new_raw = coff + n;
            if (new_raw < cur_raw) new_raw = cur_raw;   /* nie skracaj istniejacych danych */
            r = chunk_write(fs, &root, ino, chunk_off, rawbuf, new_raw);
            if (r) goto fail;
            src += n; pos += n; remaining -= n;
        }
    } else {
        uint64_t pos = off;
        size_t remaining = len;
        while (remaining > 0) {
            uint64_t file_off = (pos / GH2_DATA_BLK) * GH2_DATA_BLK;
            uint32_t boff = (uint32_t)(pos - file_off);
            uint32_t n = GH2_DATA_BLK - boff;
            if (n > remaining) n = (uint32_t)remaining;
            r = write_block(fs, &root, ino, file_off, src, n, boff);
            if (r) goto fail;
            src += n; pos += n; remaining -= n;
        }
    }

    /* aktualizuj rozmiar i mtime */
    uint64_t end = off + len;
    if (end > in.size) in.size = end;
    in.mtime = in.ctime = (uint64_t)time(NULL);
    r = fs_write_inode(fs, &root, ino, &in);
    if (r) goto fail;

    fs->fs_root = root;
    return (ssize_t)len;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

ssize_t gh2_fs_read(struct gh2_fs *fs, const char *path, void *buf, size_t len, uint64_t off) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    if (in.type == GH2_FT_DIR) return -EISDIR;
    if (in.type != GH2_FT_FILE) return -EINVAL;
    if (len == 0) return 0;
    if (off >= in.size) return 0;                      /* poza EOF */

    /* ogranicz do inode.size */
    uint64_t avail = in.size - off;
    if (len > avail) len = (size_t)avail;

    uint8_t *dst = buf;
    uint64_t pos = off;
    size_t remaining = len;

    if (fs->compress) {
        /* ---- sciezka chunk: czytaj caly chunk, kopiuj podzakres ---- */
        while (remaining > 0) {
            uint64_t chunk_off = (pos / GH2_CHUNK_BYTES) * GH2_CHUNK_BYTES;
            uint32_t coff = (uint32_t)(pos - chunk_off);
            uint32_t n = (uint32_t)GH2_CHUNK_BYTES - coff;
            if (n > remaining) n = (uint32_t)remaining;

            uint8_t rawbuf[GH2_CHUNK_BYTES];           /* pelny chunk, ogon poza raw_len = 0 */
            uint32_t raw_len = 0;
            r = chunk_read(fs, &fs->fs_root, ino, chunk_off, rawbuf, &raw_len);
            if (r) return r;
            memcpy(dst, rawbuf + coff, n);             /* dziura/za raw_len = zera (rawbuf wyzerowany) */
            dst += n; pos += n; remaining -= n;
        }
        return (ssize_t)len;
    }

    while (remaining > 0) {
        uint64_t file_off = (pos / GH2_DATA_BLK) * GH2_DATA_BLK;
        uint32_t boff = (uint32_t)(pos - file_off);
        uint32_t n = GH2_DATA_BLK - boff;
        if (n > remaining) n = (uint32_t)remaining;

        struct gh2_extent e;
        int found = (extent_lookup_at(fs, &fs->fs_root, ino, file_off, &e) == 0);
        if (found) {
            uint8_t blk[GH2_DATA_BLK];
            r = data_block_read(fs, &e, blk);          /* pelne 4 KB, ogon=0; csum->EIO/dup */
            if (r) return r;
            memcpy(dst, blk + boff, n);
        } else {
            memset(dst, 0, n);                         /* dziura -> zera */
        }
        dst += n; pos += n; remaining -= n;
    }
    return (ssize_t)len;
}

/* ============================ chmod / chown / utimens / truncate ============================ */

int gh2_fs_chmod(struct gh2_fs *fs, const char *path, uint16_t mode) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    in.mode = (uint16_t)(mode & 07777);
    in.ctime = (uint64_t)time(NULL);
    return fs_commit_one_inode(fs, ino, &in);
}

int gh2_fs_chown(struct gh2_fs *fs, const char *path, uint32_t uid, uint32_t gid) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    if (uid != (uint32_t)-1) in.uid = uid;
    if (gid != (uint32_t)-1) in.gid = gid;
    in.ctime = (uint64_t)time(NULL);
    return fs_commit_one_inode(fs, ino, &in);
}

int gh2_fs_utimens(struct gh2_fs *fs, const char *path, uint64_t atime, uint64_t mtime) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    in.atime = atime;
    in.mtime = mtime;
    in.ctime = (uint64_t)time(NULL);
    return fs_commit_one_inode(fs, ino, &in);
}

/* zbierz offsety ekstentow o file_off >= floor (do usuniecia przy skracaniu) */
struct trunc_ctx { uint64_t floor; uint64_t *offs; uint64_t *blks; uint32_t n, cap; int rc; };
static int trunc_collect_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct trunc_ctx *c = ctx;
    if (key->type != GH2_EXTENT_DATA) return 0;
    if (key->offset < c->floor) return 0;
    struct gh2_extent e;
    if (len != sizeof(e)) return 0;
    memcpy(&e, val, sizeof(e));
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 32;
        uint64_t *no = realloc(c->offs, (size_t)nc * sizeof(uint64_t));
        uint64_t *nb = realloc(c->blks, (size_t)nc * sizeof(uint64_t));
        if (!no || !nb) { free(no ? no : c->offs); free(nb ? nb : c->blks);
                          c->offs = NULL; c->blks = NULL; c->rc = -ENOMEM; return -ENOMEM; }
        c->offs = no; c->blks = nb; c->cap = nc;
    }
    c->offs[c->n] = key->offset; c->blks[c->n] = e.disk_block; c->n++;
    return 0;
}

int gh2_fs_truncate(struct gh2_fs *fs, const char *path, uint64_t size) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;
    struct gh2_inode in;
    r = gh2_fs_read_inode(fs, ino, &in);
    if (r) return r;
    if (in.type == GH2_FT_DIR) return -EISDIR;
    if (in.type != GH2_FT_FILE) {
        /* nie-plik (symlink/dev): tylko size (bez ekstentow) */
        in.size = size;
        in.mtime = in.ctime = (uint64_t)time(NULL);
        return fs_commit_one_inode(fs, ino, &in);
    }

    uint64_t now = (uint64_t)time(NULL);

    if (size >= in.size) {
        /* rozszerzenie (sparse): tylko size */
        in.size = size;
        in.mtime = in.ctime = now;
        return fs_commit_one_inode(fs, ino, &in);
    }

    if (fs->compress) {
        /* ---- sciezka chunk (kontener --compress): skroc ---- */
        /* chunk zawierajacy bajt `size`; floor = pierwszy chunk USUWANY w calosci */
        uint64_t last_chunk = (size / GH2_CHUNK_BYTES) * GH2_CHUNK_BYTES;
        uint32_t keep_in_last = (uint32_t)(size - last_chunk);   /* waznych bajtow w ost. chunku */
        uint64_t floor = (keep_in_last == 0) ? last_chunk : (last_chunk + GH2_CHUNK_BYTES);

        struct gh2_bptr croot = fs->fs_root;
        struct gh2_txn_savepoint csp = gh2_txn_alloc_mark(&fs->alloc);

        /* 1) usun chunki za nowym koncem (off >= floor) -> free ich bloki przez free vtable */
        struct free_comp_ctx c; memset(&c, 0, sizeof(c));
        {
            struct gh2_key min = comp_key(ino, floor);
            struct gh2_key max = comp_key(ino, UINT64_MAX);
            r = gh2_btree_iterate_range(&fs->dev, &croot, &min, &max, collect_comp_cb, &c);
            if (r) { free(c.offs); free(c.blks); free(c.nblk);
                     gh2_txn_alloc_rollback(&fs->alloc, csp); return c.rc ? c.rc : r; }
        }
        {
            struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
            for (uint32_t i = 0; i < c.n; i++) {
                struct gh2_key k = comp_key(ino, c.offs[i]);
                r = fs_delete(fs, &croot, &k);
                if (r) { free(c.offs); free(c.blks); free(c.nblk);
                         gh2_txn_alloc_rollback(&fs->alloc, csp); return r; }
                for (uint16_t j = 0; j < c.nblk[i]; j++) {
                    uint64_t b = c.blks[(size_t)i * GH2_CEXT_MAX_BLOCKS + j];
                    if (b) a.free(a.ctx, b);   /* defer_dec: zwolni tylko wylaczne (rc 0) */
                }
            }
        }
        free(c.offs); free(c.blks); free(c.nblk);

        /* 2) czesciowy ostatni chunk (size w srodku chunku) -> RMW: obetnij do keep_in_last,
         *    wyzeruj ogon, przepisz (chunk_write CoW: stare bloki -> defer_dec). */
        if (keep_in_last != 0) {
            uint8_t rawbuf[GH2_CHUNK_BYTES];
            uint32_t cur_raw = 0;
            r = chunk_read(fs, &croot, ino, last_chunk, rawbuf, &cur_raw);
            if (r) { gh2_txn_alloc_rollback(&fs->alloc, csp); return r; }
            if (cur_raw > keep_in_last) {
                memset(rawbuf + keep_in_last, 0, GH2_CHUNK_BYTES - keep_in_last);
                r = chunk_write(fs, &croot, ino, last_chunk, rawbuf, keep_in_last);
                if (r) { gh2_txn_alloc_rollback(&fs->alloc, csp); return r; }
            }
        }

        in.size = size;
        in.mtime = in.ctime = now;
        r = fs_write_inode(fs, &croot, ino, &in);
        if (r) { gh2_txn_alloc_rollback(&fs->alloc, csp); return r; }

        fs->fs_root = croot;
        return 0;
    }

    /* --- skrocenie: usun ekstenty >= align(size) + popraw ostatni czesciowy --- */
    /* blok zawierajacy bajt `size` (pierwszy NIEZACHOWany w calosci) */
    uint64_t last_off = (size / GH2_DATA_BLK) * GH2_DATA_BLK;
    uint32_t keep_in_last = (uint32_t)(size - last_off);   /* bajty do zachowania w ost. bloku */
    /* floor usuwania: gdy keep_in_last==0, usun od last_off; inaczej od last_off+BLK */
    uint64_t floor = (keep_in_last == 0) ? last_off : (last_off + GH2_DATA_BLK);

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    /* 1) usun pelne bloki za nowym konca */
    struct trunc_ctx tc; memset(&tc, 0, sizeof(tc)); tc.floor = floor;
    {
        struct gh2_key min = extent_key(ino, 0);
        struct gh2_key max = extent_key(ino, UINT64_MAX);
        r = gh2_btree_iterate_range(&fs->dev, &root, &min, &max, trunc_collect_cb, &tc);
        if (r) { free(tc.offs); free(tc.blks); r = tc.rc ? tc.rc : r; goto fail; }
    }
    {
        struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
        for (uint32_t i = 0; i < tc.n; i++) {
            struct gh2_key k = extent_key(ino, tc.offs[i]);
            r = fs_delete(fs, &root, &k);
            if (r) { free(tc.offs); free(tc.blks); goto fail; }
            if (tc.blks[i]) a.free(a.ctx, tc.blks[i]);
        }
    }
    free(tc.offs); free(tc.blks);

    /* 2) popraw ostatni czesciowy blok: wyzeruj ogon [keep_in_last..raw_len) i raw_len=keep_in_last.
     *    Realizujemy przez CoW write_block(0 bajtow)? Prostsze: odczytaj, wyzeruj ogon, przepisz. */
    if (keep_in_last != 0) {
        struct gh2_extent e;
        if (extent_lookup_at(fs, &root, ino, last_off, &e) == 0) {
            uint32_t old_raw = e.raw_len; if (old_raw > GH2_DATA_BLK) old_raw = GH2_DATA_BLK;
            if (old_raw > keep_in_last) {
                /* odczytaj plaintext, wyzeruj ogon, zapisz NOWY blok (CoW), zaktualizuj ekstent */
                uint8_t blk[GH2_DATA_BLK];
                r = data_block_read(fs, &e, blk);
                if (r) goto fail;
                memset(blk + keep_in_last, 0, GH2_DATA_BLK - keep_in_last);
                struct gh2_alloc a = gh2_txn_alloc_vtable(&fs->alloc);
                uint64_t nblk = 0;
                r = a.alloc(a.ctx, &nblk);
                if (r) goto fail;
                r = gh_disk_write(&fs->dev, nblk, blk);
                if (r) { a.free(a.ctx, nblk); goto fail; }
                struct gh2_extent ne = e;
                ne.disk_block = nblk;
                ne.raw_len = keep_in_last;
                ne.comp_len = keep_in_last;
                ne.csum = gh_crc32(blk, keep_in_last);
                uint8_t ebuf[sizeof(struct gh2_extent)];
                extent_encode(&ne, ebuf);
                struct gh2_key k = extent_key(ino, last_off);
                r = fs_insert(fs, &root, &k, ebuf, sizeof(ebuf));
                if (r) { a.free(a.ctx, nblk); goto fail; }
                if (e.disk_block) a.free(a.ctx, e.disk_block);   /* stary blok -> defer */
            }
        }
    }

    in.size = size;
    in.mtime = in.ctime = now;
    r = fs_write_inode(fs, &root, ino, &in);
    if (r) goto fail;

    fs->fs_root = root;
    return 0;

fail:
    gh2_txn_alloc_rollback(&fs->alloc, sp);
    return r;
}

/* ============================ statfs ============================ */

int gh2_fs_statfs(struct gh2_fs *fs, struct gh2_statfs *out) {
    if (!out) return -EINVAL;
    out->total_blocks = fs->space.nblocks;
    out->free_blocks = fs->space.nfree;
    out->block_size = GH2_BLOCK_SIZE;
    return 0;
}

/* ============================ v2xattr: rozszerzone atrybuty ============================ */
/* Itemy (ino, GH2_XATTR_ITEM, fnv1a64(name)) -> spakowana lista wpisow (kolizje hash). Wszystkie
 * mutacje atomowe per-op (savepoint/rollback na lokalnym root). Errno jak v1/POSIX. */

int gh2_fs_setxattr(struct gh2_fs *fs, const char *path, const char *name,
                    const void *value, size_t size, int flags) {
    if (!name || name[0] == '\0') return -EINVAL;
    size_t nlen_sz = strlen(name);
    if (nlen_sz > UINT16_MAX) return -ERANGE;
    if (size > UINT32_MAX) return -E2BIG;
    uint16_t nlen = (uint16_t)nlen_sz;

    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;

    uint64_t hash = fnv1a64(name, nlen);
    struct gh2_key k = xattr_key(ino, hash);

    uint8_t cur[GH2_FS_MAX_VAL];
    uint32_t curlen = 0;
    r = gh2_btree_lookup(&fs->dev, &fs->fs_root, &k, cur, sizeof(cur), &curlen);
    if (r && r != -ENOENT) return r;
    if (r == -ENOENT) curlen = 0;

    long off = (curlen > 0) ? xattr_find_entry(cur, curlen, name, nlen, NULL, NULL) : -1;
    int exists = (off >= 0);

    if ((flags & GH2_XATTR_CREATE) && exists) return -EEXIST;
    if ((flags & GH2_XATTR_REPLACE) && !exists) return -ENODATA;

    uint32_t newrec = (uint32_t)sizeof(struct gh2_xattr_hdr) + nlen + (uint32_t)size;

    /* zbuduj nowa wartosc: skopiuj istniejace wpisy pomijajac stary (jesli jest), dopisz nowy */
    uint8_t nv[GH2_FS_MAX_VAL];
    uint32_t nl = 0;
    if (exists) {
        struct gh2_xattr_hdr oh;
        memcpy(&oh, cur + off, sizeof(oh));
        uint32_t orec = (uint32_t)sizeof(oh) + oh.name_len + oh.value_len;
        if ((uint32_t)off > 0) { memcpy(nv, cur, (size_t)off); nl = (uint32_t)off; }
        uint32_t tail = (uint32_t)off + orec;
        if (tail < curlen) { memcpy(nv + nl, cur + tail, curlen - tail); nl += curlen - tail; }
    } else if (curlen > 0) {
        memcpy(nv, cur, curlen); nl = curlen;
    }

    if ((uint64_t)nl + newrec > GH2_FS_MAX_VAL) return -E2BIG;   /* name+value+naglowki za duze */

    struct gh2_xattr_hdr h; h.name_len = nlen; h.value_len = (uint32_t)size;
    memcpy(nv + nl, &h, sizeof(h));
    memcpy(nv + nl + sizeof(h), name, nlen);
    if (size) memcpy(nv + nl + sizeof(h) + nlen, value, size);
    nl += newrec;

    /* atomowo: lokalny root + savepoint */
    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
    r = fs_insert(fs, &root, &k, nv, nl);
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    fs->fs_root = root;
    return 0;
}

ssize_t gh2_fs_getxattr(struct gh2_fs *fs, const char *path, const char *name,
                        void *buf, size_t size) {
    if (!name || name[0] == '\0') return -EINVAL;
    size_t nlen_sz = strlen(name);
    if (nlen_sz > UINT16_MAX) return -ERANGE;
    uint16_t nlen = (uint16_t)nlen_sz;

    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;

    struct gh2_key k = xattr_key(ino, fnv1a64(name, nlen));
    uint8_t cur[GH2_FS_MAX_VAL];
    uint32_t curlen = 0;
    r = gh2_btree_lookup(&fs->dev, &fs->fs_root, &k, cur, sizeof(cur), &curlen);
    if (r == -ENOENT) return -ENODATA;
    if (r) return r;

    uint32_t vlen = 0, voff = 0;
    long off = xattr_find_entry(cur, curlen, name, nlen, &vlen, &voff);
    if (off < 0) return -ENODATA;

    if (size == 0) return (ssize_t)vlen;           /* zapytanie o rozmiar */
    if (size < vlen) return -ERANGE;
    memcpy(buf, cur + voff, vlen);
    return (ssize_t)vlen;
}

/* kontekst + callback listxattr: range-scan (ino,7,*), zbierz nazwy null-separated.
 * size==0 -> tylko policz rozmiar; inaczej zapisz (overflow gdy za maly -> -ERANGE). */
struct xattr_list_ctx { char *buf; size_t size; size_t need; int overflow; };
static int xattr_list_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct xattr_list_ctx *c = ctx;
    if (key->type != GH2_XATTR_ITEM) return 0;
    const uint8_t *v = val;
    uint32_t o = 0;
    while (o + sizeof(struct gh2_xattr_hdr) <= len) {
        struct gh2_xattr_hdr h;
        memcpy(&h, v + o, sizeof(h));
        uint32_t rec = (uint32_t)sizeof(h) + h.name_len + h.value_len;
        if (o + rec > len) break;
        size_t entry = (size_t)h.name_len + 1;     /* nazwa + NUL */
        if (c->size != 0) {
            if (c->need + entry > c->size) c->overflow = 1;
            else {
                memcpy(c->buf + c->need, v + o + sizeof(h), h.name_len);
                c->buf[c->need + h.name_len] = '\0';
            }
        }
        c->need += entry;
        o += rec;
    }
    return 0;
}

ssize_t gh2_fs_listxattr(struct gh2_fs *fs, const char *path, char *buf, size_t size) {
    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;

    struct xattr_list_ctx lc; memset(&lc, 0, sizeof(lc));
    lc.buf = buf; lc.size = size;

    struct gh2_key min = xattr_key(ino, 0);
    struct gh2_key max = xattr_key(ino, UINT64_MAX);
    r = gh2_btree_iterate_range(&fs->dev, &fs->fs_root, &min, &max, xattr_list_cb, &lc);
    if (r) return r;
    if (size == 0) return (ssize_t)lc.need;
    if (lc.overflow) return -ERANGE;
    return (ssize_t)lc.need;
}

int gh2_fs_removexattr(struct gh2_fs *fs, const char *path, const char *name) {
    if (!name || name[0] == '\0') return -EINVAL;
    size_t nlen_sz = strlen(name);
    if (nlen_sz > UINT16_MAX) return -ERANGE;
    uint16_t nlen = (uint16_t)nlen_sz;

    uint64_t ino = 0;
    int r = gh2_path_resolve(fs, path, &ino);
    if (r) return r;

    struct gh2_key k = xattr_key(ino, fnv1a64(name, nlen));
    uint8_t cur[GH2_FS_MAX_VAL];
    uint32_t curlen = 0;
    r = gh2_btree_lookup(&fs->dev, &fs->fs_root, &k, cur, sizeof(cur), &curlen);
    if (r == -ENOENT) return -ENODATA;
    if (r) return r;

    uint32_t vlen = 0, voff = 0;
    long off = xattr_find_entry(cur, curlen, name, nlen, &vlen, &voff);
    if (off < 0) return -ENODATA;

    struct gh2_xattr_hdr h;
    memcpy(&h, cur + off, sizeof(h));
    uint32_t rec = (uint32_t)sizeof(h) + h.name_len + h.value_len;

    struct gh2_bptr root = fs->fs_root;
    struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);

    if (curlen == rec) {
        /* jedyny wpis -> usun caly XATTR_ITEM */
        r = fs_delete(fs, &root, &k);
    } else {
        /* przepakuj: usun [off, off+rec) */
        uint8_t nv[GH2_FS_MAX_VAL];
        uint32_t nl = 0;
        if (off > 0) { memcpy(nv, cur, (size_t)off); nl = (uint32_t)off; }
        uint32_t tail = (uint32_t)off + rec;
        if (tail < curlen) { memcpy(nv + nl, cur + tail, curlen - tail); nl += curlen - tail; }
        r = fs_insert(fs, &root, &k, nv, nl);
    }
    if (r) { gh2_txn_alloc_rollback(&fs->alloc, sp); return r; }
    fs->fs_root = root;
    return 0;
}

/* ============================ v2.5: fsck (read-only walidator) ============================ */
/* Wydzielony z walidatora testowego (tests/test_v2fs.c fsck_check) + rozszerzony o ekstenty
 * (bloki danych: granice / w mapie / unikalnosc / brak kolizji z wezlem drzewa) i spojnosc
 * size. Read-only: iteruje zatwierdzony fs->fs_root, nie mutuje, nie naprawia. */

#define GH2_FSCK_MAX_INO 1000000

struct gh2_fsck {
    /* mapy indeksowane ino (ino male -> prosty array) */
    uint8_t  *is_inode;     /* INODE_ITEM istnieje */
    uint8_t  *ftype;        /* typ i-wezla (GH2_FT_*) */
    uint32_t *nlink;        /* zadeklarowany nlink */
    uint64_t *size;         /* zadeklarowany size */
    uint32_t *links_seen;   /* zliczone wpisy DIR_ITEM -> ino (bez `.`/`..`) */
    uint32_t *subdirs;      /* liczba podkatalogow (dla nlink katalogu) */
    uint8_t  *reachable;    /* osiagalny z roota (DFS po katalogach) */
    uint64_t  max_ino;

    /* mapa wykorzystania blokow danych (ekstenty): 1 = juz przypisany jakiemus ekstentowi */
    uint8_t  *blk_used;     /* indeksowany numerem bloku fizycznego [0..nblocks) */
    uint8_t  *blk_tree;     /* 1 = blok jest wezlem drzewa FS (kolizja z ekstentem = niespojnosc) */
    uint64_t  nblocks;
    struct gh2_space space_view;  /* kopia mapy (tylko odczyt gh2_space_is_used) */

    int       issues;       /* licznik niespojnosci */
};

/* --- callbacki iteracji (kontekst przekazany przez ctx) --- */

static int fsck_inode_cb2(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_fsck *f = ctx;
    if (key->type != GH2_INODE_ITEM) return 0;
    uint64_t ino = key->objectid;
    if (ino >= f->max_ino) { f->issues++; return 0; }
    struct gh2_inode in;
    if (len != sizeof(in)) { f->issues++; return 0; }
    memcpy(&in, val, sizeof(in));
    f->is_inode[ino] = 1;
    f->ftype[ino] = (uint8_t)in.type;
    f->nlink[ino] = in.nlink;
    f->size[ino] = in.size;
    return 0;
}

static int fsck_dir_cb2(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_fsck *f = ctx;
    if (key->type != GH2_DIR_ITEM) return 0;
    uint64_t parent = key->objectid;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *nm = (const char *)v + off + sizeof(struct gh2_dirent);
        int is_dot = (nlen == 1 && nm[0] == '.');
        int is_dotdot = (nlen == 2 && nm[0] == '.' && nm[1] == '.');
        if (de.ino >= f->max_ino) { f->issues++; off += rec; continue; }
        if (!is_dot && !is_dotdot) {
            f->links_seen[de.ino]++;
            /* Licz do subdirs[parent] TYLKO istniejacy katalog-dziecko. Wpis wiszacy
             * (dziecko bez INODE_ITEM) jest usuwany w pass naprawczym (a), wiec NIE moze
             * podbijac oczekiwanego nlink rodzica -> inaczej pass (c) ustawi nlink rodzica
             * na zawyzona wartosc i fsck po remount TRWALE zglasza niespojnosc.
             * is_inode/ftype dziecka sa juz wypelnione: fsck_inode_cb2 jest wolany PRZED
             * fsck_dir_cb2 w gh2_fsck. */
            if (de.ftype == GH2_FT_DIR && parent < f->max_ino &&
                de.ino < f->max_ino && f->is_inode[de.ino] &&
                f->ftype[de.ino] == GH2_FT_DIR)
                f->subdirs[parent]++;
        }
        off += rec;
    }
    return 0;
}

/* ekstenty: weryfikuj disk_block (granice / w mapie / unikalnosc / brak kolizji z wezlem) +
 * size spojny (off < align(size)). */
static int fsck_extent_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct gh2_fsck *f = ctx;
    if (key->type != GH2_EXTENT_DATA) return 0;
    uint64_t ino = key->objectid;
    struct gh2_extent e;
    if (extent_decode(val, len, &e)) { f->issues++; return 0; }
    uint64_t blk = e.disk_block;
    /* granice */
    if (blk < GH2_DATA_START || blk >= f->nblocks) { f->issues++; return 0; }
    /* oznaczony w mapie (mark-sweep zaznacza bloki danych) */
    if (!gh2_space_is_used(&f->space_view, blk)) f->issues++;
    /* nie koliduje z wezlem drzewa */
    if (f->blk_tree[blk]) f->issues++;
    /* UNIKALNY (brak wspoldzielenia miedzy ekstentami) */
    if (f->blk_used[blk]) f->issues++;
    f->blk_used[blk] = 1;
    /* size spojny: ekstent o off >= align(size) lezy poza koncem pliku (np. po truncate) */
    if (ino < f->max_ino && f->is_inode[ino]) {
        uint64_t sz = f->size[ino];
        uint64_t aligned = (sz + GH2_DATA_BLK - 1) / GH2_DATA_BLK * GH2_DATA_BLK;
        if (key->offset >= aligned) f->issues++;
    }
    return 0;
}

/* zbierz bloki-wezly drzewa (mark dla detekcji kolizji ekstent<->wezel) */
static int fsck_tree_block_cb(uint64_t block, void *ctx) {
    struct gh2_fsck *f = ctx;
    if (block < f->nblocks) f->blk_tree[block] = 1;
    return 0;
}

/* DFS osiagalnosci z roota po wpisach katalogow (bez `.`/`..`) */
static int fsck_reach(struct gh2_fs *fs, struct gh2_fsck *f, uint64_t dir_ino);
struct fsck_reach_ctx { struct gh2_fs *fs; struct gh2_fsck *f; int rc; };

static int fsck_reach_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct fsck_reach_ctx *rc = ctx;
    (void)key;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *nm = (const char *)v + off + sizeof(struct gh2_dirent);
        int is_dot = (nlen == 1 && nm[0] == '.');
        int is_dotdot = (nlen == 2 && nm[0] == '.' && nm[1] == '.');
        if (!is_dot && !is_dotdot && de.ino < rc->f->max_ino && !rc->f->reachable[de.ino]) {
            rc->f->reachable[de.ino] = 1;
            if (de.ftype == GH2_FT_DIR) {
                int r = fsck_reach(rc->fs, rc->f, de.ino);
                if (r) { rc->rc = r; return r; }
            }
        }
        off += rec;
    }
    return 0;
}

static int fsck_reach(struct gh2_fs *fs, struct gh2_fsck *f, uint64_t dir_ino) {
    struct fsck_reach_ctx rc = { fs, f, 0 };
    struct gh2_key min = dir_key(dir_ino, 0);
    struct gh2_key max = dir_key(dir_ino, UINT64_MAX);
    int r = gh2_btree_iterate_range(&fs->dev, &fs->fs_root, &min, &max, fsck_reach_cb, &rc);
    return r ? r : rc.rc;
}

/* --- pass naprawczy (repair) --- */

/* zebrany wiszacy wpis: (parent, name) do usuniecia po iteracji (nie modyfikuj w trakcie) */
struct dangling_ent { uint64_t parent; uint64_t hash; uint16_t nlen; char name[256]; };
struct fsck_collect_dangling {
    struct gh2_fsck     *f;
    struct dangling_ent *ents;
    uint32_t             n, cap;
    int                  rc;
};

/* iteruj DIR_ITEM; dla kazdego REALNEGO wpisu (poza `.`/`..`) o child bez INODE_ITEM (lub
 * child poza max_ino) zbierz (parent, name) do usuniecia. */
static int collect_dangling_cb(const struct gh2_key *key, const void *val, uint32_t len, void *ctx) {
    struct fsck_collect_dangling *c = ctx;
    if (key->type != GH2_DIR_ITEM) return 0;
    uint64_t parent = key->objectid;
    const uint8_t *v = val;
    uint32_t off = 0;
    while (off + sizeof(struct gh2_dirent) <= len) {
        struct gh2_dirent de;
        memcpy(&de, v + off, sizeof(de));
        uint32_t nlen = de.name_len;
        uint32_t rec = (uint32_t)sizeof(struct gh2_dirent) + nlen;
        if (off + rec > len) break;
        const char *nm = (const char *)v + off + sizeof(struct gh2_dirent);
        int is_dot = (nlen == 1 && nm[0] == '.');
        int is_dotdot = (nlen == 2 && nm[0] == '.' && nm[1] == '.');
        if (!is_dot && !is_dotdot && nlen < sizeof(((struct dangling_ent*)0)->name)) {
            int dangling = (de.ino >= c->f->max_ino) || !c->f->is_inode[de.ino];
            if (dangling) {
                if (c->n == c->cap) {
                    uint32_t nc = c->cap ? c->cap * 2 : 16;
                    struct dangling_ent *ne = realloc(c->ents, (size_t)nc * sizeof(*ne));
                    if (!ne) { c->rc = -ENOMEM; return -ENOMEM; }
                    c->ents = ne; c->cap = nc;
                }
                struct dangling_ent *e = &c->ents[c->n++];
                e->parent = parent;
                e->hash = key->offset;   /* RZECZYWISTY hash DIR_ITEM (moze != fnv1a64 nazwy) */
                e->nlen = (uint16_t)nlen;
                memcpy(e->name, nm, nlen);
                e->name[nlen] = '\0';
            }
        }
        off += rec;
    }
    return 0;
}

/* zwolnij i-wezel sieroty `ino` (typ `ft`) na lokalnym root: INODE_ITEM + EXTENT/COMP + XATTR +
 * SYMLINK_DATA + (jesli katalog) jego wlasne DIR_ITEM. Wzor: gh2_fs_unlink/rmdir. */
static int fsck_free_orphan(struct gh2_fs *fs, struct gh2_bptr *root, uint64_t ino, uint8_t ft) {
    int r;
    if (ft == GH2_FT_SYMLINK) {
        struct gh2_key sk = symlink_key(ino);
        r = fs_delete(fs, root, &sk);
        if (r && r != -ENOENT) return r;
    }
    if (ft == GH2_FT_FILE) {
        r = gh2_inode_free_extents(fs, root, ino);   /* EXTENT_DATA lub GH2_EXTENT_COMP (defer free) */
        if (r) return r;
    }
    if (ft == GH2_FT_DIR) {
        r = gh2_inode_free_dir_items(fs, root, ino);  /* wlasne wpisy (`.`,`..`, dzieci) */
        if (r) return r;
    }
    r = gh2_inode_free_xattrs(fs, root, ino);
    if (r) return r;
    struct gh2_key ik = inode_key(ino);
    return fs_delete(fs, root, &ik);
}

/* pass naprawczy: zaklada wypelnione f (detekcja zakonczona). Mutuje lokalny `root`.
 * Zwraca 0 (sukces) lub -errno (caller robi rollback). */
static int fsck_repair_pass(struct gh2_fs *fs, struct gh2_fsck *f, struct gh2_bptr *root) {
    int r;

    /* (a) wiszace DIR_ITEM: zbierz wpisy do nieistniejacych i-wezlow, potem usun */
    struct fsck_collect_dangling cd; memset(&cd, 0, sizeof(cd));
    cd.f = f;
    r = gh2_btree_iterate(&fs->dev, root, collect_dangling_cb, &cd);
    if (r) { free(cd.ents); return cd.rc ? cd.rc : r; }
    for (uint32_t i = 0; i < cd.n; i++) {
        r = dir_remove_entry_hashed(fs, root, cd.ents[i].parent, cd.ents[i].hash,
                                    cd.ents[i].name, cd.ents[i].nlen);
        if (r && r != -ENOENT) { free(cd.ents); return r; }
    }
    free(cd.ents);

    /* (b) sieroty: INODE istnieje, ale nieosiagalny z roota -> zwolnij i-wezel */
    for (uint64_t ino = 1; ino < f->max_ino; ino++) {
        if (ino == GH2_ROOT_INO) continue;
        if (f->is_inode[ino] && !f->reachable[ino]) {
            r = fsck_free_orphan(fs, root, ino, f->ftype[ino]);
            if (r) return r;
        }
    }

    /* (c) zly nlink: i-wezel osiagalny z mismatch -> ustaw na oczekiwany */
    for (uint64_t ino = 1; ino < f->max_ino; ino++) {
        if (!f->is_inode[ino] || !f->reachable[ino]) continue;
        uint32_t expect;
        if (f->ftype[ino] == GH2_FT_DIR) {
            expect = 2 + f->subdirs[ino];
        } else if (ino != GH2_ROOT_INO) {
            expect = f->links_seen[ino];
        } else {
            continue;   /* korzen-plik nie wystepuje; korzen-katalog obsluzony wyzej */
        }
        if (f->nlink[ino] != expect) {
            struct gh2_inode in;
            r = fs_read_inode_at(fs, root, ino, &in);
            if (r) return r;
            in.nlink = expect;
            r = fs_write_inode(fs, root, ino, &in);
            if (r) return r;
        }
    }
    return 0;
}

int gh2_fsck(struct gh2_fs *fs, int repair, int *issues) {
    struct gh2_fsck f; memset(&f, 0, sizeof(f));
    f.max_ino = fs->next_ino + 1;
    if (f.max_ino > GH2_FSCK_MAX_INO) f.max_ino = GH2_FSCK_MAX_INO;
    f.nblocks = fs->space.nblocks;

    f.is_inode   = calloc(f.max_ino, 1);
    f.ftype      = calloc(f.max_ino, 1);
    f.nlink      = calloc(f.max_ino, sizeof(uint32_t));
    f.size       = calloc(f.max_ino, sizeof(uint64_t));
    f.links_seen = calloc(f.max_ino, sizeof(uint32_t));
    f.subdirs    = calloc(f.max_ino, sizeof(uint32_t));
    f.reachable  = calloc(f.max_ino, 1);
    f.blk_used   = calloc(f.nblocks ? f.nblocks : 1, 1);
    f.blk_tree   = calloc(f.nblocks ? f.nblocks : 1, 1);
    if (!f.is_inode || !f.ftype || !f.nlink || !f.size || !f.links_seen ||
        !f.subdirs || !f.reachable || !f.blk_used || !f.blk_tree) {
        free(f.is_inode); free(f.ftype); free(f.nlink); free(f.size);
        free(f.links_seen); free(f.subdirs); free(f.reachable);
        free(f.blk_used); free(f.blk_tree);
        return -ENOMEM;
    }
    f.space_view = fs->space;   /* widok mapy (tylko odczyt is_used) */

    int rc = 0;
    /* 1) zbierz wezly drzewa (do detekcji kolizji ekstent<->wezel) */
    if (gh2_btree_walk_nodes(&fs->dev, &fs->fs_root, fsck_tree_block_cb, &f)) { rc = -EIO; goto done; }
    /* 2) zbierz INODE_ITEM, DIR_ITEM, EXTENT_DATA */
    if (gh2_btree_iterate(&fs->dev, &fs->fs_root, fsck_inode_cb2, &f)) { rc = -EIO; goto done; }
    if (gh2_btree_iterate(&fs->dev, &fs->fs_root, fsck_dir_cb2, &f)) { rc = -EIO; goto done; }
    if (gh2_btree_iterate(&fs->dev, &fs->fs_root, fsck_extent_cb, &f)) { rc = -EIO; goto done; }

    /* 3) osiagalnosc z roota */
    f.reachable[GH2_ROOT_INO] = 1;
    if (fsck_reach(fs, &f, GH2_ROOT_INO)) { rc = -EIO; goto done; }

    /* 4) walidacja per ino */
    for (uint64_t ino = 1; ino < f.max_ino; ino++) {
        if (!f.is_inode[ino]) {
            /* brak INODE -> nie moze byc wskazany przez DIR_ITEM (wisienka) */
            if (f.links_seen[ino] != 0) f.issues++;
            continue;
        }
        /* INODE istnieje -> musi byc osiagalny (brak sierot) */
        if (!f.reachable[ino]) f.issues++;

        if (f.ftype[ino] == GH2_FT_DIR) {
            /* nlink == 2 (`.`,`..`) + liczba podkatalogow */
            uint32_t expect = 2 + f.subdirs[ino];
            if (f.nlink[ino] != expect) f.issues++;
        } else if (ino != GH2_ROOT_INO) {
            /* nlink == liczba wpisow wskazujacych nan */
            if (f.nlink[ino] != f.links_seen[ino]) f.issues++;
        }
    }

    /* 5) pass naprawczy (atomowy na lokalnej kopii fs_root; *issues = wykryte powyzej). */
    if (repair && f.issues > 0) {
        struct gh2_bptr root = fs->fs_root;
        struct gh2_txn_savepoint sp = gh2_txn_alloc_mark(&fs->alloc);
        int pr = fsck_repair_pass(fs, &f, &root);
        if (pr == 0 && fs->alloc.oom) pr = -ENOMEM;
        if (pr) { gh2_txn_alloc_rollback(&fs->alloc, sp); rc = pr; goto done; }
        fs->fs_root = root;   /* sukces: caller robi gh2_fs_commit dla trwalosci */
    }

done:
    free(f.is_inode); free(f.ftype); free(f.nlink); free(f.size);
    free(f.links_seen); free(f.subdirs); free(f.reachable);
    free(f.blk_used); free(f.blk_tree);
    if (rc) return rc;
    if (issues) *issues = f.issues;
    return 0;
}
