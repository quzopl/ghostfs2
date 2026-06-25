#ifndef GH_INODE_H
#define GH_INODE_H
#include "ghostfs.h"
#include "block.h"
#include "alloc.h"
#include <sys/types.h>
int gh_inode_read(struct gh_dev*, const struct gh_superblock*, uint64_t ino, struct gh_inode*);
int gh_inode_write(struct gh_dev*, const struct gh_superblock*, uint64_t ino, const struct gh_inode*);
int gh_inode_alloc(struct gh_dev*, const struct gh_superblock*, uint16_t type, uint64_t *out_ino);
int gh_inode_free(struct gh_dev*, const struct gh_superblock*, uint64_t ino);
ssize_t gh_inode_pread(struct gh_dev*, const struct gh_superblock*, struct gh_inode*, void*, size_t, uint64_t);
ssize_t gh_inode_pwrite(struct gh_dev*, const struct gh_superblock*, uint64_t ino, struct gh_inode*, const void*, size_t, uint64_t);
int gh_inode_truncate(struct gh_dev*, const struct gh_superblock*, uint64_t ino,
                      struct gh_inode *node, uint64_t new_size);
#endif
