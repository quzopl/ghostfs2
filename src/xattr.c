#include "xattr.h"
#include <string.h>
#include <errno.h>

#define XREC_OFF 8                            /* rekordy po 8-bajtowym wskazniku next */
#define XREC_CAP (GH_BLOCK_SIZE - XREC_OFF)   /* pojemnosc rekordow w bloku */

static uint64_t next_of(const uint8_t *blk) { uint64_t n; memcpy(&n, blk, 8); return n; }
static void set_next(uint8_t *blk, uint64_t n) { memcpy(blk, &n, 8); }

/* znajdz nazwe w obrebie bloku; zwroc offset rekordu lub -1; ustaw *vlen,*voff */
static long find_in_block(const uint8_t *blk, const char *name, uint16_t *vlen, size_t *voff) {
    size_t off = XREC_OFF, nlen = strlen(name);
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        if (nl == nlen && memcmp(blk + off + 4, name, nl) == 0) {
            *vlen = vl; *voff = off + 4 + nl; return (long)off;
        }
        off += 4 + (size_t)nl + vl;
    }
    return -1;
}

static size_t used_in_block(const uint8_t *blk) {
    size_t off = XREC_OFF;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
        if (nl == 0) break;
        if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
        off += 4 + (size_t)nl + vl;
    }
    return off - XREC_OFF;
}

/* usun rekord na offsecie `pos`, kompaktujac obszar rekordow (next nietkniety) */
static void remove_record(uint8_t *blk, long pos) {
    uint8_t tmp[GH_BLOCK_SIZE]; memcpy(tmp, blk, GH_BLOCK_SIZE);
    size_t w = XREC_OFF, off = XREC_OFF;
    while (off + 4 <= GH_BLOCK_SIZE) {
        uint16_t nl, vl; memcpy(&nl, tmp + off, 2); memcpy(&vl, tmp + off + 2, 2);
        if (nl == 0) break;
        size_t rec = 4 + (size_t)nl + vl;
        if (off + rec > GH_BLOCK_SIZE) break;
        if ((long)off != pos) { memcpy(blk + w, tmp + off, rec); w += rec; }
        off += rec;
    }
    memset(blk + w, 0, GH_BLOCK_SIZE - w);
}

