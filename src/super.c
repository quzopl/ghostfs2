#define _GNU_SOURCE
#include "super.h"
#include "inode.h"
#include "dir.h"
#include "crypto.h"
#include "csum.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>

static uint64_t ceil_div(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

int gh_format(const char *path, uint64_t total_blocks, uint64_t inode_count) {
    return gh_format_enc(path, total_blocks, inode_count, NULL);
}

int gh_format_enc(const char *path, uint64_t total_blocks, uint64_t inode_count,
                  const char *passphrase) {
    struct stat pst; int is_blk = 0;
    if (stat(path, &pst) == 0 && S_ISBLK(pst.st_mode)) {
        is_blk = 1;
        int tfd = open(path, O_RDWR); if (tfd < 0) return -errno;
        uint64_t bytes = 0; int ssz = 512;
        if (ioctl(tfd, BLKGETSIZE64, &bytes) != 0) { int e = -errno; close(tfd); return e; }
        ioctl(tfd, BLKSSZGET, &ssz);
        close(tfd);
        if (ssz > (int)GH_BLOCK_SIZE) return -EINVAL;       /* sektor > 4096 niewspierany */
        uint64_t dev_total = bytes / GH_BLOCK_SIZE;
        if (total_blocks == 0 || total_blocks > dev_total) total_blocks = dev_total;
    } else if (total_blocks == 0) {
        return -EINVAL;   /* plik nieistniejacy wymaga rozmiaru */
    }
    if (total_blocks < 8) return -EINVAL;
    /* układ: [0]=superblok, mapa, i-węzły, dane */
    uint64_t bitmap_blocks = ceil_div(total_blocks, GH_BLOCK_SIZE * 8);
    uint64_t inode_blocks  = ceil_div(inode_count, GH_INODES_PER_BLK);
    uint64_t bitmap_start = 1;
    uint64_t inode_start  = bitmap_start + bitmap_blocks;
    uint64_t journal_start = inode_start + inode_blocks;
    uint64_t journal_blocks = total_blocks / 16;
    if (journal_blocks < 8) journal_blocks = 8;
    if (journal_blocks > 4096) journal_blocks = 4096;
    uint64_t csum_blocks = (total_blocks * 4 + GH_BLOCK_SIZE - 1) / GH_BLOCK_SIZE;
    uint64_t csum_start = journal_start + journal_blocks;
    uint64_t data_start = csum_start + csum_blocks;
    if (data_start >= total_blocks) return -EINVAL;

    struct gh_dev dev;
    int r = is_blk ? gh_dev_open(path, &dev) : gh_dev_create(path, total_blocks, &dev);
    if (r) return r;

    dev.checksums = 1; dev.csum_start = csum_start; dev.csum_blocks = csum_blocks;
    dev.jrnl_start = journal_start; dev.jrnl_blocks = journal_blocks;

    gh_disk_discard(&dev, data_start, total_blocks - data_start);   /* czysty start na SSD */

    struct gh_superblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, GH_MAGIC, GH_MAGIC_LEN);
    sb.block_size = GH_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.inode_count = inode_blocks * GH_INODES_PER_BLK;
    sb.bitmap_start = bitmap_start;
    sb.inode_start = inode_start;
    sb.data_start = data_start;
    sb.root_inode = GH_ROOT_INO;
    sb.journal_start = journal_start;
    sb.journal_blocks = journal_blocks;
    sb.csum_start = csum_start;
    sb.csum_blocks = csum_blocks;
    sb.flags |= GH_SB_CHECKSUMS;
    sb.flags |= GH_SB_HASHDIR;

    /* opcjonalne szyfrowanie: wyprowadz klucz z hasla, ustaw pola superbloku */
    struct gh_cipher cipher; int encrypted = 0;
    if (passphrase != NULL) {
        if (passphrase[0] == '\0') { gh_dev_close(&dev); return -EINVAL; }  /* puste haslo = blad */
        encrypted = 1;
        sb.flags |= GH_SB_ENCRYPTED;
        sb.enc_kdf_iters = 200000;
        if (gh_crypto_random(sb.enc_salt, sizeof(sb.enc_salt))) { gh_dev_close(&dev); return -EIO; }
        if (gh_crypto_derive(passphrase, sb.enc_salt, sb.enc_kdf_iters, cipher.key)) { gh_dev_close(&dev); return -EIO; }
        gh_crypto_verifier(cipher.key, sb.enc_salt, sb.enc_verifier);
    }

    /* self-suma superbloku: liczona z sb_csum=0, PRZED zapisem bloku 0 */
    sb.sb_csum = 0;
    sb.sb_csum = gh_crc32(&sb, sizeof(sb));

    /* zapis superbloku (dopełniony do pełnego bloku) — JAWNIE (cipher jeszcze nie ustawiony) */
    uint8_t blk[GH_BLOCK_SIZE]; memset(blk, 0, sizeof(blk));
    memcpy(blk, &sb, sizeof(sb));
    if ((r = gh_block_write(&dev, 0, blk))) { gh_dev_close(&dev); return r; }

    /* od teraz wszystkie zapisy (mapa/i-węzły/dziennik/root) szyfrem */
    if (encrypted) dev.cipher = &cipher;

    /* wyzeruj mapę i tablicę i-węzłów */
    memset(blk, 0, sizeof(blk));
    for (uint64_t b = bitmap_start; b < data_start; b++)
        if ((r = gh_block_write(&dev, b, blk))) { gh_dev_close(&dev); return r; }

    /* zaznacz w mapie bity bloków metadanych [0, data_start) */
    for (uint64_t b = 0; b < data_start; b++) {
        uint64_t byte = b / 8, off = byte % GH_BLOCK_SIZE;
        uint64_t mblk = bitmap_start + byte / GH_BLOCK_SIZE;
        uint8_t mbuf[GH_BLOCK_SIZE];
        if ((r = gh_block_read(&dev, mblk, mbuf))) { gh_dev_close(&dev); return r; }
        mbuf[off] |= (uint8_t)(1u << (b % 8));
        if ((r = gh_block_write(&dev, mblk, mbuf))) { gh_dev_close(&dev); return r; }
    }

    /* root: i-węzeł GH_ROOT_INO typu katalog + wpisy "." i ".." */
    uint64_t iblk = inode_start + (GH_ROOT_INO / GH_INODES_PER_BLK);
    uint64_t idx  = GH_ROOT_INO % GH_INODES_PER_BLK;
    uint8_t ibuf[GH_BLOCK_SIZE];
    if ((r = gh_block_read(&dev, iblk, ibuf))) { gh_dev_close(&dev); return r; }
    struct gh_inode root; memset(&root, 0, sizeof(root));
    root.type = GH_DIR; root.mode = 0755; root.nlink = 2;
    memcpy(ibuf + idx * GH_INODE_SIZE, &root, sizeof(root));
    if ((r = gh_block_write(&dev, iblk, ibuf))) { gh_dev_close(&dev); return r; }

    if ((r = gh_dir_add(&dev, &sb, GH_ROOT_INO, ".", GH_ROOT_INO))) { gh_dev_close(&dev); return r; }
    if ((r = gh_dir_add(&dev, &sb, GH_ROOT_INO, "..", GH_ROOT_INO))) { gh_dev_close(&dev); return r; }

    dev.cipher = NULL;   /* higiena: cipher to wskaznik na lokalny obiekt */
    gh_dev_close(&dev);
    return 0;
}

int gh_mount_sb(struct gh_dev *dev, struct gh_superblock *sb) {
    uint8_t blk[GH_BLOCK_SIZE];
    int r = gh_block_read(dev, 0, blk);
    if (r) return r;
    if (memcmp(blk, GH_MAGIC, GH_MAGIC_LEN) != 0) return -EINVAL;
    memcpy(sb, blk, sizeof(*sb));
    if (sb->block_size != GH_BLOCK_SIZE) return -EINVAL;
    if (sb->data_start >= sb->total_blocks) return -EINVAL;
    if (sb->csum_blocks > 0) {
        struct gh_superblock tmp = *sb;
        tmp.sb_csum = 0;
        if (gh_crc32(&tmp, sizeof(tmp)) != sb->sb_csum) return -EINVAL;
        dev->checksums = 1;
        dev->csum_start = sb->csum_start;
        dev->csum_blocks = sb->csum_blocks;
        dev->jrnl_start = sb->journal_start;
        dev->jrnl_blocks = sb->journal_blocks;
    }
    return 0;
}
