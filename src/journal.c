#include "journal.h"
#include "csum.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define GH_JPTRS (GH_BLOCK_SIZE / 8)   /* 512 numerów bloków na blok deskryptora */

static int raw_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    return gh_disk_write(dev, blkno, buf);
}
static int raw_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    return gh_disk_read(dev, blkno, buf);
}

int gh_jrnl_open(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (sb->journal_blocks == 0) { dev->txn = NULL; return 0; }
    struct gh_txn *t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    uint64_t avail = sb->journal_blocks - 1;
    uint64_t dmax = avail / GH_JPTRS + 1;
    if (avail <= dmax) { free(t); return -ENOSPC; }
    t->cap = (uint32_t)(avail - dmax);
    t->blknos = malloc((size_t)t->cap * sizeof(uint64_t));
    t->images = malloc((size_t)t->cap * GH_BLOCK_SIZE);
    if (!t->blknos || !t->images) { free(t->blknos); free(t->images); free(t); return -ENOMEM; }
    t->n = 0; t->active = 1;
    t->op_active = 0; t->savepoint_n = 0; t->savepoint_nd = 0;
    t->undo = NULL; t->nundo = 0; t->undocap = 0; t->dirty = 0;
    dev->txn = t;
    return 0;
}

void gh_jrnl_op_begin(struct gh_dev *dev) {
    struct gh_txn *t = dev->txn;
    if (!t) return;
    t->savepoint_n = t->n; t->savepoint_nd = dev->nd; t->nundo = 0; t->op_active = 1;
}

void gh_jrnl_op_commit(struct gh_dev *dev) {
    struct gh_txn *t = dev->txn;
    if (!t) return;
    t->nundo = 0; t->op_active = 0; t->dirty = 1;
}

void gh_jrnl_op_rollback(struct gh_dev *dev) {
    struct gh_txn *t = dev->txn;
    if (!t) return;
    for (uint32_t k = t->nundo; k > 0; k--)             /* odtworz w odwrotnej kolejnosci */
        memcpy(t->images[t->undo[k-1].idx], t->undo[k-1].img, GH_BLOCK_SIZE);
    t->n = t->savepoint_n;
    dev->nd = t->savepoint_nd;                          /* porzuc discardy operacji */
    t->nundo = 0; t->op_active = 0;
}

void gh_jrnl_close(struct gh_dev *dev) {
    gh_discard_clear(dev);
    if (!dev->txn) return;
    free(dev->txn->undo);
    free(dev->txn->blknos); free(dev->txn->images); free(dev->txn);
    dev->txn = NULL;
}

int gh_jrnl_flush(struct gh_dev *dev, const struct gh_superblock *sb) {
    struct gh_txn *t = dev->txn;
    if (!t) return 0;                                   /* tryb bez dziennika */
    if (!t->dirty || t->n == 0) { t->dirty = 0; return 0; }
    int rc = -EIO;
    uint64_t js = sb->journal_start;
    uint64_t dblocks = (t->n + GH_JPTRS - 1) / GH_JPTRS;
    uint8_t blk[GH_BLOCK_SIZE];

    for (uint64_t d = 0; d < dblocks; d++) {
        memset(blk, 0, sizeof(blk));
        for (uint32_t k = 0; k < GH_JPTRS; k++) {
            uint32_t idx = (uint32_t)(d * GH_JPTRS + k);
            if (idx >= t->n) break;
            memcpy(blk + k * 8, &t->blknos[idx], 8);
        }
        { int r = raw_write(dev, js + 1 + d, blk); if (r) { rc = r; goto fail; } }
    }
    for (uint32_t i = 0; i < t->n; i++)
        { int r = raw_write(dev, js + 1 + dblocks + i, t->images[i]); if (r) { rc = r; goto fail; } }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    struct gh_jheader h; memset(&h, 0, sizeof(h));
    memcpy(h.magic, GH_JMAGIC, 8);
    h.seq = 1; h.committed = 1; h.n_blocks = t->n; h.descriptor_blocks = dblocks;
    /* CRC nad numerami blokow + obrazami (wykrywa rozdarty zapis dziennika) */
    {
        uint32_t c = 0xFFFFFFFFu;
        c = gh_crc32_update(c, t->blknos, (size_t)t->n * sizeof(uint64_t));
        for (uint32_t i = 0; i < t->n; i++)
            c = gh_crc32_update(c, t->images[i], GH_BLOCK_SIZE);
        h.csum = c ^ 0xFFFFFFFFu;
    }
    memset(blk, 0, sizeof(blk)); memcpy(blk, &h, sizeof(h));
    if (raw_write(dev, js, blk)) { rc = -EIO; goto fail; }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    for (uint32_t i = 0; i < t->n; i++)
        { int r = raw_write(dev, t->blknos[i], t->images[i]); if (r) { rc = r; goto fail; } }
    if (fsync(dev->fd)) { rc = -EIO; goto fail; }

    memset(blk, 0, sizeof(blk));
    if (raw_write(dev, js, blk)) { rc = -EIO; goto fail; }
    /* leniwe czyszczenie naglowka: bez fsync — recover odtworzy idempotentnie,
       a CRC dziennika chroni przed rozdartym zapisem (J) */

    gh_discard_flush(dev, sb);
    t->n = 0; t->dirty = 0;          /* reset bufora, zostaw otwarty na kolejna paczke */
    return 0;
fail:
    gh_jrnl_recover(dev, sb);        /* dokoncz ewentualny committed txn na dysku (idempotentne) */
    gh_discard_clear(dev);
    t->n = 0; t->dirty = 0;
    return rc;
}

