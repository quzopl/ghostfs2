#include "fs.h"
#include "journal.h"
#include "crypto.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* rozpocznij operacje w running txn; flush na pojemnosc */
static int txn_begin(struct gh_fs *fs) {
    struct gh_txn *t = fs->dev.txn;
    if (t && t->n > t->cap / 2) {
        int r = gh_jrnl_flush(&fs->dev, &fs->sb);
        if (r) return r;     /* paczka utracona — zglos blad, nie kontynuuj cicho */
    }
    gh_jrnl_op_begin(&fs->dev);
    return 0;
}
/* zakoncz operacje: przy bledzie rollback, inaczej commit operacji (BEZ trwalego commitu) */
static int txn_end_i(struct gh_fs *fs, int rc) {
    if (rc < 0) gh_jrnl_op_rollback(&fs->dev);
    else        gh_jrnl_op_commit(&fs->dev);
    return rc;
}
/* wariant dla operacji zwracajacych ssize_t (write) */
static ssize_t txn_end_s(struct gh_fs *fs, ssize_t rc) {
    if (rc < 0) gh_jrnl_op_rollback(&fs->dev);
    else        gh_jrnl_op_commit(&fs->dev);
    return rc;
}

int gh_fs_mount_key(struct gh_fs *fs, const char *path, const char *passphrase) {
    int r = gh_dev_open(path, &fs->dev); if (r) return r;
    r = gh_mount_sb(&fs->dev, &fs->sb);
    if (r) { gh_dev_close(&fs->dev); return r; }
    fs->dev.cipher = NULL;
    if (fs->sb.flags & GH_SB_ENCRYPTED) {
        if (!passphrase || !passphrase[0]) { gh_dev_close(&fs->dev); return -EACCES; }
        struct gh_cipher *c = malloc(sizeof(*c));
        if (!c) { gh_dev_close(&fs->dev); return -ENOMEM; }
        if (gh_crypto_derive(passphrase, fs->sb.enc_salt, fs->sb.enc_kdf_iters, c->key)) {
            free(c); gh_dev_close(&fs->dev); return -EIO;
        }
        uint8_t v[32]; gh_crypto_verifier(c->key, fs->sb.enc_salt, v);
        if (memcmp(v, fs->sb.enc_verifier, 32) != 0) {   /* zle haslo */
            free(c); gh_dev_close(&fs->dev); return -EACCES;
        }
        fs->dev.cipher = c;
    }
    if (gh_bcache_create(&fs->dev) != 0) {
        if (fs->dev.cipher) { gh_crypto_wipe(fs->dev.cipher); free(fs->dev.cipher); fs->dev.cipher = NULL; }
        gh_dev_close(&fs->dev); return -ENOMEM;
    }
    r = gh_jrnl_recover(&fs->dev, &fs->sb);
    if (r) { gh_bcache_destroy(&fs->dev);
        if (fs->dev.cipher) { gh_crypto_wipe(fs->dev.cipher); free(fs->dev.cipher); fs->dev.cipher = NULL; }
        gh_dev_close(&fs->dev); return r; }
    r = gh_jrnl_open(&fs->dev, &fs->sb);
    if (r) { gh_bcache_destroy(&fs->dev);
        if (fs->dev.cipher) { gh_crypto_wipe(fs->dev.cipher); free(fs->dev.cipher); fs->dev.cipher = NULL; }
        gh_dev_close(&fs->dev); return r; }
    return 0;
}
int gh_fs_mount(struct gh_fs *fs, const char *path) {
    return gh_fs_mount_key(fs, path, getenv("GHOSTFS_KEY"));
}
void gh_fs_unmount(struct gh_fs *fs) {
    gh_jrnl_flush(&fs->dev, &fs->sb);    /* utrwal ostatnia paczke */
    gh_jrnl_close(&fs->dev);
    gh_bcache_destroy(&fs->dev);
    if (fs->dev.cipher) { gh_crypto_wipe(fs->dev.cipher); free(fs->dev.cipher); fs->dev.cipher = NULL; }
    gh_dev_close(&fs->dev);
}

