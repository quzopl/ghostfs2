#include "dir.h"
#include "csum.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#define DENT_SZ ((uint64_t)sizeof(struct gh_dirent))

/* Rozmiar bufora 'parent' u wszystkich call-site'ow gh_path_split (char parent[1024]).
 * Czesc-rodzic musi sie zmiescic wraz z terminatorem '\0'. */
#define GH_PARENT_MAX 1024

static int rd_hdr(struct gh_dev *dev, const struct gh_superblock *sb,
                  struct gh_inode *dir, struct gh_dirhdr *h) {
    uint8_t buf[sizeof(struct gh_dirent)];
    ssize_t r = gh_inode_pread(dev, sb, dir, buf, DENT_SZ, 0);
    if (r < 0) return (int)r;
    if (r != (ssize_t)DENT_SZ) return -EIO;
    memcpy(h, buf, sizeof(*h));
    return 0;
}
static int wr_hdr(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t dir_ino, struct gh_inode *dir, const struct gh_dirhdr *h) {
    uint8_t buf[sizeof(struct gh_dirent)]; memset(buf, 0, sizeof(buf));
    memcpy(buf, h, sizeof(*h));
    return gh_inode_pwrite(dev, sb, dir_ino, dir, buf, DENT_SZ, 0) == (ssize_t)DENT_SZ ? 0 : -EIO;
}
static int rd_slot(struct gh_dev *dev, const struct gh_superblock *sb,
                   struct gh_inode *dir, uint64_t slot, struct gh_dirent *de) {
    ssize_t r = gh_inode_pread(dev, sb, dir, de, DENT_SZ, slot * DENT_SZ);
    if (r < 0) return (int)r;
    if (r != (ssize_t)DENT_SZ) return -EIO;
    return 0;
}
static int wr_slot(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t dir_ino, struct gh_inode *dir, uint64_t slot, const struct gh_dirent *de) {
    return gh_inode_pwrite(dev, sb, dir_ino, dir, de, DENT_SZ, slot * DENT_SZ) == (ssize_t)DENT_SZ ? 0 : -EIO;
}
static int slot_empty(const struct gh_dirent *de) { return de->ino == 0 && de->name_len != GH_DIR_TOMB; }
static int slot_tomb(const struct gh_dirent *de)  { return de->ino == 0 && de->name_len == GH_DIR_TOMB; }

static int dir_init(struct gh_dev *dev, const struct gh_superblock *sb,
                    uint64_t dir_ino, struct gh_inode *dir) {
    struct gh_dirhdr h = { GH_DIRHDR_MAGIC, 0, GH_DIR_INIT_SLOTS, 0 };
    int r = wr_hdr(dev, sb, dir_ino, dir, &h); if (r) return r;
    struct gh_dirent empty; memset(&empty, 0, sizeof(empty));
    for (uint32_t i = 1; i <= GH_DIR_INIT_SLOTS; i++) {
        r = wr_slot(dev, sb, dir_ino, dir, i, &empty); if (r) return r;
    }
    return 0;
}

static int dir_rehash(struct gh_dev *dev, const struct gh_superblock *sb,
                      uint64_t dir_ino, struct gh_inode *dir, uint32_t target_nslots) {
    struct gh_dirhdr h; int r = rd_hdr(dev, sb, dir, &h); if (r) return r;
    uint32_t old = h.nslots;
    struct gh_dirent *ents = malloc((size_t)old * sizeof(struct gh_dirent));
    if (!ents) return -ENOMEM;
    uint32_t cnt = 0;
    for (uint32_t s = 1; s <= old; s++) {
        struct gh_dirent de; r = rd_slot(dev, sb, dir, s, &de);
        if (r) { free(ents); return r; }
        if (de.ino != 0) ents[cnt++] = de;   /* tylko zajete; tombstony znikaja */
    }
    uint32_t newn = target_nslots;
    struct gh_dirhdr nh = { GH_DIRHDR_MAGIC, cnt, newn, cnt };
    r = wr_hdr(dev, sb, dir_ino, dir, &nh); if (r) { free(ents); return r; }
    struct gh_dirent empty; memset(&empty, 0, sizeof(empty));
    for (uint32_t i = 1; i <= newn; i++) {
        r = wr_slot(dev, sb, dir_ino, dir, i, &empty); if (r) { free(ents); return r; }
    }
    for (uint32_t e = 0; e < cnt; e++) {
        uint64_t hh = gh_crc32(ents[e].name, ents[e].name_len) % newn;
        for (uint32_t p = 0; p < newn; p++) {
            uint64_t fs = 1 + (hh + p) % newn;
            struct gh_dirent de; r = rd_slot(dev, sb, dir, fs, &de); if (r) { free(ents); return r; }
            if (slot_empty(&de)) { r = wr_slot(dev, sb, dir_ino, dir, fs, &ents[e]); if (r) { free(ents); return r; } break; }
        }
    }
    free(ents);
    return 0;
}

int gh_dir_lookup(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t dir_ino, const char *name, uint64_t *out_ino) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return -ENOENT;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -ENOENT;
    size_t nl = strlen(name); if (nl > GH_NAME_MAX) return -ENAMETOOLONG;
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de)) return -ENOENT;
        if (de.ino != 0 && de.name_len == nl && memcmp(de.name, name, nl) == 0) { *out_ino = de.ino; return 0; }
    }
    return -ENOENT;
}

