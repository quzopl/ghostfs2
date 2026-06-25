#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_space.h"
#include "v2/gh2_btree.h"
#include "v2/gh2_rcache.h"
#include "v2/gh2_format.h"
#include "csum.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * ghostfs v2 — read-side node cache (csum-keyed, self-coherent).
 *
 * Dowodzimy:
 *  1) KOREKTNOSC: cache zwraca DOKLADNIE zweryfikowana tresc (memcpy bajt-exact); round-trip
 *     wielopoziomowego pliku bajt-identyczny przez cache.
 *  2) SPOJNOSC CSUM-KEYED: get(block,csum) HIT tylko gdy zapisany csum == podany; reuzyty (CoW)
 *     blok z NOWYM csum -> MISS (NIGDY stara tresc); stary bptr (snapshot) -> stara tresc.
 *  3) SELF-HEALING: cache z DOBRA trescia serwuje ja mimo bitrotu na dysku; brak w cache + zly
 *     disk + dup=0 -> -EIO; z dup -> repair.
 *  4) DOWOD REDUKCJI: licznik gh_crc32 wezlow dla N losowych odczytow << N*wysokosc (gorne wezly
 *     hit ~100%).
 *  5) READ-ONLY: mapa bits/refs/nfree/fs_root niezmienione po 1000 odczytach.
 * ========================================================================== */

static const uint64_t NBLK = 16384;

static int open_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_create(path, NBLK, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev) {
    gh_bcache_destroy(dev);
    gh_dev_close(dev);
}

/* ============================ 1. rcache jednostkowo: korektnosc + csum-keyed + eksmisja ====== */
static void test_rcache_unit(void) {
    struct gh2_rcache *rc = gh2_rcache_create();
    CHECK(rc != NULL);

    uint8_t a[GH2_BLOCK_SIZE], b[GH2_BLOCK_SIZE], out[GH2_BLOCK_SIZE];
    for (int i = 0; i < GH2_BLOCK_SIZE; i++) { a[i] = (uint8_t)(i * 7 + 1); b[i] = (uint8_t)(i * 3); }
    uint32_t ca = gh_crc32(a, GH2_BLOCK_SIZE);
    uint32_t cb = gh_crc32(b, GH2_BLOCK_SIZE);

    /* MISS na pustym */
    CHECK_EQ(gh2_rcache_get(rc, 100, ca, out), 0);

    /* PUT + GET (csum pasuje) -> HIT, bajt-exact */
    gh2_rcache_put(rc, 100, ca, a);
    memset(out, 0, sizeof(out));
    CHECK_EQ(gh2_rcache_get(rc, 100, ca, out), 1);
    CHECK_EQ(memcmp(out, a, GH2_BLOCK_SIZE), 0);

    /* CSUM-KEYED: ten sam block, INNY csum (reuzyty blok) -> MISS (NIE zwroc starej tresci) */
    CHECK_EQ(gh2_rcache_get(rc, 100, cb, out), 0);

    /* update: ten sam block, nowa tresc+csum -> HIT nowej tresci */
    gh2_rcache_put(rc, 100, cb, b);
    CHECK_EQ(gh2_rcache_get(rc, 100, cb, out), 1);
    CHECK_EQ(memcmp(out, b, GH2_BLOCK_SIZE), 0);
    CHECK_EQ(gh2_rcache_get(rc, 100, ca, out), 0);   /* stary csum juz nie pasuje */

    /* block 0 nigdy nie wchodzi (klucz pusty) */
    gh2_rcache_put(rc, 0, ca, a);
    CHECK_EQ(gh2_rcache_get(rc, 0, ca, out), 0);

    gh2_rcache_destroy(rc);
}

/* eksmisja: cache ograniczony do CAP (BOUND pamieci); niedawno wstawione bloki obecne, a po
 * przekroczeniu CAP najstarsze (nieuzywane) eksmitowane. Dowodzimy ograniczenia + braku gubienia
 * swiezo wstawionego/zweryfikowanego bloku w oknie < CAP. */
