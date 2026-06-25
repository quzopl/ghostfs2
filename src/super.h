#ifndef GH_SUPER_H
#define GH_SUPER_H
#include "ghostfs.h"
#include "block.h"
#include <errno.h>
int gh_format(const char *path, uint64_t total_blocks, uint64_t inode_count);
int gh_format_enc(const char *path, uint64_t total_blocks, uint64_t inode_count,
                  const char *passphrase);
int gh_mount_sb(struct gh_dev *dev, struct gh_superblock *sb);
#endif