int gh_xattr_set(struct gh_dev *dev, const struct gh_superblock *sb, struct gh_inode *node,
                 uint64_t ino, const char *name, const void *val, size_t vlen, int flags) {
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 0xFFFF || vlen > 0xFFFF) return -EINVAL;
    size_t need = 4 + nlen + vlen;
    if (need > XREC_CAP) return -E2BIG;

    uint8_t blk[GH_BLOCK_SIZE];
    int exists = 0;
    uint64_t guard = 0;
    /* usun istniejacy rekord (jesli jest) */
    for (uint64_t b = node->xattr_block; b; ) {
        int r = gh_block_read(dev, b, blk); if (r) return r;
        uint16_t vl; size_t voff; long pos = find_in_block(blk, name, &vl, &voff);
        if (pos >= 0) {
            exists = 1;
            if (flags & GH_XATTR_CREATE) return -EEXIST;
            remove_record(blk, pos);
            r = gh_block_write(dev, b, blk); if (r) return r;
            break;
        }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    if ((flags & GH_XATTR_REPLACE) && !exists) return -ENODATA;

    /* znajdz blok z miejscem */
    guard = 0;
    for (uint64_t b = node->xattr_block; b; ) {
        int r = gh_block_read(dev, b, blk); if (r) return r;
        size_t used = used_in_block(blk);
        if (used + need <= XREC_CAP) {
            size_t pos = XREC_OFF + used;
            uint16_t nl16 = (uint16_t)nlen, vl16 = (uint16_t)vlen;
            memcpy(blk + pos, &nl16, 2); memcpy(blk + pos + 2, &vl16, 2);
            memcpy(blk + pos + 4, name, nlen); memcpy(blk + pos + 4 + nlen, val, vlen);
            return gh_block_write(dev, b, blk);
        }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    /* brak miejsca -> nowy blok jako glowa */
    uint64_t old_head = node->xattr_block;
    uint64_t nb; int r = gh_alloc_block(dev, sb, &nb); if (r) return r;
    memset(blk, 0, sizeof(blk));
    set_next(blk, old_head);
    uint16_t nl16 = (uint16_t)nlen, vl16 = (uint16_t)vlen;
    memcpy(blk + XREC_OFF, &nl16, 2); memcpy(blk + XREC_OFF + 2, &vl16, 2);
    memcpy(blk + XREC_OFF + 4, name, nlen); memcpy(blk + XREC_OFF + 4 + nlen, val, vlen);
    if ((r = gh_block_write(dev, nb, blk))) { gh_free_block(dev, sb, nb); return r; }
    node->xattr_block = nb;
    r = gh_inode_write(dev, sb, ino, node);
    if (r) { node->xattr_block = old_head; gh_free_block(dev, sb, nb); return r; }
    return 0;
}

ssize_t gh_xattr_get(struct gh_dev *dev, const struct gh_superblock *sb,
                     const struct gh_inode *node, const char *name, void *buf, size_t size) {
    uint8_t blk[GH_BLOCK_SIZE];
    uint64_t guard = 0;
    for (uint64_t b = node->xattr_block; b; ) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        uint16_t vl; size_t voff; long pos = find_in_block(blk, name, &vl, &voff);
        if (pos >= 0) {
            if (size == 0) return vl;
            if (size < vl) return -ERANGE;
            memcpy(buf, blk + voff, vl); return vl;
        }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    return -ENODATA;
}

ssize_t gh_xattr_list(struct gh_dev *dev, const struct gh_superblock *sb,
                      const struct gh_inode *node, char *buf, size_t size) {
    uint8_t blk[GH_BLOCK_SIZE];
    size_t need = 0;
    uint64_t guard = 0;
    for (uint64_t b = node->xattr_block; b; ) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        size_t off = XREC_OFF;
        while (off + 4 <= GH_BLOCK_SIZE) {
            uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
            if (nl == 0) break;
            if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
            need += (size_t)nl + 1;
            off += 4 + (size_t)nl + vl;
        }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    if (size == 0) return (ssize_t)need;
    if (size < need) return -ERANGE;
    size_t w = 0;
    guard = 0;
    for (uint64_t b = node->xattr_block; b; ) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        size_t off = XREC_OFF;
        while (off + 4 <= GH_BLOCK_SIZE) {
            uint16_t nl, vl; memcpy(&nl, blk + off, 2); memcpy(&vl, blk + off + 2, 2);
            if (nl == 0) break;
            if (off + 4 + (size_t)nl + vl > GH_BLOCK_SIZE) break;
            memcpy(buf + w, blk + off + 4, nl); w += nl; buf[w++] = '\0';
            off += 4 + (size_t)nl + vl;
        }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    return (ssize_t)need;
}

int gh_xattr_remove(struct gh_dev *dev, const struct gh_superblock *sb, struct gh_inode *node,
                    uint64_t ino, const char *name) {
    uint8_t blk[GH_BLOCK_SIZE];
    int found = 0;
    uint64_t guard = 0;
    for (uint64_t b = node->xattr_block; b; ) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        uint16_t vl; size_t voff; long pos = find_in_block(blk, name, &vl, &voff);
        if (pos >= 0) {
            remove_record(blk, pos);
            if (gh_block_write(dev, b, blk)) return -EIO;
            found = 1; break;
        }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    if (!found) return -ENODATA;
    /* caly lancuch pusty? -> zwolnij i wyzeruj xattr_block */
    int all_empty = 1;
    guard = 0;
    for (uint64_t b = node->xattr_block; b; ) {
        if (gh_block_read(dev, b, blk)) return -EIO;
        if (used_in_block(blk) > 0) { all_empty = 0; break; }
        if (++guard > sb->total_blocks) return -EIO;   /* ochrona przed cyklem */
        b = next_of(blk);
    }
    if (all_empty) {
        uint64_t b = node->xattr_block; guard = 0;
        while (b && guard < sb->total_blocks) {
            if (gh_block_read(dev, b, blk)) break;
            uint64_t nx = next_of(blk);
            gh_free_block(dev, sb, b);
            b = nx; guard++;
        }
        node->xattr_block = 0;
        return gh_inode_write(dev, sb, ino, node);
    }
    return 0;
}
