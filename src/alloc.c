#include "alloc.h"
#include <string.h>
#include <stdlib.h>

static int bitmap_rw(struct gh_dev *dev, const struct gh_superblock *sb,
                     uint64_t blkno, int set_op, int do_write, int *out_set) {
    uint64_t bit = blkno;
    uint64_t byte = bit / 8;
    uint64_t blk_in_map = sb->bitmap_start + byte / GH_BLOCK_SIZE;
    uint64_t off = byte % GH_BLOCK_SIZE;
    uint8_t buf[GH_BLOCK_SIZE];
    int r = gh_block_read(dev, blk_in_map, buf);
    if (r) return r;
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    if (out_set) *out_set = (buf[off] & mask) ? 1 : 0;
    if (do_write) {
        if (set_op) buf[off] |= mask; else buf[off] &= (uint8_t)~mask;
        return gh_block_write(dev, blk_in_map, buf);
    }
    return 0;
}

int gh_bitmap_test(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t blkno, int *set) {
    if (!set) return -EINVAL;
    return bitmap_rw(dev, sb, blkno, 0, 0, set);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int gh_discard_flush(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (dev->nd == 0) return 0;
    /* zostaw tylko bloki wciaz wolne (ochrona przed realokacja) */
    uint32_t w = 0;
    for (uint32_t i = 0; i < dev->nd; i++) {
        int set = 0;
        if (gh_bitmap_test(dev, sb, dev->discards[i], &set) == 0 && !set)
            dev->discards[w++] = dev->discards[i];
    }
    dev->nd = w;
    if (w == 0) return 0;
    qsort(dev->discards, w, sizeof(uint64_t), cmp_u64);
    uint32_t i = 0;
    while (i < w) {
        uint64_t start = dev->discards[i], cnt = 1;
        while (i + cnt < w && dev->discards[i + cnt] == start + cnt) cnt++;
        gh_disk_discard(dev, start, cnt);     /* best-effort */
        i += (uint32_t)cnt;
    }
    dev->nd = 0;
    return 0;
}

int gh_free_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t blkno) {
    if (blkno < sb->data_start || blkno >= sb->total_blocks) return -EINVAL;
    if (blkno < dev->hint_block) dev->hint_block = blkno;
    int r = bitmap_rw(dev, sb, blkno, 0, 1, NULL);
    if (r) return r;
    if (dev->txn && dev->txn->active) gh_discard_pending_add(dev, blkno);  /* odroczenie */
    else gh_disk_discard(dev, blkno, 1);                                   /* natychmiast */
    return 0;
}

static int alloc_impl(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out, int zero) {
    if (!out) return -EINVAL;
    uint64_t start = dev->hint_block;
    if (start < sb->data_start || start >= sb->total_blocks) start = sb->data_start;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t lo = (pass == 0) ? start : sb->data_start;
        uint64_t hi = (pass == 0) ? sb->total_blocks : start;
        for (uint64_t b = lo; b < hi; b++) {
            int set = 0; int r = gh_bitmap_test(dev, sb, b, &set); if (r) return r;
            if (!set) {
                r = bitmap_rw(dev, sb, b, 1, 1, NULL); if (r) return r;
                if (zero) {
                    uint8_t z[GH_BLOCK_SIZE]; memset(z, 0, sizeof(z));
                    r = gh_block_write(dev, b, z); if (r) return r;
                }
                dev->hint_block = b + 1;
                *out = b; return 0;
            }
        }
    }
    return -ENOSPC;
}
int gh_alloc_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out) {
    return alloc_impl(dev, sb, out, 1);
}
int gh_alloc_block_nz(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out) {
    return alloc_impl(dev, sb, out, 0);   /* bez zerowania: caller pisze pelny blok bezposrednio */
}