int gh_fs_getattr(struct gh_fs *fs, const char *path,
                  struct gh_inode *out, uint64_t *out_ino) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    r = gh_inode_read(&fs->dev, &fs->sb, ino, out); if (r) return r;
    if (out_ino) *out_ino = ino;
    return 0;
}

static int truncate_locked(struct gh_fs *fs, const char *path, uint64_t new_size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type == GH_DIR) return -EISDIR;
    if (n.type != GH_FILE) return -EINVAL;   /* symlink/wezel specjalny: chron rdev/tresc */
    return gh_inode_truncate(&fs->dev, &fs->sb, ino, &n, new_size);
}
int gh_fs_truncate(struct gh_fs *fs, const char *path, uint64_t new_size) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, truncate_locked(fs, path, new_size));
}

static int utimens_locked(struct gh_fs *fs, const char *path, uint64_t atime, uint64_t mtime) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    n.atime = atime; n.mtime = mtime; n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}
int gh_fs_utimens(struct gh_fs *fs, const char *path, uint64_t atime, uint64_t mtime) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, utimens_locked(fs, path, atime, mtime));
}

static int chmod_locked(struct gh_fs *fs, const char *path, uint16_t mode) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    n.mode = mode & 0777; n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}
int gh_fs_chmod(struct gh_fs *fs, const char *path, uint16_t mode) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, chmod_locked(fs, path, mode));
}

static int chown_locked(struct gh_fs *fs, const char *path, uint32_t uid, uint32_t gid) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (uid != (uint32_t)-1) n.uid = uid;
    if (gid != (uint32_t)-1) n.gid = gid;
    n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}
int gh_fs_chown(struct gh_fs *fs, const char *path, uint32_t uid, uint32_t gid) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, chown_locked(fs, path, uid, gid));
}

int gh_fs_statfs(struct gh_fs *fs, struct gh_statfs *out) {
    struct gh_superblock *sb = &fs->sb;
    out->block_size = GH_BLOCK_SIZE;
    out->total_blocks = sb->total_blocks;
    out->total_inodes = sb->inode_count;
    out->name_max = GH_NAME_MAX;
    /* wolne bloki: czytaj kazdy blok mapy bitowej RAZ (nie pelny odczyt+CRC na kazdy bit) */
    uint64_t freeb = 0;
    uint8_t  bm[GH_BLOCK_SIZE];
    uint64_t cur_bm = (uint64_t)-1;
    for (uint64_t b = sb->data_start; b < sb->total_blocks; b++) {
        uint64_t byte   = b / 8;
        uint64_t bm_blk = sb->bitmap_start + byte / GH_BLOCK_SIZE;
        if (bm_blk != cur_bm) {
            int r = gh_block_read(&fs->dev, bm_blk, bm); if (r) return r;
            cur_bm = bm_blk;
        }
        if (!(bm[byte % GH_BLOCK_SIZE] & (1u << (b % 8)))) freeb++;
    }
    out->free_blocks = freeb;
    /* wolne i-wezly: czytaj kazdy blok tablicy i-wezlow RAZ (type to pierwsze pole, GH_FREE==0) */
    uint64_t freei = 0;
    uint8_t  ib[GH_BLOCK_SIZE];
    uint64_t cur_i = (uint64_t)-1;
    for (uint64_t i = 0; i < sb->inode_count; i++) {
        uint64_t iblk = sb->inode_start + i / GH_INODES_PER_BLK;
        if (iblk != cur_i) {
            int r = gh_block_read(&fs->dev, iblk, ib); if (r) return r;
            cur_i = iblk;
        }
        uint16_t type;
        memcpy(&type, ib + (i % GH_INODES_PER_BLK) * GH_INODE_SIZE, sizeof(type));
        if (type == GH_FREE) freei++;
    }
    out->free_inodes = freei;
    return 0;
}

