#include "v2/gh2_vfs.h"
#include "super.h"
#include "v2/gh2_super.h"
#include "v2/gh2_format.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ============================ detekcja wersji ============================ */

int gfs_detect(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    uint8_t buf[GH2_MAGIC_LEN];
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    int saved = errno;
    close(fd);
    if (n != (ssize_t)sizeof(buf)) return n < 0 ? -saved : -EINVAL;
    if (memcmp(buf, GH_MAGIC,  GH_MAGIC_LEN)  == 0) return 1;
    if (memcmp(buf, GH2_MAGIC, GH2_MAGIC_LEN) == 0) return 2;
    return -EINVAL;
}

/* ============================ mount / unmount ============================ */

int gfs_mount(struct gfs *g, const char *path, const char *key) {
    memset(g, 0, sizeof(*g));
    int v = gfs_detect(path);
    if (v < 0) return v;
    g->version = v;
    if (v == 1) {
        return key ? gh_fs_mount_key(&g->v1, path, key)
                   : gh_fs_mount(&g->v1, path);
    }
    /* v2: gh_dev_open + bcache + gh2_fs_mount_key (v2enc). key z GHOSTFS_KEY/prompt (CLI/FUSE).
     * Zaszyfrowany kontener bez/zlym kluczem -> -EACCES. */
    int r = gh_dev_open(path, &g->dev2);
    if (r) return r;
    if (gh_bcache_create(&g->dev2) != 0) { gh_dev_close(&g->dev2); return -ENOMEM; }
    r = gh2_fs_mount_key(&g->v2, &g->dev2, key);   /* kopiuje dev2 (z cache+fd) do v2.dev */
    if (r) { gh_bcache_destroy(&g->dev2); gh_dev_close(&g->dev2); return r; }
    return 0;
}

void gfs_unmount(struct gfs *g) {
    if (g->version == 1) { gh_fs_unmount(&g->v1); return; }
    gh2_fs_unmount(&g->v2);
    /* cache i fd wspoldzielone (v2.dev = kopia dev2): zwolnij raz przez v2.dev */
    gh_bcache_destroy(&g->v2.dev);
    gh_dev_close(&g->v2.dev);
    g->dev2.cache = NULL; g->dev2.fd = -1;
}

/* ============================ getattr (mapowanie) ============================ */

int gfs_getattr(struct gfs *g, const char *path, struct gfs_attr *out) {
    memset(out, 0, sizeof(*out));
    if (g->version == 1) {
        struct gh_inode n; uint64_t ino;
        int r = gh_fs_getattr(&g->v1, path, &n, &ino);
        if (r) return r;
        out->type = n.type; out->mode = n.mode;
        out->uid = n.uid; out->gid = n.gid; out->nlink = n.nlink;
        out->size = n.size; out->atime = n.atime; out->mtime = n.mtime; out->ctime = n.ctime;
        if (n.type == GH_CHR || n.type == GH_BLK) out->rdev = n.direct[0];
        return 0;
    }
    struct gh2_inode n; uint64_t ino;
    int r = gh2_fs_getattr(&g->v2, path, &n, &ino);
    if (r) return r;
    out->type = n.type; out->mode = n.mode;
    out->uid = n.uid; out->gid = n.gid; out->nlink = n.nlink;
    out->size = n.size; out->atime = n.atime; out->mtime = n.mtime; out->ctime = n.ctime;
    out->rdev = n.rdev;
    return 0;
}

/* ============================ tworzenie ============================ */

int gfs_create(struct gfs *g, const char *path, uint16_t mode) {
    return g->version == 1 ? gh_fs_create(&g->v1, path, mode)
                           : gh2_fs_create(&g->v2, path, mode);
}
int gfs_mkdir(struct gfs *g, const char *path, uint16_t mode) {
    return g->version == 1 ? gh_fs_mkdir(&g->v1, path, mode)
                           : gh2_fs_mkdir(&g->v2, path, mode);
}
int gfs_mknod(struct gfs *g, const char *path, uint32_t mode, uint64_t rdev) {
    return g->version == 1 ? gh_fs_mknod(&g->v1, path, (uint16_t)mode, rdev)
                           : gh2_fs_mknod(&g->v2, path, mode, rdev);
}
int gfs_symlink(struct gfs *g, const char *target, const char *path) {
    return g->version == 1 ? gh_fs_symlink(&g->v1, target, path)
                           : gh2_fs_symlink(&g->v2, target, path);
}