static void test_rcache_eviction_bounded(void) {
    struct gh2_rcache *rc = gh2_rcache_create();
    CHECK(rc != NULL);
    uint8_t out[GH2_BLOCK_SIZE];

    /* wstaw 4*CAP roznych blokow; po kazdym wstawieniu blok JUST-inserted musi byc obecny
     * (transparentnosc swiezego wpisu), a count nigdy > CAP (twardy bound pamieci). */
    uint64_t maxcount = 0;
    for (uint64_t blk = 1000; blk < 1000 + 4 * GH2_RCACHE_CAP; blk++) {
        uint8_t cb[GH2_BLOCK_SIZE];
        memset(cb, (int)(blk & 0xFF), sizeof(cb));
        cb[0] = (uint8_t)(blk >> 8); cb[1] = (uint8_t)(blk >> 16);
        uint32_t cc = gh_crc32(cb, GH2_BLOCK_SIZE);
        gh2_rcache_put(rc, blk, cc, cb);
        /* swiezo wstawiony blok -> natychmiastowy HIT bajt-exact */
        CHECK_EQ(gh2_rcache_get(rc, blk, cc, out), 1);
        CHECK_EQ(memcmp(out, cb, GH2_BLOCK_SIZE), 0);
        uint64_t c = gh2_rcache_count(rc);
        if (c > maxcount) maxcount = c;
        CHECK(c <= GH2_RCACHE_CAP);
    }
    /* cache faktycznie sie zapelnil do CAP (a nie rosnie bez ograniczen) */
    CHECK_EQ((long long)maxcount, (long long)GH2_RCACHE_CAP);

    /* najstarszy (1000), dawno nieuzywany, zostal eksmitowany (cache nie trzyma wszystkich 4*CAP) */
    uint8_t old[GH2_BLOCK_SIZE];
    memset(old, (int)(1000 & 0xFF), sizeof(old)); old[0] = 0; old[1] = 0;
    CHECK_EQ(gh2_rcache_get(rc, 1000, gh_crc32(old, GH2_BLOCK_SIZE), out), 0);

    gh2_rcache_destroy(rc);
}