int gh_fs_sync(struct gh_fs *fs) {
    int r = gh_jrnl_flush(&fs->dev, &fs->sb);
    if (r) return r;
    return fsync(fs->dev.fd) == 0 ? 0 : -errno;
}

static int make_node(struct gh_fs *fs, const char *path, uint16_t type, uint16_t mode, uint64_t rdev) {
    char parent[1024], name[256];
    int r = gh_path_split(path, parent, name); if (r) return r;
    uint64_t pino; r = gh_path_resolve(&fs->dev, &fs->sb, parent, &pino);
    if (r) return r;
    uint64_t exists; if (gh_dir_lookup(&fs->dev, &fs->sb, pino, name, &exists) == 0)
        return -EEXIST;
    uint64_t ino; r = gh_inode_alloc(&fs->dev, &fs->sb, type, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    n.mode = mode;
    if (type == GH_DIR) {   /* dopisz . i .. przez gh_dir_add (tablica haszowa) */
        int e = gh_dir_add(&fs->dev, &fs->sb, ino, ".", ino);
        if (e) { gh_inode_free(&fs->dev, &fs->sb, ino); return e; }
        e = gh_dir_add(&fs->dev, &fs->sb, ino, "..", pino);
        if (e) { gh_inode_free(&fs->dev, &fs->sb, ino); return e; }
        gh_inode_read(&fs->dev, &fs->sb, ino, &n);   /* re-read: size zmienione */
        n.mode = mode; n.nlink = 2;
    } else if (type == GH_CHR || type == GH_BLK) {
        n.direct[0] = rdev;   /* rdev w direct[0] (technika ext2) */
    }
    gh_inode_write(&fs->dev, &fs->sb, ino, &n);
    r = gh_dir_add(&fs->dev, &fs->sb, pino, name, ino);
    if (r) { gh_inode_free(&fs->dev, &fs->sb, ino); return r; }
    return 0;
}

int gh_fs_create(struct gh_fs *fs, const char *path, uint16_t mode) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, GH_FILE, mode, 0));
}
int gh_fs_mkdir(struct gh_fs *fs, const char *path, uint16_t mode) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, GH_DIR, mode, 0));
}
int gh_fs_mknod(struct gh_fs *fs, const char *path, uint16_t mode, uint64_t rdev) {
    uint16_t type;
    if (S_ISFIFO(mode)) type = GH_FIFO;
    else if (S_ISSOCK(mode)) type = GH_SOCK;
    else if (S_ISCHR(mode)) type = GH_CHR;
    else if (S_ISBLK(mode)) type = GH_BLK;
    else if (S_ISREG(mode)) type = GH_FILE;
    else return -EINVAL;
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, make_node(fs, path, type, (uint16_t)(mode & 07777), rdev));
}

static int remove_node(struct gh_fs *fs, const char *path, int want_dir) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    if (ino == GH_ROOT_INO) return -EBUSY;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (want_dir) {
        if (n.type != GH_DIR) return -ENOTDIR;
        int empty; r = gh_dir_is_empty(&fs->dev, &fs->sb, ino, &empty); if (r) return r;
        if (!empty) return -ENOTEMPTY;
    } else {
        if (n.type == GH_DIR) return -EISDIR;
    }
    char parent[1024], name[256];
    r = gh_path_split(path, parent, name); if (r) return r;
    uint64_t pino; r = gh_path_resolve(&fs->dev, &fs->sb, parent, &pino);
    if (r) return r;
    r = gh_dir_remove(&fs->dev, &fs->sb, pino, name); if (r) return r;
    if (want_dir) {
        return gh_inode_free(&fs->dev, &fs->sb, ino);   /* katalogi nie sa wspoldzielone */
    }
    /* pliki/symlinki: zwolnij i-wezel dopiero przy ostatnim linku */
    struct gh_inode tn; r = gh_inode_read(&fs->dev, &fs->sb, ino, &tn); if (r) return r;
    if (tn.nlink > 1) {
        tn.nlink--; tn.ctime = (uint64_t)time(NULL);
        return gh_inode_write(&fs->dev, &fs->sb, ino, &tn);
    }
    return gh_inode_free(&fs->dev, &fs->sb, ino);
}

