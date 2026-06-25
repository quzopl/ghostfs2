#include "inode.h"
#include <string.h>
#include <time.h>
#include <errno.h>

static void inode_loc(const struct gh_superblock *sb, uint64_t ino,
                      uint64_t *blk, uint64_t *idx) {
    *blk = sb->inode_start + ino / GH_INODES_PER_BLK;
    *idx = ino % GH_INODES_PER_BLK;
}

int gh_inode_read(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t ino, struct gh_inode *out) {
    if (ino >= sb->inode_count) return -EINVAL;
    uint64_t blk, idx; inode_loc(sb, ino, &blk, &idx);
    uint8_t buf[GH_BLOCK_SIZE];
    int r = gh_block_read(dev, blk, buf); if (r) return r;
    memcpy(out, buf + idx * GH_INODE_SIZE, sizeof(*out));
    return 0;
}

int gh_inode_write(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t ino, const struct gh_inode *in) {
    if (ino >= sb->inode_count) return -EINVAL;
    uint64_t blk, idx; inode_loc(sb, ino, &blk, &idx);
    uint8_t buf[GH_BLOCK_SIZE];
    int r = gh_block_read(dev, blk, buf); if (r) return r;
    memcpy(buf + idx * GH_INODE_SIZE, in, sizeof(*in));
    return gh_block_write(dev, blk, buf);
}

int gh_inode_alloc(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint16_t type, uint64_t *out_ino) {
    uint64_t start = dev->hint_inode;
    if (start < GH_ROOT_INO + 1 || start >= sb->inode_count) start = GH_ROOT_INO + 1;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t lo = (pass == 0) ? start : (uint64_t)(GH_ROOT_INO + 1);
        uint64_t hi = (pass == 0) ? sb->inode_count : start;
        for (uint64_t ino = lo; ino < hi; ino++) {
            struct gh_inode n; int r = gh_inode_read(dev, sb, ino, &n);
            if (r) return r;
            if (n.type == GH_FREE) {
                memset(&n, 0, sizeof(n));
                n.type = type; n.mode = (type == GH_DIR) ? 0755 : 0644;
                n.nlink = 1; n.atime = n.mtime = n.ctime = (uint64_t)time(NULL);
                r = gh_inode_write(dev, sb, ino, &n); if (r) return r;
                dev->hint_inode = ino + 1;
                *out_ino = ino; return 0;
            }
        }
    }
    return -ENOSPC;
}

/* mapuje logiczny numer bloku pliku na fizyczny; alokuje gdy alloc!=0.
   leaf_direct: nowy blok liscia (danych pliku) alokowany BEZ zerowania (gh_alloc_block_nz)
   i *out_newleaf=1 -> caller zapisze pelny blok bezposrednio. Bloki WSKAZNIKOWE
   (indirect/double/l1) ZAWSZE gh_alloc_block (zerowane, journalowane). */
