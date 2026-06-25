#ifndef GHOSTFS_H
#define GHOSTFS_H
#include <stdint.h>

#define GH_BLOCK_SIZE   4096u
#define GH_MAGIC        "GHOSTFS\x01"   /* 8 bajtów */
#define GH_MAGIC_LEN    8
#define GH_NDIRECT      12
#define GH_PTRS_PER_BLK (GH_BLOCK_SIZE / 8)   /* 512 */
#define GH_INODE_SIZE   256u
#define GH_INODES_PER_BLK (GH_BLOCK_SIZE / GH_INODE_SIZE) /* 16 */
#define GH_NAME_MAX     255
#define GH_ROOT_INO     1
#define GH_SB_ENCRYPTED 0x1u
#define GH_SB_CHECKSUMS 0x2u
#define GH_SB_HASHDIR   0x4u
#define GH_DIR_INIT_SLOTS 16
#define GH_DIR_TOMB     0xFFFFu
#define GH_XATTR_MAX    GH_BLOCK_SIZE   /* limit laczny xattr na i-wezel */
#define GH_SYMLINK_MAX  GH_BLOCK_SIZE   /* limit dlugosci celu symlinka */

enum gh_itype { GH_FREE = 0, GH_FILE = 1, GH_DIR = 2, GH_SYMLINK = 3,
                GH_FIFO = 4, GH_SOCK = 5, GH_CHR = 6, GH_BLK = 7 };

struct gh_superblock {            /* leży w bloku 0, dopełniony do 4096 B */
    uint8_t  magic[GH_MAGIC_LEN];
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t bitmap_start;
    uint64_t inode_start;
    uint64_t data_start;
    uint64_t root_inode;
    uint64_t journal_start;   /* pierwszy blok regionu dziennika (0 = brak) */
    uint64_t journal_blocks;  /* rozmiar dziennika w blokach (0 = brak) */
    uint32_t flags;             /* bit GH_SB_ENCRYPTED */
    uint32_t enc_kdf_iters;
    uint8_t  enc_salt[16];
    uint8_t  enc_verifier[32];
    uint64_t csum_start;        /* pierwszy blok regionu sum (0 = brak) */
    uint64_t csum_blocks;       /* rozmiar regionu sum w blokach (0 = brak) */
    uint32_t sb_csum;           /* CRC32 superbloku (liczony z sb_csum=0) */
};

struct gh_inode {                 /* dokładnie GH_INODE_SIZE bajtów */
    uint16_t type;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t atime, mtime, ctime;
    uint64_t direct[GH_NDIRECT];
    uint64_t indirect;
    uint64_t double_indirect;
    uint64_t xattr_block;         /* 0 = brak xattr */
    uint8_t  pad[GH_INODE_SIZE - (2+2+4+4+4+8+8+8+8 + 8*GH_NDIRECT + 8 + 8 + 8)];
};

struct gh_dirent {                /* rekord stałej długości w pliku katalogu */
    uint64_t ino;                 /* 0 = wpis wolny/usunięty */
    uint16_t name_len;
    char     name[GH_NAME_MAX + 1];
};

struct gh_statfs {
    uint32_t block_size;
    uint64_t total_blocks, free_blocks;
    uint64_t total_inodes, free_inodes;
    uint32_t name_max;
};

_Static_assert(sizeof(struct gh_inode) == GH_INODE_SIZE, "i-wezel != 256B");

#define GH_JMAGIC "GHJRNL\0\1"   /* 8 bajtów */

#define GH_DIRHDR_MAGIC 0x4753484448495231ULL   /* "GSHDHIR1" */
struct gh_dirhdr {            /* w slocie 0 pliku katalogu, dopelniony do rozmiaru dirent */
    uint64_t magic;
    uint32_t used;            /* zajete + tombstony */
    uint32_t nslots;
    uint32_t live;            /* zajete (bez tombstonow) */
};

struct gh_jheader {              /* pierwszy blok regionu dziennika */
    uint8_t  magic[8];
    uint64_t seq;
    uint32_t committed;          /* 1 = transakcja kompletna na dysku */
    uint32_t n_blocks;           /* liczba obrazów bloków */
    uint64_t descriptor_blocks;  /* ile bloków deskryptora */
    uint32_t csum;               /* CRC nad deskryptorem + obrazami (rozdarty zapis) */
};

#endif
