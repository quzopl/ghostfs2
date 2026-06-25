#define _GNU_SOURCE
#include "block.h"
#include "ghostfs.h"
#include "crypto.h"
#include "csum.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

int gh_dev_create(const char *path, uint64_t total_blocks, struct gh_dev *dev) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -errno;
    off_t bytes = (off_t)total_blocks * GH_BLOCK_SIZE;
    if (ftruncate(fd, bytes) != 0) { int e = -errno; close(fd); return e; }
    dev->fd = fd; dev->total_blocks = total_blocks; dev->txn = NULL; dev->cipher = NULL;
    dev->fail_after = 0;
    dev->hint_block = 0; dev->hint_inode = 0;
    dev->cache = NULL;
    dev->is_blkdev = 0; dev->discards = NULL; dev->nd = 0; dev->dcap = 0;
    dev->checksums = 0; dev->csum_start = 0; dev->csum_blocks = 0;
    dev->jrnl_start = 0; dev->jrnl_blocks = 0;
    dev->v2_ncache = NULL;
    dev->v2_rcache = NULL;
    return 0;
}

int gh_dev_open(const char *path, struct gh_dev *dev) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = -errno; close(fd); return e; }
    uint64_t bytes; int is_blk = 0;
    if (S_ISBLK(st.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) { int e = -errno; close(fd); return e; }
        is_blk = 1;
    } else {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0) { int e = -errno; close(fd); return e; }
        bytes = (uint64_t)end;
    }
    dev->fd = fd;
    dev->total_blocks = bytes / GH_BLOCK_SIZE;
    dev->txn = NULL;
    dev->cipher = NULL;
    dev->fail_after = 0;
    dev->hint_block = 0; dev->hint_inode = 0;
    dev->cache = NULL;
    dev->is_blkdev = is_blk; dev->discards = NULL; dev->nd = 0; dev->dcap = 0;
    dev->checksums = 0; dev->csum_start = 0; dev->csum_blocks = 0;
    dev->jrnl_start = 0; dev->jrnl_blocks = 0;
    dev->v2_ncache = NULL;
    dev->v2_rcache = NULL;
    return 0;
}

int gh_bcache_create(struct gh_dev *dev) {
    struct gh_bcache *c = calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    c->nslots = GH_BCACHE_SLOTS;
    c->slots = calloc(c->nslots, sizeof(struct gh_bentry));
    if (!c->slots) { free(c); return -ENOMEM; }
    if (pthread_mutex_init(&c->lock, NULL) != 0) { free(c->slots); free(c); return -EIO; }
    dev->cache = c;
    return 0;
}
void gh_bcache_destroy(struct gh_dev *dev) {
    if (!dev->cache) return;
    pthread_mutex_destroy(&dev->cache->lock);
    free(dev->cache->slots); free(dev->cache); dev->cache = NULL;
}

void gh_dev_close(struct gh_dev *dev) {
    if (!dev) return;
    if (dev->discards) { free(dev->discards); dev->discards = NULL; dev->nd = dev->dcap = 0; }
    if (dev->fd >= 0) { close(dev->fd); dev->fd = -1; }
}

int gh_disk_discard(struct gh_dev *dev, uint64_t blkno, uint64_t count) {
    if (count == 0) return 0;
    uint64_t off = blkno * GH_BLOCK_SIZE;
    uint64_t len = count * GH_BLOCK_SIZE;
    if (dev->is_blkdev) {
        uint64_t range[2] = { off, len };
        if (ioctl(dev->fd, BLKDISCARD, range) != 0) {
            if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL) return 0;
            return -errno;
        }
    } else {
        if (fallocate(dev->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      (off_t)off, (off_t)len) != 0) {
            if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL) return 0;
            return -errno;
        }
    }
    return 0;
}

void gh_discard_pending_add(struct gh_dev *dev, uint64_t blkno) {
    if (dev->nd >= dev->dcap) {
        uint32_t nc = dev->dcap ? dev->dcap * 2 : 64;
        uint64_t *p = realloc(dev->discards, (size_t)nc * sizeof(uint64_t));
        if (!p) return;                 /* best-effort: porzuc gdy brak pamieci */
        dev->discards = p; dev->dcap = nc;
    }
    dev->discards[dev->nd++] = blkno;
}