/* ============================ odczyt celu symlinka ============================ */

ssize_t gfs_readlink(struct gfs *g, const char *path, char *buf, size_t buflen) {
    if (g->version == 1) return gh_fs_readlink(&g->v1, path, buf, buflen);
    int r = gh2_fs_readlink(&g->v2, path, buf, buflen);
    if (r) return r;
    return (ssize_t)strlen(buf);
}

/* ============================ linkowanie / usuwanie ============================ */

int gfs_link(struct gfs *g, const char *oldpath, const char *newpath) {
    return g->version == 1 ? gh_fs_link(&g->v1, oldpath, newpath)
                           : gh2_fs_link(&g->v2, oldpath, newpath);
}
int gfs_unlink(struct gfs *g, const char *path) {
    return g->version == 1 ? gh_fs_unlink(&g->v1, path)
                           : gh2_fs_unlink(&g->v2, path);
}
int gfs_rmdir(struct gfs *g, const char *path) {
    return g->version == 1 ? gh_fs_rmdir(&g->v1, path)
                           : gh2_fs_rmdir(&g->v2, path);
}
int gfs_rename(struct gfs *g, const char *oldpath, const char *newpath, unsigned flags) {
    return g->version == 1 ? gh_fs_rename2(&g->v1, oldpath, newpath, flags)
                           : gh2_fs_rename(&g->v2, oldpath, newpath, flags);
}

/* ============================ metadane ============================ */

int gfs_chmod(struct gfs *g, const char *path, uint16_t mode) {
    return g->version == 1 ? gh_fs_chmod(&g->v1, path, mode)
                           : gh2_fs_chmod(&g->v2, path, mode);
}
int gfs_chown(struct gfs *g, const char *path, uint32_t uid, uint32_t gid) {
    return g->version == 1 ? gh_fs_chown(&g->v1, path, uid, gid)
                           : gh2_fs_chown(&g->v2, path, uid, gid);
}
int gfs_utimens(struct gfs *g, const char *path, uint64_t atime, uint64_t mtime) {
    return g->version == 1 ? gh_fs_utimens(&g->v1, path, atime, mtime)
                           : gh2_fs_utimens(&g->v2, path, atime, mtime);
}
int gfs_truncate(struct gfs *g, const char *path, uint64_t size) {
    return g->version == 1 ? gh_fs_truncate(&g->v1, path, size)
                           : gh2_fs_truncate(&g->v2, path, size);
}

/* ============================ dane ============================ */

ssize_t gfs_read(struct gfs *g, const char *path, void *buf, size_t len, uint64_t off) {
    return g->version == 1 ? gh_fs_read(&g->v1, path, buf, len, off)
                           : gh2_fs_read(&g->v2, path, buf, len, off);
}
ssize_t gfs_write(struct gfs *g, const char *path, const void *buf, size_t len, uint64_t off) {
    return g->version == 1 ? gh_fs_write(&g->v1, path, buf, len, off)
                           : gh2_fs_write(&g->v2, path, buf, len, off);
}

/* ============================ readdir (adapter callbacka) ============================ */

struct rd_adapter { gfs_readdir_cb cb; void *ctx; };

static int v1_rd_cb(const struct gh_dirent *de, void *c) {
    struct rd_adapter *a = c;
    /* typ z lookupu niedostepny tu taniej; FUSE i tak robi getattr. Przekaz 0. */
    return a->cb(de->name, 0, a->ctx);
}
static int v2_rd_cb(const char *name, uint16_t name_len, uint64_t ino,
                    uint8_t ftype, void *c) {
    (void)name_len; (void)ino;
    struct rd_adapter *a = c;
    return a->cb(name, ftype, a->ctx);
}