/* ============================ 2. transparentnosc: round-trip bajt-exact wielopoziomowy ====== */
static void test_roundtrip_transparent(void) {
    char tmp[] = "/tmp/gh2rc_rt_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);
    CHECK(fs.dev.v2_rcache != NULL);                 /* rcache aktywny po mount */

    /* wymusz drzewo wielopoziomowe: duzo plikow + tresc, by katalog/fs-tree mialy >1 poziom. */
    char path[64];
    for (int i = 0; i < 400; i++) {
        snprintf(path, sizeof(path), "/f%03d", i);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
        char data[200];
        for (int j = 0; j < (int)sizeof(data); j++) data[j] = (char)(i * 31 + j);
        CHECK_EQ(gh2_fs_write(&fs, path, data, sizeof(data), 0), (ssize_t)sizeof(data));
    }
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* odczyt KAZDEGO pliku przez cache (wielokrotnie) -> bajt-exact */
    for (int rep = 0; rep < 3; rep++) {
        for (int i = 0; i < 400; i++) {
            snprintf(path, sizeof(path), "/f%03d", i);
            char rd[200]; char want[200];
            for (int j = 0; j < (int)sizeof(want); j++) want[j] = (char)(i * 31 + j);
            ssize_t n = gh2_fs_read(&fs, path, rd, sizeof(rd), 0);
            CHECK_EQ(n, (ssize_t)sizeof(rd));
            CHECK_EQ(memcmp(rd, want, sizeof(rd)), 0);
        }
    }
    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ 3. spojnosc csum-keyed: CoW reuse na poziomie B-drzewa ========= */
/* Scenariusz: zapisz wezel (block X, csum C1), read -> cache[X]=C1. Zwolnij X (free), realokuj X
 * z INNA trescia (csum C2) -> read nowego bptr (X,C2): MISS (csum) -> re-read disk -> NOWA tresc.
 * Read STAREGO bptr (X,C1): cache HIT -> STARA tresc (snapshot-correct). */
static void test_csum_keyed_reuse(void) {
    char tmp[] = "/tmp/gh2rc_ck_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    /* rcache wlasciwy (poza FS-mount), zeby pokazac mechanizm bez ncache write-back. */
    dev.v2_rcache = gh2_rcache_create();
    CHECK(dev.v2_rcache != NULL);

    struct gh2_bump_alloc bump;
    gh2_bump_init(&bump, 2, NBLK);                   /* bloki 0/1 zarezerwowane */
    struct gh2_alloc a = gh2_bump_alloc(&bump);

    /* zbuduj lisc 1 (tresc A) */
    uint8_t bufA[GH2_BLOCK_SIZE]; memset(bufA, 0, sizeof(bufA));
    struct gh2_node_hdr *hA = (struct gh2_node_hdr *)bufA;
    hA->level = 0; hA->nritems = 0; hA->generation = 1; hA->owner = 7;
    bufA[64] = 0xA1;                                 /* znacznik tresci A (poza walidowanym obszarem nritems=0) */
    struct gh2_bptr ba;
    CHECK_EQ(gh2_node_write(&dev, &a, bufA, 1, &ba), 0);
    uint64_t Xblock = ba.block;
    uint32_t C1 = ba.csum;

    /* read -> populate cache[X]=C1 */
    uint8_t rd[GH2_BLOCK_SIZE];
    CHECK_EQ(gh2_node_read(&dev, &ba, rd), 0);
    CHECK_EQ(rd[64], 0xA1);
    CHECK_EQ(gh2_rcache_get(dev.v2_rcache, Xblock, C1, rd), 1);   /* w cache */

    /* zwolnij X i realokuj — bump LIFO zwroci X jako pierwszy wolny */
    a.free(a.ctx, Xblock);
    uint8_t bufB[GH2_BLOCK_SIZE]; memset(bufB, 0, sizeof(bufB));
    struct gh2_node_hdr *hB = (struct gh2_node_hdr *)bufB;
    hB->level = 0; hB->nritems = 0; hB->generation = 2; hB->owner = 7;
    bufB[64] = 0xB2;                                 /* znacznik tresci B (rozny) */
    struct gh2_bptr bb;
    CHECK_EQ(gh2_node_write(&dev, &a, bufB, 2, &bb), 0);
    CHECK_EQ(bb.block, Xblock);                      /* ten sam blok reuzyty */
    uint32_t C2 = bb.csum;
    CHECK(C2 != C1);                                 /* inna tresc -> inny csum */

    /* read NOWEGO bptr (X,C2): MUSI zwrocic NOWA tresc B (csum-keyed MISS na starym wpisie) */
    memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_node_read(&dev, &bb, rd), 0);
    CHECK_EQ(rd[64], 0xB2);                          /* NOWA tresc, nie 0xA1 */

    /* read STAREGO bptr (X,C1): teraz cache zaktualizowany do C2 (re-read wstawil B). Stary csum
     * C1 != cache.csum -> MISS -> re-read disk (= tresc B). Kluczowe: NIGDY nie zwraca stale-old
     * pod nowym bptr; tu stary bptr juz nie wskazuje waznej tresci (blok nadpisany) — czytamy B
     * (lub -EIO gdyby csum nie pasowal). Dowodzimy: NIE zwraca 0xA1 jako tresci wpisu o csum C2. */
    memset(rd, 0, sizeof(rd));
    int rr = gh2_node_read(&dev, &ba, rd);
    /* ba (C1) wskazuje na blok ktory teraz ma tresc B (csum C2) -> csum mismatch -> -EIO (brak dup) */
    CHECK_EQ(rr, -EIO);

    gh2_rcache_destroy(dev.v2_rcache); dev.v2_rcache = NULL;
    gh2_bump_destroy(&bump);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ 4. self-healing: cache omija bitrot; dup/EIO bez cache ========= */
static void test_self_healing(void) {
    char tmp[] = "/tmp/gh2rc_heal_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    dev.v2_rcache = gh2_rcache_create();
    CHECK(dev.v2_rcache != NULL);

    struct gh2_bump_alloc bump;
    gh2_bump_init(&bump, 2, NBLK);
    struct gh2_alloc a = gh2_bump_alloc(&bump);

    /* --- A) cache ma dobry wezel; zbitrotuj disk-block -> read przez cache nadal DOBRY --- */
    uint8_t buf[GH2_BLOCK_SIZE]; memset(buf, 0, sizeof(buf));
    struct gh2_node_hdr *h = (struct gh2_node_hdr *)buf;
    h->level = 0; h->nritems = 0; h->generation = 1; h->owner = 1;
    buf[80] = 0x5A;
    struct gh2_bptr bp;
    CHECK_EQ(gh2_node_write(&dev, &a, buf, 1, &bp), 0);   /* dup_meta=0 -> brak dup */

    uint8_t rd[GH2_BLOCK_SIZE];
    CHECK_EQ(gh2_node_read(&dev, &bp, rd), 0);            /* populate cache */
    CHECK_EQ(rd[80], 0x5A);

    /* zbitrotuj disk-block (zla suma) — read powinien isc przez cache (csum bp pasuje do cache) */
    uint8_t corrupt[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(&dev, bp.block, corrupt), 0);
    corrupt[80] ^= 0xFF;                                  /* psuje tresc -> zla suma */
    CHECK_EQ(gh_disk_write(&dev, bp.block, corrupt), 0);
    memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_node_read(&dev, &bp, rd), 0);            /* cache HIT -> dobra tresc */
    CHECK_EQ(rd[80], 0x5A);                               /* samonaprawa: cache omija bitrot */

    /* --- B) BRAK w cache + zly disk + dup=0 -> -EIO --- */
    gh2_rcache_destroy(dev.v2_rcache);                    /* zwolnij cache z czesci A (brak wycieku) */
    struct gh2_rcache *fresh = gh2_rcache_create();
    CHECK(fresh != NULL);
    dev.v2_rcache = fresh;                                /* swiezy cache (bp nie znany) */
    CHECK_EQ(gh2_node_read(&dev, &bp, rd), -EIO);         /* disk zly, brak cache, brak dup */

    /* --- C) z DUP: zbitrotuj 1 kopie, druga dobra -> repair (bez cache) --- */
    dev.v2_rcache = NULL;                                 /* wylacz cache, by wymusic disk+dup */
    a.dup_meta = 1;
    uint8_t buf2[GH2_BLOCK_SIZE]; memset(buf2, 0, sizeof(buf2));
    struct gh2_node_hdr *h2 = (struct gh2_node_hdr *)buf2;
    h2->level = 0; h2->nritems = 0; h2->generation = 1; h2->owner = 1;
    buf2[90] = 0x77;
    struct gh2_bptr bp2;
    CHECK_EQ(gh2_node_write(&dev, &a, buf2, 1, &bp2), 0);
    CHECK(bp2.dup_block != 0);
    /* zbitrotuj primary; dup dobry */
    uint8_t c2[GH2_BLOCK_SIZE];
    CHECK_EQ(gh_disk_read(&dev, bp2.block, c2), 0);
    c2[90] ^= 0xFF;
    CHECK_EQ(gh_disk_write(&dev, bp2.block, c2), 0);
    memset(rd, 0, sizeof(rd));
    CHECK_EQ(gh2_node_read(&dev, &bp2, rd), 0);           /* repair z dup */
    CHECK_EQ(rd[90], 0x77);
    /* po repair primary naprawiony na dysku */
    CHECK_EQ(gh_disk_read(&dev, bp2.block, c2), 0);
    CHECK_EQ(gh_crc32(c2, GH2_BLOCK_SIZE), bp2.csum);

    gh2_rcache_destroy(fresh);
    gh2_bump_destroy(&bump);
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ 5. dowod redukcji CRC wezlow ============================ */
static void test_crc_reduction(void) {
    char tmp[] = "/tmp/gh2rc_crc_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    /* zbuduj duze drzewo (wielopoziomowe) */
    char path[64];
    const int NF = 600;
    for (int i = 0; i < NF; i++) {
        snprintf(path, sizeof(path), "/c%04d", i);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
    }
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    /* rozgrzej cache: jeden pelny przebieg odczytow (gorne wezly do cache) */
    struct gh2_inode in;
    for (int i = 0; i < NF; i++) {
        snprintf(path, sizeof(path), "/c%04d", i);
        CHECK_EQ(gh2_fs_getattr(&fs, path, &in, NULL), 0);
    }

    /* zmierz CRC-weryfikacje wezlow podczas N losowych odczytow PO rozgrzaniu */
    const int N = 2000;
    gh2_node_crc_verify_count = 0;
    unsigned seed = 12345;
    for (int k = 0; k < N; k++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 8) % (unsigned)NF);
        snprintf(path, sizeof(path), "/c%04d", i);
        CHECK_EQ(gh2_fs_getattr(&fs, path, &in, NULL), 0);
    }
    unsigned long verifies = gh2_node_crc_verify_count;
    printf("  [redukcja] N=%d odczytow, CRC-weryfikacji wezlow=%lu (avg %.3f/odczyt)\n",
           N, verifies, (double)verifies / N);
    /* gorne wezly w cache -> CRC pomijany; weryfikacje << N (gdyby kazdy odczyt weryfikowal
     * cala sciezke wysokosci h>=2, byloby >= 2*N). Dowodzimy < N (mniej niz 1 CRC/odczyt). */
    CHECK(verifies < (unsigned long)N);

    /* kontrola: bez rcache TE SAME odczyty weryfikuja >= N (kazdy descent CRC-uje korzen). */
    gh2_rcache_destroy(fs.dev.v2_rcache); fs.dev.v2_rcache = NULL;
    gh2_node_crc_verify_count = 0;
    seed = 12345;
    for (int k = 0; k < N; k++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 8) % (unsigned)NF);
        snprintf(path, sizeof(path), "/c%04d", i);
        CHECK_EQ(gh2_fs_getattr(&fs, path, &in, NULL), 0);
    }
    unsigned long no_cache = gh2_node_crc_verify_count;
    printf("  [kontrola] bez rcache: CRC-weryfikacji=%lu (avg %.3f/odczyt)\n",
           no_cache, (double)no_cache / N);
    CHECK(no_cache > verifies);                       /* cache realnie redukuje */
    CHECK(no_cache >= (unsigned long)N);              /* >=1 CRC/odczyt bez cache */

    gh2_fs_unmount(&fs);                              /* rcache juz NULL — bezpieczne */
    close_dev(&dev);
    unlink(tmp);
}