int gh_fs_unlink(struct gh_fs *fs, const char *path) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, remove_node(fs, path, 0));
}
int gh_fs_rmdir(struct gh_fs *fs, const char *path) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, remove_node(fs, path, 1));
}

static int link_locked(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, oldpath, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type == GH_DIR) return -EPERM;
    char parent[1024], name[256];
    r = gh_path_split(newpath, parent, name); if (r) return r;
    uint64_t pino; r = gh_path_resolve(&fs->dev, &fs->sb, parent, &pino);
    if (r) return r;
    uint64_t ex;
    if (gh_dir_lookup(&fs->dev, &fs->sb, pino, name, &ex) == 0) return -EEXIST;
    r = gh_dir_add(&fs->dev, &fs->sb, pino, name, ino); if (r) return r;
    n.nlink++; n.ctime = (uint64_t)time(NULL);
    return gh_inode_write(&fs->dev, &fs->sb, ino, &n);
}
int gh_fs_link(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, link_locked(fs, oldpath, newpath));
}

/* czy `anc` jest przodkiem (lub równy) katalogu `start`, idąc po ".." do roota */
static int is_ancestor(struct gh_dev *dev, const struct gh_superblock *sb,
                       uint64_t anc, uint64_t start) {
    uint64_t cur = start;
    for (int guard = 0; guard < 4096; guard++) {
        if (cur == anc) return 1;
        if (cur == sb->root_inode) return 0;
        uint64_t parent;
        if (gh_dir_lookup(dev, sb, cur, "..", &parent) != 0) return 0;
        if (parent == cur) return 0;
        cur = parent;
    }
    return 0;
}

static int rename_locked(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t sino; int r = gh_path_resolve(&fs->dev, &fs->sb, oldpath, &sino);
    if (r) return r;
    if (sino == GH_ROOT_INO) return -EBUSY;
    struct gh_inode sn; r = gh_inode_read(&fs->dev, &fs->sb, sino, &sn); if (r) return r;

    char op[1024], on[256], np[1024], nn[256];
    r = gh_path_split(oldpath, op, on); if (r) return r;
    r = gh_path_split(newpath, np, nn); if (r) return r;
    uint64_t opino, npino;
    r = gh_path_resolve(&fs->dev, &fs->sb, op, &opino); if (r) return r;
    r = gh_path_resolve(&fs->dev, &fs->sb, np, &npino); if (r) return r;

    if (sn.type == GH_DIR && is_ancestor(&fs->dev, &fs->sb, sino, npino)) return -EINVAL;

    uint64_t tino;
    if (gh_dir_lookup(&fs->dev, &fs->sb, npino, nn, &tino) == 0) {
        if (tino == sino) return 0;
        struct gh_inode tn; r = gh_inode_read(&fs->dev, &fs->sb, tino, &tn); if (r) return r;
        if (sn.type == GH_DIR) {
            if (tn.type != GH_DIR) return -ENOTDIR;
            int empty; r = gh_dir_is_empty(&fs->dev, &fs->sb, tino, &empty); if (r) return r;
            if (!empty) return -ENOTEMPTY;
        } else {
            if (tn.type == GH_DIR) return -EISDIR;
        }
        r = gh_dir_remove(&fs->dev, &fs->sb, npino, nn); if (r) return r;
        if (tn.type != GH_DIR && tn.nlink > 1) {
            tn.nlink--; gh_inode_write(&fs->dev, &fs->sb, tino, &tn);
        } else {
            gh_inode_free(&fs->dev, &fs->sb, tino);
        }
    }

    r = gh_dir_add(&fs->dev, &fs->sb, npino, nn, sino); if (r) return r;
    r = gh_dir_remove(&fs->dev, &fs->sb, opino, on);
    if (r) { gh_dir_remove(&fs->dev, &fs->sb, npino, nn); return r; }

    if (sn.type == GH_DIR && opino != npino) {
        gh_dir_set_ino(&fs->dev, &fs->sb, sino, "..", npino);
    }
    return 0;
}