int gh_jrnl_recover(struct gh_dev *dev, const struct gh_superblock *sb) {
    if (sb->journal_blocks == 0) return 0;
    uint64_t js = sb->journal_start;
    uint8_t blk[GH_BLOCK_SIZE];
    if (raw_read(dev, js, blk)) return 0;        /* nie da sie czytac -> pomin */
    struct gh_jheader h; memcpy(&h, blk, sizeof(h));
    if (memcmp(h.magic, GH_JMAGIC, 8) != 0) return 0;
    if (!h.committed) return 0;                  /* nie zatwierdzone -> nic */
    if (h.n_blocks == 0 || h.n_blocks > sb->journal_blocks) return 0;
    uint64_t need = (h.n_blocks + GH_JPTRS - 1) / GH_JPTRS;
    if (h.descriptor_blocks != need) return 0;                 /* niespojny deskryptor -> traktuj jak brak txn */
    if (1 + h.descriptor_blocks + h.n_blocks > sb->journal_blocks) return 0;  /* nie miesci sie w regionie */

    uint64_t *targets = malloc((size_t)h.n_blocks * sizeof(uint64_t));
    if (!targets) return -ENOMEM;
    for (uint64_t d = 0; d < h.descriptor_blocks; d++) {
        uint8_t db[GH_BLOCK_SIZE];
        if (raw_read(dev, js + 1 + d, db)) { free(targets); return -EIO; }
        for (uint32_t k = 0; k < GH_JPTRS; k++) {
            uint32_t idx = (uint32_t)(d * GH_JPTRS + k);
            if (idx >= h.n_blocks) break;
            memcpy(&targets[idx], db + k * 8, 8);
        }
    }
    /* CRC nad numerami + obrazami (jak we flush); niezgodnosc = rozdarty zapis -> nie odtwarzaj */
    uint8_t (*imgs)[GH_BLOCK_SIZE] = malloc((size_t)h.n_blocks * GH_BLOCK_SIZE);
    if (!imgs) { free(targets); return -ENOMEM; }
    for (uint32_t i = 0; i < h.n_blocks; i++) {
        if (raw_read(dev, js + 1 + h.descriptor_blocks + i, imgs[i])) {
            free(imgs); free(targets); return -EIO;
        }
    }
    {
        uint32_t c = 0xFFFFFFFFu;
        c = gh_crc32_update(c, targets, (size_t)h.n_blocks * sizeof(uint64_t));
        for (uint32_t i = 0; i < h.n_blocks; i++)
            c = gh_crc32_update(c, imgs[i], GH_BLOCK_SIZE);
        if ((c ^ 0xFFFFFFFFu) != h.csum) { free(imgs); free(targets); return 0; }   /* rozdarcie -> noop */
    }
    for (uint32_t i = 0; i < h.n_blocks; i++) {
        if (targets[i] < sb->total_blocks) {
            if (raw_write(dev, targets[i], imgs[i])) { free(imgs); free(targets); return -EIO; }
        }
    }
    free(imgs);
    free(targets);
    if (fsync(dev->fd)) return -EIO;
    memset(blk, 0, sizeof(blk));               /* wyczysc naglowek */
    if (raw_write(dev, js, blk)) return -EIO;
    if (fsync(dev->fd)) return -EIO;
    return 0;
}