static int bmap(struct gh_dev *dev, const struct gh_superblock *sb,
                struct gh_inode *node, uint64_t lbn, int alloc, uint64_t *out,
                int leaf_direct, int *out_newleaf) {
    if (out_newleaf) *out_newleaf = 0;
    if (lbn < GH_NDIRECT) {
        if (node->direct[lbn] == 0) {
            if (!alloc) { *out = 0; return 0; }
            int r = leaf_direct ? gh_alloc_block_nz(dev, sb, &node->direct[lbn])
                                : gh_alloc_block(dev, sb, &node->direct[lbn]);
            if (r) return r;
            if (out_newleaf) *out_newleaf = 1;
        }
        *out = node->direct[lbn]; return 0;
    }
    lbn -= GH_NDIRECT;
    if (lbn < GH_PTRS_PER_BLK) {
        if (node->indirect == 0) {
            if (!alloc) { *out = 0; return 0; }
            int r = gh_alloc_block(dev, sb, &node->indirect); if (r) return r;   /* wskaznikowy: zerowany */
        }
        uint64_t ptrs[GH_PTRS_PER_BLK];
        int r = gh_block_read(dev, node->indirect, ptrs); if (r) return r;
        if (ptrs[lbn] == 0) {
            if (!alloc) { *out = 0; return 0; }
            r = leaf_direct ? gh_alloc_block_nz(dev, sb, &ptrs[lbn])
                            : gh_alloc_block(dev, sb, &ptrs[lbn]);
            if (r) return r;
            if (out_newleaf) *out_newleaf = 1;
            r = gh_block_write(dev, node->indirect, ptrs); if (r) return r;
        }
        *out = ptrs[lbn]; return 0;
    }
    lbn -= GH_PTRS_PER_BLK;
    /* double indirect */
    if (lbn < (uint64_t)GH_PTRS_PER_BLK * GH_PTRS_PER_BLK) {
        if (node->double_indirect == 0) {
            if (!alloc) { *out = 0; return 0; }
            int r = gh_alloc_block(dev, sb, &node->double_indirect); if (r) return r;   /* wskaznikowy */
        }
        uint64_t l1[GH_PTRS_PER_BLK];
        int r = gh_block_read(dev, node->double_indirect, l1); if (r) return r;
        uint64_t i1 = lbn / GH_PTRS_PER_BLK, i2 = lbn % GH_PTRS_PER_BLK;
        if (l1[i1] == 0) {
            if (!alloc) { *out = 0; return 0; }
            r = gh_alloc_block(dev, sb, &l1[i1]); if (r) return r;   /* wskaznikowy: zerowany */
            r = gh_block_write(dev, node->double_indirect, l1); if (r) return r;
        }
        uint64_t l2[GH_PTRS_PER_BLK];
        r = gh_block_read(dev, l1[i1], l2); if (r) return r;
        if (l2[i2] == 0) {
            if (!alloc) { *out = 0; return 0; }
            r = leaf_direct ? gh_alloc_block_nz(dev, sb, &l2[i2])
                            : gh_alloc_block(dev, sb, &l2[i2]);
            if (r) return r;
            if (out_newleaf) *out_newleaf = 1;
            r = gh_block_write(dev, l1[i1], l2); if (r) return r;
        }
        *out = l2[i2]; return 0;
    }
    return -EFBIG;
}

ssize_t gh_inode_pread(struct gh_dev *dev, const struct gh_superblock *sb,
                       struct gh_inode *node, void *buf, size_t n, uint64_t off) {
    if (off >= node->size) return 0;
    if (off + n > node->size) n = node->size - off;
    size_t done = 0; uint8_t blk[GH_BLOCK_SIZE];
    while (done < n) {
        uint64_t lbn = (off + done) / GH_BLOCK_SIZE;
        uint64_t boff = (off + done) % GH_BLOCK_SIZE;
        size_t chunk = GH_BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        uint64_t phys; int r = bmap(dev, sb, node, lbn, 0, &phys, 0, NULL);
        if (r) return r;
        if (phys == 0) memset(blk, 0, GH_BLOCK_SIZE);     /* dziura = zera */
        else { r = gh_block_read(dev, phys, blk); if (r) return r; }
        memcpy((uint8_t*)buf + done, blk + boff, chunk);
        done += chunk;
    }
    return (ssize_t)done;
}

ssize_t gh_inode_pwrite(struct gh_dev *dev, const struct gh_superblock *sb,
                        uint64_t ino, struct gh_inode *node,
                        const void *buf, size_t n, uint64_t off) {
    size_t done = 0; uint8_t blk[GH_BLOCK_SIZE];
    int direct = (node->type == GH_FILE);     /* tylko dane zwyklych plikow */
    while (done < n) {
        uint64_t lbn = (off + done) / GH_BLOCK_SIZE;
        uint64_t boff = (off + done) % GH_BLOCK_SIZE;
        size_t chunk = GH_BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        int newleaf = 0;
        uint64_t phys; int r = bmap(dev, sb, node, lbn, 1, &phys, direct, &newleaf);
        if (r) return r;
        if (direct && newleaf) {
            /* nowy blok pliku: zbuduj pelny blok (zera+dane), zapisz BEZPOSREDNIO (bez RMW) */
            if (chunk != GH_BLOCK_SIZE) memset(blk, 0, GH_BLOCK_SIZE);
            memcpy(blk + boff, (const uint8_t*)buf + done, chunk);
            r = gh_block_write_direct(dev, phys, blk); if (r) return r;
        } else {
            /* nadpisanie istniejacego / katalog / inne: jak dotad (journalowane) */
            if (chunk != GH_BLOCK_SIZE) { r = gh_block_read(dev, phys, blk); if (r) return r; }
            memcpy(blk + boff, (const uint8_t*)buf + done, chunk);
            r = gh_block_write(dev, phys, blk); if (r) return r;
        }
        done += chunk;
    }
    if (off + n > node->size) node->size = off + n;
    node->mtime = (uint64_t)time(NULL);
    int r = gh_inode_write(dev, sb, ino, node); if (r) return r;
    return (ssize_t)done;
}