static int exchange_locked(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    uint64_t sino, tino;
    int r = gh_path_resolve(&fs->dev, &fs->sb, oldpath, &sino); if (r) return r;
    r = gh_path_resolve(&fs->dev, &fs->sb, newpath, &tino); if (r) return r;
    if (sino == tino) return 0;
    if (sino == GH_ROOT_INO || tino == GH_ROOT_INO) return -EBUSY;
    char op[1024], on[256], np[1024], nn[256];
    r = gh_path_split(oldpath, op, on); if (r) return r;
    r = gh_path_split(newpath, np, nn); if (r) return r;
    uint64_t opino, npino;
    r = gh_path_resolve(&fs->dev, &fs->sb, op, &opino); if (r) return r;
    r = gh_path_resolve(&fs->dev, &fs->sb, np, &npino); if (r) return r;
    struct gh_inode sn, tn;
    r = gh_inode_read(&fs->dev, &fs->sb, sino, &sn); if (r) return r;
    r = gh_inode_read(&fs->dev, &fs->sb, tino, &tn); if (r) return r;
    /* zamien wpisy */
    r = gh_dir_set_ino(&fs->dev, &fs->sb, opino, on, tino); if (r) return r;
    r = gh_dir_set_ino(&fs->dev, &fs->sb, npino, nn, sino); if (r) return r;
    /* aktualizuj ".." gdy katalog zmienia rodzica */
    if (opino != npino) {
        if (sn.type == GH_DIR) {
            gh_dir_set_ino(&fs->dev, &fs->sb, sino, "..", npino);
        }
        if (tn.type == GH_DIR) {
            gh_dir_set_ino(&fs->dev, &fs->sb, tino, "..", opino);
        }
    }
    return 0;
}

int gh_fs_rename2(struct gh_fs *fs, const char *oldpath, const char *newpath, unsigned flags) {
    if (flags & ~(GH_RENAME_NOREPLACE | GH_RENAME_EXCHANGE)) return -EINVAL;
    if ((flags & GH_RENAME_NOREPLACE) && (flags & GH_RENAME_EXCHANGE)) return -EINVAL;
    int b = txn_begin(fs); if (b) return b;
    int rc;
    if (flags & GH_RENAME_EXCHANGE) {
        rc = exchange_locked(fs, oldpath, newpath);
    } else if (flags & GH_RENAME_NOREPLACE) {
        uint64_t pino, t; char p[1024], n[256];
        if ((rc = gh_path_split(newpath, p, n)) == 0
            && (rc = gh_path_resolve(&fs->dev, &fs->sb, p, &pino)) == 0) {
            if (gh_dir_lookup(&fs->dev, &fs->sb, pino, n, &t) == 0) rc = -EEXIST;
            else rc = rename_locked(fs, oldpath, newpath);
        }
    } else {
        rc = rename_locked(fs, oldpath, newpath);
    }
    return txn_end_i(fs, rc);
}

int gh_fs_rename(struct gh_fs *fs, const char *oldpath, const char *newpath) {
    return gh_fs_rename2(fs, oldpath, newpath, 0);
}