int gfs_readdir(struct gfs *g, const char *path, gfs_readdir_cb cb, void *ctx) {
    struct rd_adapter a = { cb, ctx };
    if (g->version == 1) return gh_fs_readdir(&g->v1, path, v1_rd_cb, &a);
    return gh2_fs_readdir(&g->v2, path, v2_rd_cb, &a);
}

/* ============================ statfs ============================ */

int gfs_statfs(struct gfs *g, struct gfs_statfs *out) {
    memset(out, 0, sizeof(*out));
    if (g->version == 1) {
        struct gh_statfs s; int r = gh_fs_statfs(&g->v1, &s);
        if (r) return r;
        out->block_size = s.block_size;
        out->total_blocks = s.total_blocks; out->free_blocks = s.free_blocks;
        out->total_inodes = s.total_inodes; out->free_inodes = s.free_inodes;
        out->name_max = s.name_max;
        return 0;
    }
    struct gh2_statfs s; int r = gh2_fs_statfs(&g->v2, &s);
    if (r) return r;
    out->block_size = (uint32_t)s.block_size;
    out->total_blocks = s.total_blocks; out->free_blocks = s.free_blocks;
    out->total_inodes = 0; out->free_inodes = 0;   /* v2: bez stalej puli i-wezlow */
    out->name_max = 255;
    return 0;
}

/* ============================ sync / fsck ============================ */

int gfs_sync(struct gfs *g) {
    return g->version == 1 ? gh_fs_sync(&g->v1) : gh2_fs_commit(&g->v2);
}

/* ============================ snapshoty (v2; v1 -> ENOTSUP) ============================ */

int gfs_snapshot(struct gfs *g, const char *name) {
    if (g->version == 1) { (void)name; return -ENOTSUP; }
    return gh2_fs_snapshot(&g->v2, name);
}

int gfs_subvol_list(struct gfs *g, gfs_subvol_cb cb, void *ctx) {
    if (g->version == 1) { (void)cb; (void)ctx; return -ENOTSUP; }
    /* gfs_subvol_cb i gh2_subvol_cb maja identyczna sygnature */
    return gh2_fs_subvol_list(&g->v2, (gh2_subvol_cb)cb, ctx);
}

int gfs_subvol_delete(struct gfs *g, uint64_t subvol_id) {
    if (g->version == 1) { (void)subvol_id; return -ENOTSUP; }
    return gh2_fs_subvol_delete(&g->v2, subvol_id);
}

int gfs_fsck(struct gfs *g, int repair, int *issues) {
    if (g->version == 1) return gh_fsck(&g->v1, repair, issues);
    /* v2: repair!=0 wykonuje trwale naprawy poziomu drzewa (caller robi gfs_sync). */
    return gh2_fsck(&g->v2, repair, issues);
}

/* ============================ xattr (v1 + v2) ============================ */

int gfs_setxattr(struct gfs *g, const char *path, const char *name,
                 const void *val, size_t size, int flags) {
    if (g->version == 1) return gh_fs_setxattr(&g->v1, path, name, val, size, flags);
    return gh2_fs_setxattr(&g->v2, path, name, val, size, flags);
}
ssize_t gfs_getxattr(struct gfs *g, const char *path, const char *name,
                     void *buf, size_t size) {
    if (g->version == 1) return gh_fs_getxattr(&g->v1, path, name, buf, size);
    return gh2_fs_getxattr(&g->v2, path, name, buf, size);
}
ssize_t gfs_listxattr(struct gfs *g, const char *path, char *buf, size_t size) {
    if (g->version == 1) return gh_fs_listxattr(&g->v1, path, buf, size);
    return gh2_fs_listxattr(&g->v2, path, buf, size);
}
int gfs_removexattr(struct gfs *g, const char *path, const char *name) {
    if (g->version == 1) return gh_fs_removexattr(&g->v1, path, name);
    return gh2_fs_removexattr(&g->v2, path, name);
}