int gh_inode_free(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t ino) {
    struct gh_inode n; int r = gh_inode_read(dev, sb, ino, &n); if (r) return r;
    int first_err = 0;
    /* wezly specjalne (FIFO/SOCK/CHR/BLK) nie maja blokow danych; direct[0]
     * urzadzenia to rdev — NIE wolno go traktowac jak numer bloku */
    int has_blocks = (n.type == GH_FILE || n.type == GH_DIR || n.type == GH_SYMLINK);
    if (has_blocks) {
    /* bezpośrednie */
    for (int i = 0; i < GH_NDIRECT; i++)
        if (n.direct[i]) { int e = gh_free_block(dev, sb, n.direct[i]); if (e && !first_err) first_err = e; }
    /* pojedynczo pośrednie */
    if (n.indirect) {
        uint64_t p[GH_PTRS_PER_BLK];
        if (gh_block_read(dev, n.indirect, p) == 0)
            for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++)
                if (p[i]) { int e = gh_free_block(dev, sb, p[i]); if (e && !first_err) first_err = e; }
        int e = gh_free_block(dev, sb, n.indirect); if (e && !first_err) first_err = e;
    }
    /* podwójnie pośrednie */
    if (n.double_indirect) {
        uint64_t l1[GH_PTRS_PER_BLK];
        if (gh_block_read(dev, n.double_indirect, l1) == 0) {
            for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++) {
                if (!l1[i]) continue;
                uint64_t l2[GH_PTRS_PER_BLK];
                if (gh_block_read(dev, l1[i], l2) == 0)
                    for (uint64_t j = 0; j < GH_PTRS_PER_BLK; j++)
                        if (l2[j]) { int e = gh_free_block(dev, sb, l2[j]); if (e && !first_err) first_err = e; }
                int e = gh_free_block(dev, sb, l1[i]); if (e && !first_err) first_err = e;
            }
        }
        int e = gh_free_block(dev, sb, n.double_indirect); if (e && !first_err) first_err = e;
    }
    }
    /* xattr_block zwalniany ZAWSZE — wezly specjalne tez moga miec xattr */
    {
        uint64_t xb = n.xattr_block; uint64_t guard = 0;
        while (xb) {
            if (guard >= sb->total_blocks) {   /* ochrona przed cyklem */
                if (!first_err) first_err = -EIO;
                break;
            }
            uint8_t xblk[GH_BLOCK_SIZE]; uint64_t nx = 0;
            if (gh_block_read(dev, xb, xblk) == 0) memcpy(&nx, xblk, 8);
            int e = gh_free_block(dev, sb, xb); if (e && !first_err) first_err = e;
            xb = nx; guard++;
        }
    }
    memset(&n, 0, sizeof(n)); n.type = GH_FREE;
    r = gh_inode_write(dev, sb, ino, &n);
    if (ino < dev->hint_inode) dev->hint_inode = ino;
    return r ? r : first_err;
}

/* zwolnij wszystkie liscie wskazywane przez blok posredni + sam blok */
static int free_indirect_full(struct gh_dev *dev, const struct gh_superblock *sb,
                              uint64_t blk) {
    int first_err = 0;
    uint64_t p[GH_PTRS_PER_BLK];
    int r = gh_block_read(dev, blk, p);
    if (r) { first_err = r; }
    else
        for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++)
            if (p[i]) { int e = gh_free_block(dev, sb, p[i]); if (e && !first_err) first_err = e; }
    int e = gh_free_block(dev, sb, blk); if (e && !first_err) first_err = e;
    return first_err;
}

/* zwolnij liscie bloku posredniego od indeksu `from`, wyzeruj wskazniki */
static int free_indirect_from(struct gh_dev *dev, const struct gh_superblock *sb,
                              uint64_t blk, uint64_t from) {
    uint64_t p[GH_PTRS_PER_BLK];
    int r = gh_block_read(dev, blk, p); if (r) return r;
    int first_err = 0, dirty = 0;
    for (uint64_t i = from; i < GH_PTRS_PER_BLK; i++)
        if (p[i]) { int e = gh_free_block(dev, sb, p[i]); if (e && !first_err) first_err = e; p[i] = 0; dirty = 1; }
    if (dirty) { int e = gh_block_write(dev, blk, p); if (e && !first_err) first_err = e; }
    return first_err;
}

