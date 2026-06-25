#ifndef GH2_FORMAT_H
#define GH2_FORMAT_H
/* ghostfs v2 — format on-disk (CoW B-tree). Magic "GHOSTFS\x02" (osobny od v1). */
#include <stdint.h>
#include <assert.h>

#define GH2_MAGIC      "GHOSTFS\x02"   /* 8 bajtow */
#define GH2_MAGIC_LEN  8
#define GH2_BLOCK_SIZE 4096

/* sloty superbloku (ping-pong): bloki 0 i 1. Region danych od bloku 2. */
#define GH2_SB_SLOT_A    0
#define GH2_SB_SLOT_B    1
#define GH2_DATA_START   2

/* flagi superbloku */
#define GH2_SB_ENCRYPTED 0x1u
#define GH2_SB_DUP_META  0x2u
#define GH2_SB_COMPRESS  0x4u

/* v2.9: chunk extent (typ itemu B-drzewa) i rozmiar chunku w blokach.
 * Klucz (ino, GH2_EXTENT_COMP, chunk_aligned_off); wartosc = gh2_cext_hdr + uint64_t blocks[].
 * Chunk = GH2_COMP_CHUNK blokow logicznych (8*4096 = 32 KB). */
#define GH2_EXTENT_COMP  6
#define GH2_COMP_CHUNK   8u

/* wskaznik bloku: z duplikatem (samonaprawa) i suma (integralnosc). */
struct gh2_bptr {
    uint64_t block;       /* numer bloku fizycznego (0 = brak) */
    uint64_t dup_block;   /* kopia (0 = brak) — DUP metadane */
    uint32_t csum;        /* crc32 wskazywanego bloku (plaintext) */
    uint32_t _pad;
    uint64_t generation;  /* generacja, w ktorej zapisano */
};

/* superblok (jeden slot); mieści się w 1 bloku 4096 B. */
struct gh2_superblock {
    char     magic[GH2_MAGIC_LEN];
    uint64_t generation;        /* rosnie co commit */
    uint64_t total_blocks;
    uint32_t block_size;
    uint32_t flags;
    struct gh2_bptr root_tree;  /* korzeń drzewa korzeni (v2.1+); v2.0: zerowy stub */
    uint64_t next_free;         /* bump alokator (stub v2.0; prawdziwy v2.2) */
    uint64_t root_dir_ino;      /* rezerwa */
    uint8_t  uuid[16];
    uint8_t  enc_salt[16];      /* reuzycie krypto v1 */
    uint8_t  enc_verifier[32];
    uint64_t reserved[16];
    uint32_t sb_csum;           /* crc32 wszystkiego PRZED tym polem; musi byc ostatnie */
};

_Static_assert(sizeof(struct gh2_superblock) <= GH2_BLOCK_SIZE, "superblok v2 > blok");
_Static_assert(GH2_MAGIC_LEN == 8, "magic v2 musi miec 8 bajtow");

#endif
