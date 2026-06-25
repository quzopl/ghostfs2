#ifndef GH2_VFS_H
#define GH2_VFS_H
/* ghostfs v2.6 — fasada VFS nad v1 (gh_fs_*) i v2 (gh2_fs_*).
 *
 * FUSE i CLI wykrywaja magic kontenera (GHOSTFS\x01 = v1, GHOSTFS\x02 = v2) i kieruja
 * operacje do odpowiedniego rdzenia przez wspolny front `struct gfs`. Atrybuty mapowane
 * na neutralny `struct gfs_attr` (te same wartosci type/errno w obu rdzeniach).
 */
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */
#include "fs.h"           /* struct gh_fs (v1) */
#include "block.h"        /* struct gh_dev */
#include "v2/gh2_fs.h"    /* struct gh2_fs (v2) */

/* typy plikow wspolne: GH_xxx oraz GH2_FT_xxx maja te same wartosci 1..7 */
struct gfs_attr {
    uint16_t type;        /* 1=FILE 2=DIR 3=SYMLINK 4=FIFO 5=SOCK 6=CHR 7=BLK */
    uint16_t mode;        /* bity uprawnien (rwx) */
    uint32_t uid, gid, nlink;
    uint64_t size, atime, mtime, ctime, rdev;
};

struct gfs {
    int version;          /* 1 lub 2 */
    struct gh_fs  v1;
    struct gh_dev dev2;   /* nosnik v2 (gh2_fs_mount kopiuje go do v2.dev) */
    struct gh2_fs v2;
};

/* callback readdir fasady: nazwa NUL-terminowana, typ pliku (1..7) */
typedef int (*gfs_readdir_cb)(const char *name, uint16_t type, void *ctx);

/* wykryj wersje kontenera po magicu bloku 0. Zwraca 1, 2 lub -errno. */
int gfs_detect(const char *path);

/* zamontuj (auto-detect). key==NULL: bez hasla / z GHOSTFS_KEY (v1). Zwraca 0 lub -errno
 * (-EACCES gdy v1 zaszyfrowany bez/zlym hasla). */
int  gfs_mount(struct gfs *g, const char *path, const char *key);
void gfs_unmount(struct gfs *g);

/* operacje (przelaczane po g->version) */
int gfs_getattr(struct gfs *g, const char *path, struct gfs_attr *out);
int gfs_create(struct gfs *g, const char *path, uint16_t mode);
int gfs_mkdir(struct gfs *g, const char *path, uint16_t mode);
int gfs_mknod(struct gfs *g, const char *path, uint32_t mode, uint64_t rdev);
int gfs_symlink(struct gfs *g, const char *target, const char *path);
ssize_t gfs_readlink(struct gfs *g, const char *path, char *buf, size_t buflen);
int gfs_link(struct gfs *g, const char *oldpath, const char *newpath);
int gfs_unlink(struct gfs *g, const char *path);
int gfs_rmdir(struct gfs *g, const char *path);
int gfs_rename(struct gfs *g, const char *oldpath, const char *newpath, unsigned flags);
int gfs_chmod(struct gfs *g, const char *path, uint16_t mode);
int gfs_chown(struct gfs *g, const char *path, uint32_t uid, uint32_t gid);
int gfs_utimens(struct gfs *g, const char *path, uint64_t atime, uint64_t mtime);
int gfs_truncate(struct gfs *g, const char *path, uint64_t size);
ssize_t gfs_read(struct gfs *g, const char *path, void *buf, size_t len, uint64_t off);
ssize_t gfs_write(struct gfs *g, const char *path, const void *buf, size_t len, uint64_t off);
int gfs_readdir(struct gfs *g, const char *path, gfs_readdir_cb cb, void *ctx);

/* statfs neutralny */
struct gfs_statfs {
    uint32_t block_size;
    uint64_t total_blocks, free_blocks;
    uint64_t total_inodes, free_inodes;   /* v2: 0/0 (bez stalej puli i-wezlow) */
    uint32_t name_max;
};
int gfs_statfs(struct gfs *g, struct gfs_statfs *out);

/* trwalosc: v1 gh_fs_sync / v2 gh2_fs_commit */
int gfs_sync(struct gfs *g);

/* ---- snapshoty (tylko v2; v1 -> -ENOTSUP) ---- */
/* utworz snapshot aktywnego subwolumenu pod nazwa `name`. */
int gfs_snapshot(struct gfs *g, const char *name);
/* lista subwolumenow (id + nazwa). cb jak gh2_subvol_cb. */
typedef int (*gfs_subvol_cb)(uint64_t id, const char *name, void *ctx);
int gfs_subvol_list(struct gfs *g, gfs_subvol_cb cb, void *ctx);
/* usun subwolumen po id (snapshot). */
int gfs_subvol_delete(struct gfs *g, uint64_t subvol_id);

/* fsck. repair!=0: v1 i v2 wykonuja trwale naprawy (gh_fsck/gh2_fsck repair; caller robi gfs_sync
 * dla trwalosci). *issues = liczba niespojnosci (0 = OK). Zwraca 0 lub -errno przy bledzie I/O. */
int gfs_fsck(struct gfs *g, int repair, int *issues);

/* xattr: v1 -> rdzen; v2 -> -ENOTSUP */
int     gfs_setxattr(struct gfs *g, const char *path, const char *name,
                     const void *val, size_t size, int flags);
ssize_t gfs_getxattr(struct gfs *g, const char *path, const char *name,
                     void *buf, size_t size);
ssize_t gfs_listxattr(struct gfs *g, const char *path, char *buf, size_t size);
int     gfs_removexattr(struct gfs *g, const char *path, const char *name);

#endif
