#ifndef GH_ALLOC_H
#define GH_ALLOC_H
#include "ghostfs.h"
#include "block.h"
#include <errno.h>
int gh_alloc_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out);
int gh_alloc_block_nz(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t *out);
int gh_free_block(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t blkno);
int gh_bitmap_test(struct gh_dev *dev, const struct gh_superblock *sb, uint64_t blkno, int *set);
#endif