int gh_dir_add(struct gh_dev *dev, const struct gh_superblock *sb,
               uint64_t dir_ino, const char *name, uint64_t ino) {
    size_t nl = strlen(name); if (nl > GH_NAME_MAX) return -ENAMETOOLONG;
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) {
        r = dir_init(dev, sb, dir_ino, &dir); if (r) return r;
        r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    }
    uint64_t ex; if (gh_dir_lookup(dev, sb, dir_ino, name, &ex) == 0) return -EEXIST;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -EIO;
    if ((uint64_t)(h.used + 1) * 4 > (uint64_t)h.nslots * 3) {
        /* gdy zywych duzo -> podwoj; gdy dominuja tombstony -> rehash in-place tym samym nslots */
        uint32_t target = ((uint64_t)(h.live + 1) * 4 > (uint64_t)h.nslots * 3) ? h.nslots * 2 : h.nslots;
        r = dir_rehash(dev, sb, dir_ino, &dir, target); if (r) return r;
        r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
        r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    }
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de) || slot_tomb(&de)) {
            int was_empty = slot_empty(&de);
            struct gh_dirent ne; memset(&ne, 0, sizeof(ne));
            ne.ino = ino; ne.name_len = (uint16_t)nl; memcpy(ne.name, name, nl);
            r = wr_slot(dev, sb, dir_ino, &dir, fs, &ne); if (r) return r;
            if (was_empty) h.used++;   /* pusty->zajety: used i live rosna */
            h.live++;                  /* tomb->zajety: tylko live rosnie */
            r = wr_hdr(dev, sb, dir_ino, &dir, &h); if (r) return r;
            return 0;
        }
    }
    return -ENOSPC;
}

int gh_dir_remove(struct gh_dev *dev, const struct gh_superblock *sb,
                  uint64_t dir_ino, const char *name) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return -ENOENT;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -ENOENT;
    size_t nl = strlen(name);
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de)) return -ENOENT;
        if (de.ino != 0 && de.name_len == nl && memcmp(de.name, name, nl) == 0) {
            struct gh_dirent tomb; memset(&tomb, 0, sizeof(tomb)); tomb.name_len = GH_DIR_TOMB;
            r = wr_slot(dev, sb, dir_ino, &dir, fs, &tomb); if (r) return r;
            h.live--;   /* zajety->tomb: used bez zmian, live maleje */
            return wr_hdr(dev, sb, dir_ino, &dir, &h);
        }
    }
    return -ENOENT;
}

int gh_dir_set_ino(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t dir_ino, const char *name, uint64_t new_ino) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return -ENOENT;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC || h.nslots == 0) return -ENOENT;
    size_t nl = strlen(name);
    uint64_t hh = gh_crc32(name, nl) % h.nslots;
    for (uint32_t p = 0; p < h.nslots; p++) {
        uint64_t fs = 1 + (hh + p) % h.nslots;
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, fs, &de); if (r) return r;
        if (slot_empty(&de)) return -ENOENT;
        if (de.ino != 0 && de.name_len == nl && memcmp(de.name, name, nl) == 0) {
            de.ino = new_ino;
            return wr_slot(dev, sb, dir_ino, &dir, fs, &de);
        }
    }
    return -ENOENT;
}

int gh_dir_is_empty(struct gh_dev *dev, const struct gh_superblock *sb,
                    uint64_t dir_ino, int *empty) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    *empty = 1;
    if (dir.size < DENT_SZ * 2) return 0;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC) return 0;
    for (uint32_t s = 1; s <= h.nslots; s++) {
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, s, &de); if (r) return r;
        if (de.ino != 0 && !(de.name_len == 1 && de.name[0] == '.')
            && !(de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.')) { *empty = 0; return 0; }
    }
    return 0;
}

int gh_dir_iterate(struct gh_dev *dev, const struct gh_superblock *sb,
                   uint64_t dir_ino, gh_dir_iter_fn cb, void *ctx) {
    struct gh_inode dir; int r = gh_inode_read(dev, sb, dir_ino, &dir); if (r) return r;
    if (dir.size < DENT_SZ * 2) return 0;
    struct gh_dirhdr h; r = rd_hdr(dev, sb, &dir, &h); if (r) return r;
    if (h.magic != GH_DIRHDR_MAGIC) return 0;
    for (uint32_t s = 1; s <= h.nslots; s++) {
        struct gh_dirent de; r = rd_slot(dev, sb, &dir, s, &de); if (r) return r;
        if (de.ino != 0) { int c = cb(&de, ctx); if (c) return c; }
    }
    return 0;
}

int gh_path_split(const char *path, char *parent, char *name) {
    const char *slash = strrchr(path, '/');
    if (!slash) return -EINVAL;
    size_t plen = (size_t)(slash - path);
    /* czesc-rodzic + '\0' musi sie zmiescic w buforze 'parent' (1024 B) */
    if (plen >= GH_PARENT_MAX) return -ENAMETOOLONG;
    if (plen == 0) { strcpy(parent, "/"); }
    else { memcpy(parent, path, plen); parent[plen] = '\0'; }
    if (strlen(slash + 1) > GH_NAME_MAX) return -ENAMETOOLONG;
    strcpy(name, slash + 1);
    return 0;
}

int gh_path_resolve(struct gh_dev *dev, const struct gh_superblock *sb,
                    const char *path, uint64_t *out_ino) {
    if (path[0] != '/') return -EINVAL;
    uint64_t cur = sb->root_inode;
    char comp[GH_NAME_MAX + 1]; size_t ci = 0;
    for (const char *p = path + 1; ; p++) {
        if (*p == '/' || *p == '\0') {
            if (ci > 0) {
                comp[ci] = '\0';
                uint64_t next; int r = gh_dir_lookup(dev, sb, cur, comp, &next);
                if (r) return r;
                cur = next; ci = 0;
            }
            if (*p == '\0') break;
        } else {
            if (ci >= GH_NAME_MAX) return -ENAMETOOLONG;
            comp[ci++] = *p;
        }
    }
    *out_ino = cur; return 0;
}
