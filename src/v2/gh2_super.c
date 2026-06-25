#define _POSIX_C_SOURCE 200809L
#include "v2/gh2_super.h"
#include "csum.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* superblok jest NIESZYFROWANY (trzyma sol) -> raw pread/pwrite na dev->fd. */

uint32_t gh2_sb_csum(const struct gh2_superblock *sb) {
    /* suma ze wszystkiego przed polem sb_csum (ostatnie pole struktury) */
    size_t n = offsetof(struct gh2_superblock, sb_csum);
    return gh_crc32(sb, n);
}

static int sb_write_slot(struct gh_dev *dev, int slot, const struct gh2_superblock *sb) {
    /* fault injection (sweep crash-consistency): failuj N-ty zapis (jak gh_disk_write),
       by pokryc rowniez punkt atomowej podmiany korzenia (zapis superbloku). Aktywne
       tylko gdy test ustawi dev->fail_after > 0. */
    if (dev->fail_after > 0) { if (--dev->fail_after == 0) return -EIO; }
    uint8_t blk[GH2_BLOCK_SIZE];
    memset(blk, 0, sizeof(blk));
    memcpy(blk, sb, sizeof(*sb));
    off_t off = (off_t)slot * GH2_BLOCK_SIZE;
    ssize_t w = pwrite(dev->fd, blk, GH2_BLOCK_SIZE, off);
    if (w != GH2_BLOCK_SIZE) return -EIO;
    return 0;
}

static int sb_read_slot(struct gh_dev *dev, int slot, struct gh2_superblock *out, int *valid) {
    uint8_t blk[GH2_BLOCK_SIZE];
    off_t off = (off_t)slot * GH2_BLOCK_SIZE;
    ssize_t r = pread(dev->fd, blk, GH2_BLOCK_SIZE, off);
    if (r != GH2_BLOCK_SIZE) return -EIO;
    struct gh2_superblock sb;
    memcpy(&sb, blk, sizeof(sb));
    *valid = 0;
    if (memcmp(sb.magic, GH2_MAGIC, GH2_MAGIC_LEN) != 0) return 0;
    if (sb.block_size != GH2_BLOCK_SIZE) return 0;
    if (gh2_sb_csum(&sb) != sb.sb_csum) return 0;       /* rozdarcie/korupcja */
    *out = sb; *valid = 1;
    return 0;
}

int gh2_format(struct gh_dev *dev, uint64_t total_blocks, uint32_t flags) {
    if (total_blocks <= GH2_DATA_START) return -EINVAL;
    struct gh2_superblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, GH2_MAGIC, GH2_MAGIC_LEN);
    sb.generation = 1;
    sb.total_blocks = total_blocks;
    sb.block_size = GH2_BLOCK_SIZE;
    sb.flags = flags;
    sb.next_free = GH2_DATA_START;       /* region danych od bloku 2 */
    sb.root_dir_ino = 0;
    /* root_tree zerowy (stub) — drzewa w v2.1+ */
    sb.sb_csum = gh2_sb_csum(&sb);
    /* bootstrap: oba sloty gen=1 (mount wybierze ktorykolwiek) */
    int r = sb_write_slot(dev, GH2_SB_SLOT_A, &sb); if (r) return r;
    r = sb_write_slot(dev, GH2_SB_SLOT_B, &sb); if (r) return r;
    if (fsync(dev->fd)) return -EIO;
    return 0;
}

int gh2_mount(struct gh_dev *dev, struct gh2_superblock *out) {
    struct gh2_superblock a, b; int va = 0, vb = 0;
    int r = sb_read_slot(dev, GH2_SB_SLOT_A, &a, &va); if (r) return r;
    r = sb_read_slot(dev, GH2_SB_SLOT_B, &b, &vb); if (r) return r;
    if (va && vb) { *out = (a.generation >= b.generation) ? a : b; return 0; }
    if (va) { *out = a; return 0; }
    if (vb) { *out = b; return 0; }
    return -EINVAL;          /* brak waznego superbloku */
}

int gh2_commit_super(struct gh_dev *dev, struct gh2_superblock *sb) {
    uint64_t old_gen = sb->generation;
    sb->generation += 1;
    sb->block_size = GH2_BLOCK_SIZE;
    memcpy(sb->magic, GH2_MAGIC, GH2_MAGIC_LEN);
    sb->sb_csum = gh2_sb_csum(sb);
    int slot = (int)(sb->generation & 1);    /* nigdy slot z generacji-1 (przeciwna parzystosc) */
    int r = sb_write_slot(dev, slot, sb);
    if (r == 0 && fsync(dev->fd)) r = -EIO;
    if (r == 0) {
        /* read-back verify: wykryj rozdarty zapis NATYCHMIAST (inaczej kolejny commit
           nadpisalby ostatni dobry slot — drugi slot wciaz trzyma old_gen, wiec stan caly) */
        struct gh2_superblock chk; int valid = 0;
        r = sb_read_slot(dev, slot, &chk, &valid);
        if (r == 0 && (!valid || chk.generation != sb->generation))
            r = -EIO;
    }
    if (r) {
        /* commit nieudany: przywroc generacje do ostatniej trwalej (drugi slot = old_gen),
           by ponowna proba pisala TEN slot (rozdarty), nie nadpisala dobrego */
        sb->generation = old_gen;
        sb->sb_csum = gh2_sb_csum(sb);
        return r;
    }
    return 0;
}