int gh_inode_truncate(struct gh_dev *dev, const struct gh_superblock *sb,
                      uint64_t ino, struct gh_inode *node, uint64_t new_size) {
    uint64_t old_size = node->size;
    uint64_t new_nblk = (new_size + GH_BLOCK_SIZE - 1) / GH_BLOCK_SIZE;
    int first_err = 0;

    if (new_size < old_size) {
        /* bezpośrednie */
        for (uint64_t i = (new_nblk < GH_NDIRECT ? new_nblk : GH_NDIRECT);
             i < GH_NDIRECT; i++)
            if (node->direct[i]) {
                int e = gh_free_block(dev, sb, node->direct[i]); if (e && !first_err) first_err = e;
                node->direct[i] = 0;
            }

        /* pojedynczo pośrednie: zakres [NDIRECT, NDIRECT+PTRS) */
        if (node->indirect) {
            if (new_nblk <= GH_NDIRECT) {
                int e = free_indirect_full(dev, sb, node->indirect); if (e && !first_err) first_err = e;
                node->indirect = 0;
            } else if (new_nblk < GH_NDIRECT + GH_PTRS_PER_BLK) {
                int e = free_indirect_from(dev, sb, node->indirect, new_nblk - GH_NDIRECT);
                if (e && !first_err) first_err = e;
            }
        }

        /* podwójnie pośrednie */
        if (node->double_indirect) {
            uint64_t dstart = GH_NDIRECT + GH_PTRS_PER_BLK;
            if (new_nblk <= dstart) {
                uint64_t l1[GH_PTRS_PER_BLK];
                int r = gh_block_read(dev, node->double_indirect, l1);
                if (r) { if (!first_err) first_err = r; }
                else
                    for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++)
                        if (l1[i]) { int e = free_indirect_full(dev, sb, l1[i]); if (e && !first_err) first_err = e; }
                int e = gh_free_block(dev, sb, node->double_indirect); if (e && !first_err) first_err = e;
                node->double_indirect = 0;
            } else {
                uint64_t rel = new_nblk - dstart;
                uint64_t i1_start = rel / GH_PTRS_PER_BLK;
                uint64_t i2_start = rel % GH_PTRS_PER_BLK;
                uint64_t l1[GH_PTRS_PER_BLK];
                int r = gh_block_read(dev, node->double_indirect, l1);
                if (r) { if (!first_err) first_err = r; }
                else {
                    int l1dirty = 0;
                    for (uint64_t i1 = i1_start; i1 < GH_PTRS_PER_BLK; i1++) {
                        if (!l1[i1]) continue;
                        uint64_t from = (i1 == i1_start) ? i2_start : 0;
                        if (from == 0) {
                            int e = free_indirect_full(dev, sb, l1[i1]); if (e && !first_err) first_err = e;
                            l1[i1] = 0; l1dirty = 1;
                        } else {
                            int e = free_indirect_from(dev, sb, l1[i1], from); if (e && !first_err) first_err = e;
                        }
                    }
                    if (l1dirty) { int e = gh_block_write(dev, node->double_indirect, l1); if (e && !first_err) first_err = e; }
                }
            }
        }

        /* zeruj ogon ostatniego zachowanego bloku */
        uint64_t tail = new_size % GH_BLOCK_SIZE;
        if (tail != 0 && new_nblk > 0) {
            uint64_t phys;
            if (bmap(dev, sb, node, new_nblk - 1, 0, &phys, 0, NULL) == 0 && phys) {
                uint8_t blk[GH_BLOCK_SIZE];
                if (gh_block_read(dev, phys, blk) == 0) {
                    memset(blk + tail, 0, GH_BLOCK_SIZE - tail);
                    int e = gh_block_write(dev, phys, blk); if (e && !first_err) first_err = e;
                }
            }
        }
    }

    node->size = new_size;
    node->mtime = node->ctime = (uint64_t)time(NULL);
    int r = gh_inode_write(dev, sb, ino, node);
    return r ? r : first_err;
}