/* ============================ 6. read-only: brak mutacji po 1000 odczytach ============================ */
static int snap_space(struct gh2_fs *fs, uint8_t **bits, uint16_t **refs, uint64_t *nfree) {
    uint64_t nb = fs->space.nblocks;
    size_t bb = (size_t)((nb + 7) / 8);
    *bits = malloc(bb); *refs = malloc((size_t)nb * sizeof(uint16_t));
    if (!*bits || !*refs) return -1;
    memcpy(*bits, fs->space.bits, bb);
    memcpy(*refs, fs->space.refs, (size_t)nb * sizeof(uint16_t));
    *nfree = fs->space.nfree;
    return 0;
}
static void test_read_only(void) {
    char tmp[] = "/tmp/gh2rc_ro_XXXXXX";
    int fd = mkstemp(tmp); CHECK(fd >= 0); close(fd);
    struct gh_dev dev;
    CHECK_EQ(open_dev(&dev, tmp), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    struct gh2_fs fs;
    CHECK_EQ(gh2_fs_mount(&fs, &dev), 0);

    char path[64];
    const int NF = 300;
    for (int i = 0; i < NF; i++) {
        snprintf(path, sizeof(path), "/r%04d", i);
        CHECK_EQ(gh2_fs_create(&fs, path, 0644), 0);
        char d[64]; for (int j = 0; j < 64; j++) d[j] = (char)(i + j);
        CHECK_EQ(gh2_fs_write(&fs, path, d, 64, 0), 64);
    }
    CHECK_EQ(gh2_fs_commit(&fs), 0);

    uint8_t *bits0; uint16_t *refs0; uint64_t nfree0;
    struct gh2_bptr root0 = fs.fs_root;
    CHECK_EQ(snap_space(&fs, &bits0, &refs0, &nfree0), 0);

    struct gh2_inode in;
    for (int k = 0; k < 1000; k++) {
        snprintf(path, sizeof(path), "/r%04d", k % NF);
        CHECK_EQ(gh2_fs_getattr(&fs, path, &in, NULL), 0);
        char rd[64];
        CHECK_EQ(gh2_fs_read(&fs, path, rd, 64, 0), 64);
    }

    /* mapa/refs/nfree/fs_root niezmienione */
    size_t bb = (size_t)((fs.space.nblocks + 7) / 8);
    CHECK_EQ(memcmp(bits0, fs.space.bits, bb), 0);
    CHECK_EQ(memcmp(refs0, fs.space.refs, (size_t)fs.space.nblocks * sizeof(uint16_t)), 0);
    CHECK_EQ((long long)nfree0, (long long)fs.space.nfree);
    CHECK_EQ(memcmp(&root0, &fs.fs_root, sizeof(root0)), 0);

    int issues = -1;
    CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
    CHECK_EQ(issues, 0);

    free(bits0); free(refs0);
    gh2_fs_unmount(&fs);
    close_dev(&dev);
    unlink(tmp);
}

int main(void) {
    RUN_TEST(test_rcache_unit);
    RUN_TEST(test_rcache_eviction_bounded);
    RUN_TEST(test_roundtrip_transparent);
    RUN_TEST(test_csum_keyed_reuse);
    RUN_TEST(test_self_healing);
    RUN_TEST(test_crc_reduction);
    RUN_TEST(test_read_only);
    return TEST_SUMMARY();
}