void gh_discard_clear(struct gh_dev *dev) { dev->nd = 0; }

unsigned long gh_disk_read_count = 0;

int gh_disk_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    if (blkno >= dev->total_blocks) return -EINVAL;
    gh_disk_read_count++;
    struct gh_bcache *c = dev->cache;
    if (c && blkno != 0) {
        struct gh_bentry *e = &c->slots[blkno % c->nslots];
        pthread_mutex_lock(&c->lock);
        if (e->valid && e->blkno == blkno) {
            memcpy(buf, e->data, GH_BLOCK_SIZE);
            pthread_mutex_unlock(&c->lock);
            return 0;
        }
        pthread_mutex_unlock(&c->lock);
    }
    off_t off = (off_t)blkno * GH_BLOCK_SIZE;
    if (dev->cipher && blkno != 0) {
        uint8_t tmp[GH_BLOCK_SIZE];
        ssize_t n = pread(dev->fd, tmp, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
        int r = gh_crypto_decrypt_block(dev->cipher, blkno, tmp, buf);
        if (r) return r;
    } else {
        ssize_t n = pread(dev->fd, buf, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
    }
    if (c && blkno != 0) {
        struct gh_bentry *e = &c->slots[blkno % c->nslots];
        pthread_mutex_lock(&c->lock);
        e->blkno = blkno; e->valid = 1; memcpy(e->data, buf, GH_BLOCK_SIZE);
        pthread_mutex_unlock(&c->lock);
    }
    return 0;
}

/* instrumentacja zapisow (testy/benchmarki) — domyslnie nieaktywna (zera) */
unsigned long gh_disk_write_count = 0;
uint64_t      gh_disk_write_watch = 0;
unsigned long gh_disk_write_watch_hits = 0;

int gh_disk_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (dev->fail_after > 0) {
        if (--dev->fail_after == 0) return -EIO;   /* symulacja awarii zapisu */
    }
    if (blkno >= dev->total_blocks) return -EINVAL;
    gh_disk_write_count++;
    if (gh_disk_write_watch != 0 && blkno == gh_disk_write_watch) gh_disk_write_watch_hits++;
    off_t off = (off_t)blkno * GH_BLOCK_SIZE;
    if (dev->cipher && blkno != 0) {
        uint8_t tmp[GH_BLOCK_SIZE];
        int r = gh_crypto_encrypt_block(dev->cipher, blkno, buf, tmp); if (r) return r;
        ssize_t n = pwrite(dev->fd, tmp, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
    } else {
        ssize_t n = pwrite(dev->fd, buf, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
    }
    struct gh_bcache *c = dev->cache;
    if (c && blkno != 0) {
        struct gh_bentry *e = &c->slots[blkno % c->nslots];
        pthread_mutex_lock(&c->lock);
        e->blkno = blkno; e->valid = 1; memcpy(e->data, buf, GH_BLOCK_SIZE);
        pthread_mutex_unlock(&c->lock);
    }
    return 0;
}

/* czy blok podlega sumowaniu: blok 0, region dziennika i region sum sa wylaczone
   (region sum wylaczony -> rekurencja gh_block_read/write(cb) jest bazowa) */
static int gh_is_csummed(struct gh_dev *dev, uint64_t blkno) {
    if (!dev->checksums) return 0;
    if (blkno == 0) return 0;
    if (dev->jrnl_blocks && blkno >= dev->jrnl_start && blkno < dev->jrnl_start + dev->jrnl_blocks) return 0;
    if (blkno >= dev->csum_start && blkno < dev->csum_start + dev->csum_blocks) return 0;
    return 1;
}

int gh_block_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    int r; int got = 0;
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) { memcpy(buf, t->images[i], GH_BLOCK_SIZE); got = 1; break; }
    }
    r = got ? 0 : gh_disk_read(dev, blkno, buf);
    if (r == 0 && gh_is_csummed(dev, blkno)) {
        uint32_t crc = gh_crc32(buf, GH_BLOCK_SIZE);
        uint64_t cb = dev->csum_start + (blkno * 4) / GH_BLOCK_SIZE;
        uint32_t coff = (uint32_t)((blkno * 4) % GH_BLOCK_SIZE);
        uint8_t cbuf[GH_BLOCK_SIZE];
        if (gh_block_read(dev, cb, cbuf) == 0) {     /* cb nie sumowany -> brak rekurencji */
            uint32_t stored; memcpy(&stored, cbuf + coff, 4);
            if (stored != 0 && stored != crc) return -EIO;   /* korupcja */
        }
    }
    return r;
}
static int gh_txn_undo_push(struct gh_txn *t, uint32_t idx) {
    if (t->nundo >= t->undocap) {
        uint32_t nc = t->undocap ? t->undocap * 2 : 16;
        struct gh_undo *u = realloc(t->undo, (size_t)nc * sizeof(struct gh_undo));
        if (!u) return -ENOMEM;
        t->undo = u; t->undocap = nc;
    }
    t->undo[t->nundo].idx = idx;
    memcpy(t->undo[t->nundo].img, t->images[idx], GH_BLOCK_SIZE);
    t->nundo++;
    return 0;
}

