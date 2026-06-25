#ifndef GH_XATTR_H
#define GH_XATTR_H
#include "ghostfs.h"
#include "block.h"
#include "inode.h"
#include "alloc.h"
#include <sys/types.h>

#define GH_XATTR_CREATE  1   /* zawiedź gdy istnieje */
#define GH_XATTR_REPLACE 2   /* zawiedź gdy nie istnieje */

int     gh_xattr_set(struct gh_dev*, const struct gh_superblock*, struct gh_inode *node,
                     uint64_t ino, const char *name, const void *val, size_t vlen, int flags);
ssize_t gh_xattr_get(struct gh_dev*, const struct gh_superblock*, const struct gh_inode *node,
                     const char *name, void *buf, size_t size);
ssize_t gh_xattr_list(struct gh_dev*, const struct gh_superblock*, const struct gh_inode *node,
                      char *buf, size_t size);
int     gh_xattr_remove(struct gh_dev*, const struct gh_superblock*, struct gh_inode *node,
                        uint64_t ino, const char *name);
#endif