static int symlink_locked(struct gh_fs *fs, const char *target, const char *linkpath) {
    size_t tlen = strlen(target);
    if (tlen == 0) return -EINVAL;
    if (tlen > GH_SYMLINK_MAX) return -ENAMETOOLONG;
    char parent[1024], name[256];
    int r = gh_path_split(linkpath, parent, name); if (r) return r;
    uint64_t pino; r = gh_path_resolve(&fs->dev, &fs->sb, parent, &pino);
    if (r) return r;
    uint64_t ex;
    if (gh_dir_lookup(&fs->dev, &fs->sb, pino, name, &ex) == 0) return -EEXIST;
    uint64_t ino; r = gh_inode_alloc(&fs->dev, &fs->sb, GH_SYMLINK, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n);
    if (r) { gh_inode_free(&fs->dev, &fs->sb, ino); return r; }
    n.mode = 0777;
    if (gh_inode_pwrite(&fs->dev, &fs->sb, ino, &n, target, tlen, 0) != (ssize_t)tlen) {
        gh_inode_free(&fs->dev, &fs->sb, ino); return -EIO;
    }
    r = gh_dir_add(&fs->dev, &fs->sb, pino, name, ino);
    if (r) { gh_inode_free(&fs->dev, &fs->sb, ino); return r; }
    return 0;
}
int gh_fs_symlink(struct gh_fs *fs, const char *target, const char *linkpath) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, symlink_locked(fs, target, linkpath));
}

ssize_t gh_fs_readlink(struct gh_fs *fs, const char *path, char *buf, size_t size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    if (n.type != GH_SYMLINK) return -EINVAL;
    size_t cn = (n.size < size) ? n.size : size;
    return gh_inode_pread(&fs->dev, &fs->sb, &n, buf, cn, 0);
}

static int setxattr_locked(struct gh_fs *fs, const char *path, const char *name,
                           const void *val, size_t size, int flags) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_set(&fs->dev, &fs->sb, &n, ino, name, val, size, flags);
}
int gh_fs_setxattr(struct gh_fs *fs, const char *path, const char *name,
                   const void *val, size_t size, int flags) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, setxattr_locked(fs, path, name, val, size, flags));
}
ssize_t gh_fs_getxattr(struct gh_fs *fs, const char *path, const char *name,
                       void *buf, size_t size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_get(&fs->dev, &fs->sb, &n, name, buf, size);
}
ssize_t gh_fs_listxattr(struct gh_fs *fs, const char *path, char *buf, size_t size) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_list(&fs->dev, &fs->sb, &n, buf, size);
}
static int removexattr_locked(struct gh_fs *fs, const char *path, const char *name) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino); if (r) return r;
    struct gh_inode n; r = gh_inode_read(&fs->dev, &fs->sb, ino, &n); if (r) return r;
    return gh_xattr_remove(&fs->dev, &fs->sb, &n, ino, name);
}
int gh_fs_removexattr(struct gh_fs *fs, const char *path, const char *name) {
    int b = txn_begin(fs); if (b) return b;
    return txn_end_i(fs, removexattr_locked(fs, path, name));
}

ssize_t gh_fs_read(struct gh_fs *fs, const char *path, void *buf, size_t n, uint64_t off) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode node; r = gh_inode_read(&fs->dev, &fs->sb, ino, &node); if (r) return r;
    if (node.type != GH_FILE) return -EISDIR;
    return gh_inode_pread(&fs->dev, &fs->sb, &node, buf, n, off);
}

ssize_t gh_fs_write(struct gh_fs *fs, const char *path, const void *buf, size_t n, uint64_t off) {
    int b = txn_begin(fs); if (b) return b;
    ssize_t rc;
    {
        uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
        if (r) { rc = r; }
        else {
            struct gh_inode node; r = gh_inode_read(&fs->dev, &fs->sb, ino, &node);
            if (r) rc = r;
            else if (node.type != GH_FILE) rc = -EISDIR;
            else rc = gh_inode_pwrite(&fs->dev, &fs->sb, ino, &node, buf, n, off);
        }
    }
    return txn_end_s(fs, rc);
}

