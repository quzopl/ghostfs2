#ifndef GH_BLOCK_H
#define GH_BLOCK_H
#include <stdint.h>
#include <pthread.h>
#include "ghostfs.h"

struct gh_undo { uint32_t idx; uint8_t img[GH_BLOCK_SIZE]; };
struct gh_txn {
    int       active;
    uint64_t *blknos;                  /* docelowe numery bloków (deduplikowane) */
    uint8_t (*images)[GH_BLOCK_SIZE];  /* obrazy bloków */
    uint32_t  n, cap;
    int       op_active;
    uint32_t  savepoint_n;
    uint32_t  savepoint_nd;
    struct gh_undo *undo; uint32_t nundo, undocap;
    int       dirty;
};

struct gh_cipher;   /* zdefiniowane w crypto.h */

struct gh_bentry { uint64_t blkno; int valid; uint8_t data[GH_BLOCK_SIZE]; };
struct gh_bcache { pthread_mutex_t lock; uint32_t nslots; struct gh_bentry *slots; };
#define GH_BCACHE_SLOTS 1024u

struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn;
                struct gh_cipher *cipher; long fail_after;
                uint64_t hint_block; uint64_t hint_inode; struct gh_bcache *cache;
                int is_blkdev; uint64_t *discards; uint32_t nd; uint32_t dcap;
                int checksums; uint64_t csum_start, csum_blocks, jrnl_start, jrnl_blocks; };

int  gh_bcache_create(struct gh_dev *dev);
void gh_bcache_destroy(struct gh_dev *dev);

int  gh_dev_create(const char *path, uint64_t total_blocks, struct gh_dev *dev);
int  gh_dev_open(const char *path, struct gh_dev *dev);
void gh_dev_close(struct gh_dev *dev);
int  gh_disk_read(struct gh_dev *dev, uint64_t blkno, void *buf);
int  gh_disk_write(struct gh_dev *dev, uint64_t blkno, const void *buf);
int  gh_block_read(struct gh_dev *dev, uint64_t blkno, void *buf);
int  gh_block_write(struct gh_dev *dev, uint64_t blkno, const void *buf);
int  gh_block_write_direct(struct gh_dev *dev, uint64_t blkno, const void *buf);

int  gh_disk_discard(struct gh_dev *dev, uint64_t blkno, uint64_t count);
void gh_discard_pending_add(struct gh_dev *dev, uint64_t blkno);
void gh_discard_clear(struct gh_dev *dev);
int  gh_discard_flush(struct gh_dev *dev, const struct gh_superblock *sb);
#endif
