#ifndef GH_DIR_H
#define GH_DIR_H
#include "ghostfs.h"
#include "block.h"
#include "inode.h"
typedef int (*gh_dir_iter_fn)(const struct gh_dirent*, void*);
int gh_dir_add(struct gh_dev*, const struct gh_superblock*, uint64_t dir_ino, const char *name, uint64_t ino);
int gh_dir_lookup(struct gh_dev*, const struct gh_superblock*, uint64_t dir_ino, const char *name, uint64_t *out_ino);
int gh_dir_remove(struct gh_dev*, const struct gh_superblock*, uint64_t dir_ino, const char *name);
int gh_dir_set_ino(struct gh_dev*, const struct gh_superblock*, uint64_t dir_ino, const char *name, uint64_t new_ino);
int gh_dir_is_empty(struct gh_dev*, const struct gh_superblock*, uint64_t dir_ino, int *empty);
int gh_dir_iterate(struct gh_dev*, const struct gh_superblock*, uint64_t dir_ino, gh_dir_iter_fn cb, void *ctx);
int gh_path_resolve(struct gh_dev*, const struct gh_superblock*, const char *path, uint64_t *out_ino);
int gh_path_split(const char *path, char *parent, char *name);
#endif