int gh_fs_readdir(struct gh_fs *fs, const char *path, gh_dir_iter_fn cb, void *ctx) {
    uint64_t ino; int r = gh_path_resolve(&fs->dev, &fs->sb, path, &ino);
    if (r) return r;
    struct gh_inode node; r = gh_inode_read(&fs->dev, &fs->sb, ino, &node); if (r) return r;
    if (node.type != GH_DIR) return -ENOTDIR;
    return gh_dir_iterate(&fs->dev, &fs->sb, ino, cb, ctx);
}

/* fsck: zbuduj oczekiwaną mapę z osiągalnych i-węzłów, porównaj z zapisaną */
static void mark(uint8_t *want, uint64_t b, uint64_t total, uint64_t *bad) {
    if (b >= total) { (*bad)++; return; }
    want[b/8] |= (uint8_t)(1u << (b % 8));
}

int gh_fsck(struct gh_fs *fs, int repair, int *issues) {
    struct gh_superblock *sb = &fs->sb;
    if (repair) gh_jrnl_op_begin(&fs->dev);   /* naprawy w transakcji -> dirty -> flush przy unmount */
    int tree_issues = 0;
    {
        uint8_t  *reach  = calloc(sb->inode_count, 1);
        uint32_t *refcnt = calloc(sb->inode_count, sizeof(uint32_t));
        uint64_t *stack  = malloc(sb->inode_count * sizeof(uint64_t));
        if (!reach || !refcnt || !stack) { free(reach); free(refcnt); free(stack); return -ENOMEM; }
        uint64_t sp = 0;
        if (sb->root_inode < sb->inode_count) { reach[sb->root_inode] = 1; stack[sp++] = sb->root_inode; }
        uint64_t DENT = sizeof(struct gh_dirent);
        while (sp > 0) {
            uint64_t dino = stack[--sp];
            struct gh_inode dn;
            if (gh_inode_read(&fs->dev, sb, dino, &dn) || dn.type != GH_DIR) continue;
            for (uint64_t off = 0; off < dn.size; off += DENT) {
                struct gh_dirent de;
                if (gh_inode_pread(&fs->dev, sb, &dn, &de, DENT, off) != (ssize_t)DENT) break;
                if (de.ino == 0 || de.ino >= sb->inode_count) continue;
                if (de.name_len == 1 && de.name[0] == '.') continue;
                if (de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.') continue;
                struct gh_inode tn;
                if (gh_inode_read(&fs->dev, sb, de.ino, &tn)) continue;
                if (tn.type == GH_DIR) {
                    if (!reach[de.ino]) { reach[de.ino] = 1; stack[sp++] = de.ino; }
                } else if (tn.type != GH_FREE) {
                    /* plik/symlink oraz wezly specjalne (FIFO/SOCK/CHR/BLK):
                     * osiagalne i zliczane do nlink */
                    reach[de.ino] = 1; refcnt[de.ino]++;
                }
            }
        }
        for (uint64_t ino = GH_ROOT_INO; ino < sb->inode_count; ino++) {
            if (ino == sb->root_inode) continue;
            struct gh_inode n;
            if (gh_inode_read(&fs->dev, sb, ino, &n) || n.type == GH_FREE) continue;
            if (!reach[ino]) {                       /* osierocony i-wezel */
                tree_issues++;
                if (repair) gh_inode_free(&fs->dev, sb, ino);
            } else if (n.type != GH_DIR) {           /* plik/symlink/wezel specjalny */
                if (n.nlink != refcnt[ino]) {        /* zly nlink */
                    tree_issues++;
                    if (repair) { n.nlink = refcnt[ino]; gh_inode_write(&fs->dev, sb, ino, &n); }
                }
            }
        }
        free(reach); free(refcnt); free(stack);
    }
    size_t mapbytes = (sb->total_blocks + 7) / 8;
    uint8_t *want = calloc(mapbytes, 1);
    if (!want) return -ENOMEM;
    uint64_t bad = 0;
    for (uint64_t b = 0; b < sb->data_start; b++) mark(want, b, sb->total_blocks, &bad);  /* metadane */

    for (uint64_t ino = 0; ino < sb->inode_count; ino++) {
        struct gh_inode n;
        if (gh_inode_read(&fs->dev, sb, ino, &n) || n.type == GH_FREE) continue;
        /* wezly specjalne (FIFO/SOCK/CHR/BLK) nie maja blokow danych; direct[0]
         * urzadzenia to rdev — NIE wolno go znaczyc jako osiagalny blok */
        int has_blocks = (n.type == GH_FILE || n.type == GH_DIR || n.type == GH_SYMLINK);
        /* bezpośrednie */
        if (has_blocks)
            for (int i = 0; i < GH_NDIRECT; i++)
                if (n.direct[i]) mark(want, n.direct[i], sb->total_blocks, &bad);
        /* xattr_block: ZAWSZE (wezly specjalne tez moga miec xattr) */
        {
            uint64_t xb = n.xattr_block; uint64_t guard = 0;
            while (xb && guard < sb->total_blocks) {
                mark(want, xb, sb->total_blocks, &bad);
                uint8_t xblk[GH_BLOCK_SIZE]; uint64_t nx = 0;
                if (xb < sb->total_blocks && gh_block_read(&fs->dev, xb, xblk) == 0)
                    memcpy(&nx, xblk, 8);
                else break;
                xb = nx; guard++;
            }
        }
        /* pojedynczo pośrednie: blok wskaźnikowy + liście */
        if (has_blocks && n.indirect) {
            mark(want, n.indirect, sb->total_blocks, &bad);
            uint64_t p[GH_PTRS_PER_BLK];
            if (gh_block_read(&fs->dev, n.indirect, p) == 0)
                for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++)
                    if (p[i]) mark(want, p[i], sb->total_blocks, &bad);
        }
        /* podwójnie pośrednie: blok L1 + bloki L2 + liście danych */
        if (has_blocks && n.double_indirect) {
            mark(want, n.double_indirect, sb->total_blocks, &bad);
            uint64_t l1[GH_PTRS_PER_BLK];
            if (gh_block_read(&fs->dev, n.double_indirect, l1) == 0)
                for (uint64_t i = 0; i < GH_PTRS_PER_BLK; i++) {
                    if (!l1[i]) continue;
                    mark(want, l1[i], sb->total_blocks, &bad);
                    uint64_t l2[GH_PTRS_PER_BLK];
                    if (gh_block_read(&fs->dev, l1[i], l2) == 0)
                        for (uint64_t j = 0; j < GH_PTRS_PER_BLK; j++)
                            if (l2[j]) mark(want, l2[j], sb->total_blocks, &bad);
                }
        }
    }

    int diff = 0;
    for (uint64_t b = 0; b < sb->total_blocks; b++) {
        int set = 0; gh_bitmap_test(&fs->dev, sb, b, &set);
        int exp = (want[b/8] >> (b%8)) & 1;
        if (set != exp) diff++;   /* tylko zliczamy; naprawa hurtem nizej */
    }
    if (issues) *issues = (int)(diff + bad + tree_issues);
    if (repair && diff) {
        /* nadpisz cala mape oczekiwana */
        uint8_t blk[GH_BLOCK_SIZE];
        uint64_t mapblocks = (sb->total_blocks + GH_BLOCK_SIZE*8 - 1)/(GH_BLOCK_SIZE*8);
        for (uint64_t mb = 0; mb < mapblocks; mb++) {
            memset(blk, 0, sizeof(blk));
            size_t copy = mapbytes - mb*GH_BLOCK_SIZE;
            if (copy > GH_BLOCK_SIZE) copy = GH_BLOCK_SIZE;
            memcpy(blk, want + mb*GH_BLOCK_SIZE, copy);
            gh_block_write(&fs->dev, sb->bitmap_start + mb, blk);
        }
    }
    free(want);
    if (repair) gh_jrnl_op_commit(&fs->dev);  /* zatwierdz paczke napraw (utrwalana przy flush/unmount) */
    return 0;
}