int gh_block_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (gh_is_csummed(dev, blkno)) {
        uint32_t crc = gh_crc32(buf, GH_BLOCK_SIZE);
        uint64_t cb = dev->csum_start + (blkno * 4) / GH_BLOCK_SIZE;
        uint32_t coff = (uint32_t)((blkno * 4) % GH_BLOCK_SIZE);
        uint8_t cbuf[GH_BLOCK_SIZE];
        int r = gh_block_read(dev, cb, cbuf); if (r) return r;
        memcpy(cbuf + coff, &crc, 4);
        r = gh_block_write(dev, cb, cbuf); if (r) return r;   /* rekurencja bazowa: cb nie sumowany */
    }
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) {
                if (t->op_active && i < t->savepoint_n) {
                    if (gh_txn_undo_push(t, i) != 0) return -ENOMEM;  /* nie modyfikuj bez undo */
                }
                memcpy(t->images[i], buf, GH_BLOCK_SIZE);
                return 0;
            }
        if (t->n >= t->cap) return -ENOSPC;
        t->blknos[t->n] = blkno; memcpy(t->images[t->n], buf, GH_BLOCK_SIZE); t->n++;
        return 0;
    }
    return gh_disk_write(dev, blkno, buf);
}

/* Zapis bloku danych z pominieciem dziennika: SUMA journalowana (spojna po commicie),
   DANE bezposrednio na dysk (pwrite + cache). Tylko dla NOWO-alokowanych blokow plikow,
   ktore nie sa jeszcze referencyjne do czasu commitu metadanych (mapa/i-wezel). */
int gh_block_write_direct(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (gh_is_csummed(dev, blkno)) {
        uint32_t crc = gh_crc32(buf, GH_BLOCK_SIZE);
        uint64_t cb = dev->csum_start + (blkno * 4) / GH_BLOCK_SIZE;
        uint32_t coff = (uint32_t)((blkno * 4) % GH_BLOCK_SIZE);
        uint8_t cbuf[GH_BLOCK_SIZE];
        int r = gh_block_read(dev, cb, cbuf); if (r) return r;
        memcpy(cbuf + coff, &crc, 4);
        r = gh_block_write(dev, cb, cbuf); if (r) return r;   /* suma: do txn (journalowana) */
    }
    int r = gh_disk_write(dev, blkno, buf); if (r) return r;   /* dane: bezposrednio */
    /* Blok mogl byc wczesniej (w tej samej, niezflushowanej paczce) zapisany do bufora txn,
       potem zwolniony i REALOKOWANY jako ten liscia. Nieaktualny obraz w txn zacienialby
       odczyt (gh_block_read czyta z txn) ORAZ przy commicie nadpisalby bezposredni zapis.
       Zsynchronizuj wiec ewentualny obraz w buforze txn z trescia zapisana bezposrednio. */
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) {
                if (t->op_active && i < t->savepoint_n) {
                    if (gh_txn_undo_push(t, i) != 0) return -ENOMEM;
                }
                memcpy(t->images[i], buf, GH_BLOCK_SIZE);
                break;
            }
    }
    return 0;
}
