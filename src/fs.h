#ifndef GH_FS_H
#define GH_FS_H
#include "ghostfs.h"
#include "block.h"
#include "super.h"
#include "inode.h"
#include "dir.h"
#include "alloc.h"
#include "xattr.h"
struct gh_fs { struct gh_dev dev; struct gh_superblock sb; };
int gh_fs_mount(struct gh_fs*, const char *path);
int gh_fs_mount_key(struct gh_fs*, const char *path, const char *passphrase);
void gh_fs_unmount(struct gh_fs*);
int gh_fs_getattr(struct gh_fs*, const char *path, struct gh_inode *out, uint64_t *out_ino);
int gh_fs_create(struct gh_fs*, const char *path, uint16_t mode);
int gh_fs_mkdir(struct gh_fs*, const char *path, uint16_t mode);
int gh_fs_mknod(struct gh_fs*, const char *path, uint16_t mode, uint64_t rdev);
int gh_fs_unlink(struct gh_fs*, const char *path);
int gh_fs_rmdir(struct gh_fs*, const char *path);
ssize_t gh_fs_read(struct gh_fs*, const char *path, void *buf, size_t n, uint64_t off);
ssize_t gh_fs_write(struct gh_fs*, const char *path, const void *buf, size_t n, uint64_t off);
int gh_fs_readdir(struct gh_fs*, const char *path, gh_dir_iter_fn cb, void *ctx);
int gh_fs_truncate(struct gh_fs*, const char *path, uint64_t new_size);
int gh_fs_utimens(struct gh_fs*, const char *path, uint64_t atime, uint64_t mtime);
int gh_fs_chmod(struct gh_fs*, const char *path, uint16_t mode);
int gh_fs_chown(struct gh_fs*, const char *path, uint32_t uid, uint32_t gid);
int gh_fs_statfs(struct gh_fs*, struct gh_statfs *out);
int gh_fs_sync(struct gh_fs*);
int gh_fs_link(struct gh_fs*, const char *oldpath, const char *newpath);
int gh_fs_rename(struct gh_fs*, const char *oldpath, const char *newpath);
#define GH_RENAME_NOREPLACE 0x1u
#define GH_RENAME_EXCHANGE  0x2u
int gh_fs_rename2(struct gh_fs*, const char *oldpath, const char *newpath, unsigned flags);
int     gh_fs_symlink(struct gh_fs*, const char *target, const char *linkpath);
ssize_t gh_fs_readlink(struct gh_fs*, const char *path, char *buf, size_t size);
int     gh_fs_setxattr(struct gh_fs*, const char *path, const char *name,
                       const void *val, size_t size, int flags);
ssize_t gh_fs_getxattr(struct gh_fs*, const char *path, const char *name,
                       void *buf, size_t size);
ssize_t gh_fs_listxattr(struct gh_fs*, const char *path, char *buf, size_t size);
int     gh_fs_removexattr(struct gh_fs*, const char *path, const char *name);
int gh_fsck(struct gh_fs*, int repair, int *issues);
#endif
