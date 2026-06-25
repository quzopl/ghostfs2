#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "v2/gh2_fs.h"
#include "v2/gh2_format.h"
#include "block.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ============================================================================
 * ghostfs v2enc — testy szyfrowania (reuzycie krypto v1):
 *  - format z kluczem -> mount kluczem -> round-trip pliki/katalogi/dane bajt-exact, fsck==0;
 *  - AT-REST: marker NIE wystepuje w surowych bajtach kontenera poza SB (bloki 0,1);
 *  - zly klucz -> -EACCES; brak klucza na zaszyfrowanym -> -EACCES;
 *  - --compress (zaszyfrowany skompresowany: round-trip + at-rest brak markera nawet sciśliwego);
 *  - snapshot (zaszyfrowany: izolacja CoW + round-trip);
 *  - persystencja remount; cipher wipe (po unmount dev->cipher==NULL; brak wycieku — ASan).
 * ========================================================================== */

static const uint64_t NBLK = 8192;
static const char *KEY  = "correct horse battery staple";
static const char *BADK = "wrong wrong wrong";

/* ---- dev helpers ---- */
static int open_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_create(path, NBLK, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static int reopen_dev(struct gh_dev *dev, const char *path) {
    int r = gh_dev_open(path, dev);
    if (r) return r;
    return gh_bcache_create(dev);
}
static void close_dev(struct gh_dev *dev) {
    gh_bcache_destroy(dev);
    gh_dev_close(dev);
}

/* format zaszyfrowany; wymaz cipher pozostawiony na dev (jak robi CLI). */
static int format_key(const char *path, uint32_t flags, const char *key) {
    struct gh_dev dev;
    int r = open_dev(&dev, path);
    if (r) return r;
    r = gh2_fs_format_key(&dev, NBLK, flags, key);
    if (dev.cipher) { gh_crypto_wipe(dev.cipher); free(dev.cipher); dev.cipher = NULL; }
    close_dev(&dev);
    return r;
}

/* ---- readdir liczace ---- */
struct rd_count { int n; int saw_dot, saw_dotdot, saw_a, saw_sub; };
static int rd_cb(const char *name, uint16_t name_len, uint64_t ino, uint8_t ftype, void *ctx) {
    (void)name_len; (void)ino; (void)ftype;
    struct rd_count *c = ctx;
    c->n++;
    if (!strcmp(name, ".")) c->saw_dot = 1;
    else if (!strcmp(name, "..")) c->saw_dotdot = 1;
    else if (!strcmp(name, "a.txt")) c->saw_a = 1;
    else if (!strcmp(name, "sub")) c->saw_sub = 1;
    return 0;
}

/* czy `needle` (len) wystepuje w surowych bajtach kontenera POZA blokami SB (0,1)? */
static int marker_at_rest(const char *path, const uint8_t *needle, size_t nlen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) { close(fd); return -1; }
    /* czytaj od bloku 2 (pomin oba sloty SB, ktore SA jawne — trzymaja sol+weryfikator) */
    off_t start = (off_t)GH2_DATA_START * GH2_BLOCK_SIZE;
    if (sz <= start) { close(fd); return 0; }
    size_t len = (size_t)(sz - start);
    uint8_t *buf = malloc(len);
    if (!buf) { close(fd); return -1; }
    ssize_t got = pread(fd, buf, len, start);
    close(fd);
    if (got != (ssize_t)len) { free(buf); return -1; }
    int found = 0;
    if (len >= nlen) {
        for (size_t i = 0; i + nlen <= len; i++) {
            if (memcmp(buf + i, needle, nlen) == 0) { found = 1; break; }
        }
    }
    free(buf);
    return found;   /* 1 = znaleziono plaintext (ZLE), 0 = brak (OK) */
}

/* ============================ round-trip + at-rest (bez kompresji) ============================ */
static void test_roundtrip_atrest(void) {
    const char *path = "/tmp/gh2enc_rt.img";
    unlink(path);
    CHECK_EQ(format_key(path, 0, KEY), 0);

    /* unikalny marker, ktorego szukamy w surowym kontenerze */
    static const char marker[] = "PLAINTEXT_MARKER_7f3a_do_not_leak_2026";
    char payload[4096];
    memset(payload, 0, sizeof(payload));
    /* powtorz marker tak, by trafil do bloku danych */
    for (size_t i = 0; i + sizeof(marker) <= sizeof(payload); i += sizeof(marker))
        memcpy(payload + i, marker, sizeof(marker) - 1);

    /* zapis przez zamontowany szyfrowany fs */
    struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, path), 0);
    struct gh2_fs fs;
    int r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    if (r == 0) {
        CHECK_EQ(gh2_fs_mkdir(&fs, "/sub", 0755), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/a.txt", 0644), 0);
        CHECK_EQ(gh2_fs_create(&fs, "/sub/b.bin", 0644), 0);
        ssize_t w = gh2_fs_write(&fs, "/a.txt", payload, sizeof(payload), 0);
        CHECK_EQ(w, (ssize_t)sizeof(payload));
        w = gh2_fs_write(&fs, "/sub/b.bin", payload, sizeof(payload), 0);
        CHECK_EQ(w, (ssize_t)sizeof(payload));
        CHECK_EQ(gh2_fs_commit(&fs), 0);

        int issues = -1;
        CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
        CHECK_EQ(issues, 0);
        gh2_fs_unmount(&fs);
        CHECK(fs.dev.cipher == NULL);   /* cipher wymazany */
    }
    close_dev(&dev);

    /* AT-REST: marker NIE moze byc widoczny w surowych bajtach (poza SB) */
    CHECK_EQ(marker_at_rest(path, (const uint8_t *)marker, sizeof(marker) - 1), 0);

    /* round-trip po remount: dane bajt-exact, drzewo katalogow widoczne */
    CHECK_EQ(reopen_dev(&dev, path), 0);
    r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    if (r == 0) {
        char got[4096];
        ssize_t rd = gh2_fs_read(&fs, "/a.txt", got, sizeof(got), 0);
        CHECK_EQ(rd, (ssize_t)sizeof(payload));
        CHECK_EQ(memcmp(got, payload, sizeof(payload)), 0);

        memset(got, 0, sizeof(got));
        rd = gh2_fs_read(&fs, "/sub/b.bin", got, sizeof(got), 0);
        CHECK_EQ(rd, (ssize_t)sizeof(payload));
        CHECK_EQ(memcmp(got, payload, sizeof(payload)), 0);

        struct rd_count c; memset(&c, 0, sizeof(c));
        CHECK_EQ(gh2_fs_readdir(&fs, "/", rd_cb, &c), 0);
        CHECK(c.saw_dot && c.saw_dotdot && c.saw_a && c.saw_sub);

        gh2_fs_unmount(&fs);
        CHECK(fs.dev.cipher == NULL);
    }
    close_dev(&dev);
    unlink(path);
}

/* ============================ zly / brak klucza -> -EACCES ============================ */
static void test_wrong_and_missing_key(void) {
    const char *path = "/tmp/gh2enc_acl.img";
    unlink(path);
    CHECK_EQ(format_key(path, 0, KEY), 0);

    /* brak klucza (wrapper bez hasla) -> -EACCES */
    struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, path), 0);
    struct gh2_fs fs;
    int r = gh2_fs_mount(&fs, &dev);
    CHECK_EQ(r, -EACCES);
    CHECK(fs.dev.cipher == NULL);   /* nieudany mount nie zostawia klucza */
    close_dev(&dev);

    /* zly klucz -> -EACCES */
    CHECK_EQ(reopen_dev(&dev, path), 0);
    r = gh2_fs_mount_key(&fs, &dev, BADK);
    CHECK_EQ(r, -EACCES);
    CHECK(fs.dev.cipher == NULL);
    close_dev(&dev);

    /* puste haslo -> -EACCES */
    CHECK_EQ(reopen_dev(&dev, path), 0);
    r = gh2_fs_mount_key(&fs, &dev, "");
    CHECK_EQ(r, -EACCES);
    close_dev(&dev);

    /* poprawny klucz -> sukces */
    CHECK_EQ(reopen_dev(&dev, path), 0);
    r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    if (r == 0) { gh2_fs_unmount(&fs); CHECK(fs.dev.cipher == NULL); }
    close_dev(&dev);
    unlink(path);
}

/* ============================ zaszyfrowany + --compress ============================ */
static void test_compress_atrest(void) {
    const char *path = "/tmp/gh2enc_comp.img";
    unlink(path);
    CHECK_EQ(format_key(path, GH2_SB_COMPRESS, KEY), 0);

    /* wysoce sciśliwy marker (kompresja przed szyfrowaniem; mimo to brak plaintextu) */
    static const char marker[] = "COMPRESSIBLE_MARKER_aaaa_5e1c_leak";
    size_t plen = 64 * 1024;   /* >1 chunk, sciśliwe */
    char *payload = malloc(plen);
    CHECK(payload != NULL);
    if (!payload) return;
    for (size_t i = 0; i + sizeof(marker) <= plen; i += sizeof(marker))
        memcpy(payload + i, marker, sizeof(marker) - 1);
    /* dopelnij ogon zerami (sciśliwe) */
    size_t used = (plen / sizeof(marker)) * sizeof(marker);
    memset(payload + used, 0, plen - used);

    struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, path), 0);
    struct gh2_fs fs;
    int r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    if (r == 0) {
        CHECK_EQ(gh2_fs_create(&fs, "/c.dat", 0644), 0);
        ssize_t w = gh2_fs_write(&fs, "/c.dat", payload, plen, 0);
        CHECK_EQ(w, (ssize_t)plen);
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        int issues = -1;
        CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
        CHECK_EQ(issues, 0);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);

    /* at-rest: nawet sciśliwy marker nie moze przeciec (kompresja -> szyfrowanie) */
    CHECK_EQ(marker_at_rest(path, (const uint8_t *)marker, sizeof(marker) - 1), 0);

    /* round-trip */
    CHECK_EQ(reopen_dev(&dev, path), 0);
    r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    if (r == 0) {
        char *got = malloc(plen);
        CHECK(got != NULL);
        if (got) {
            ssize_t rd = gh2_fs_read(&fs, "/c.dat", got, plen, 0);
            CHECK_EQ(rd, (ssize_t)plen);
            CHECK_EQ(memcmp(got, payload, plen), 0);
            free(got);
        }
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);
    free(payload);
    unlink(path);
}

/* ============================ zaszyfrowany + snapshot (izolacja CoW) ============================ */
static void test_snapshot_isolation(void) {
    const char *path = "/tmp/gh2enc_snap.img";
    unlink(path);
    CHECK_EQ(format_key(path, 0, KEY), 0);

    const char v1data[] = "ORIGINAL-v1-contents-aaaa";
    const char v2data[] = "MODIFIED-v2-contents-bbbb";

    struct gh_dev dev; CHECK_EQ(reopen_dev(&dev, path), 0);
    struct gh2_fs fs;
    int r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    uint64_t snap_id = 0;
    if (r == 0) {
        CHECK_EQ(gh2_fs_create(&fs, "/f.txt", 0644), 0);
        CHECK_EQ(gh2_fs_write(&fs, "/f.txt", v1data, sizeof(v1data), 0), (ssize_t)sizeof(v1data));
        CHECK_EQ(gh2_fs_commit(&fs), 0);

        /* snapshot widoku v1 */
        CHECK_EQ(gh2_fs_snapshot(&fs, "snap1"), 0);

        /* znajdz id snapshotu (max != default) */
        struct gh2_subvol_item sv; (void)sv;
        /* zamiast iterowac, snapshot ma id = max+1 = 2 (jedyny obok default 1) */
        snap_id = 2;

        /* nadpisz oryginal (CoW) */
        CHECK_EQ(gh2_fs_write(&fs, "/f.txt", v2data, sizeof(v2data), 0), (ssize_t)sizeof(v2data));
        CHECK_EQ(gh2_fs_commit(&fs), 0);

        /* aktywny widzi v2 */
        char got[64];
        ssize_t rd = gh2_fs_read(&fs, "/f.txt", got, sizeof(got), 0);
        CHECK_EQ(rd, (ssize_t)sizeof(v2data));
        CHECK_EQ(memcmp(got, v2data, sizeof(v2data)), 0);

        /* snapshot wciaz widzi v1 (izolacja, deszyfrowanie dziala) */
        memset(got, 0, sizeof(got));
        rd = gh2_fs_read_subvol(&fs, snap_id, "/f.txt", got, sizeof(got), 0);
        CHECK_EQ(rd, (ssize_t)sizeof(v1data));
        CHECK_EQ(memcmp(got, v1data, sizeof(v1data)), 0);

        int issues = -1;
        CHECK_EQ(gh2_fsck(&fs, 0, &issues), 0);
        CHECK_EQ(issues, 0);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);

    /* remont: snapshot wciaz izolowany po deszyfrowaniu z dysku */
    CHECK_EQ(reopen_dev(&dev, path), 0);
    r = gh2_fs_mount_key(&fs, &dev, KEY);
    CHECK_EQ(r, 0);
    if (r == 0) {
        char got[64];
        ssize_t rd = gh2_fs_read_subvol(&fs, snap_id, "/f.txt", got, sizeof(got), 0);
        CHECK_EQ(rd, (ssize_t)sizeof(v1data));
        CHECK_EQ(memcmp(got, v1data, sizeof(v1data)), 0);
        gh2_fs_unmount(&fs);
    }
    close_dev(&dev);
    unlink(path);
}

/* ============================ regresja: nieszyfrowany v2 nietkniety ============================ */
static void test_unencrypted_still_works(void) {
    const char *path = "/tmp/gh2enc_plain.img";
    unlink(path);
    /* format bez klucza (wrapper) */
    struct gh_dev dev; CHECK_EQ(open_dev(&dev, path), 0);
    CHECK_EQ(gh2_fs_format(&dev, NBLK, 0), 0);
    CHECK(dev.cipher == NULL);   /* bez hasla -> brak ciphera */
    close_dev(&dev);

    const char data[] = "plain-data-zzzz";
    CHECK_EQ(reopen_dev(&dev, path), 0);
    struct gh2_fs fs;
    /* mount bez klucza dziala (nieszyfrowany) */
    int r = gh2_fs_mount(&fs, &dev);
    CHECK_EQ(r, 0);
    if (r == 0) {
        CHECK(fs.dev.cipher == NULL);
        CHECK_EQ(gh2_fs_create(&fs, "/p.txt", 0644), 0);
        CHECK_EQ(gh2_fs_write(&fs, "/p.txt", data, sizeof(data), 0), (ssize_t)sizeof(data));
        CHECK_EQ(gh2_fs_commit(&fs), 0);
        char got[32];
        ssize_t rd = gh2_fs_read(&fs, "/p.txt", got, sizeof(got), 0);
        CHECK_EQ(rd, (ssize_t)sizeof(data));
        CHECK_EQ(memcmp(got, data, sizeof(data)), 0);
        gh2_fs_unmount(&fs);
        CHECK(fs.dev.cipher == NULL);
    }
    close_dev(&dev);

    /* nieszyfrowany: marker WIDOCZNY at-rest (potwierdza, ze test markera dziala) */
    CHECK_EQ(marker_at_rest(path, (const uint8_t *)"plain-data-zzzz", 15), 1);
    unlink(path);
}

int main(void) {
    RUN_TEST(test_roundtrip_atrest);
    RUN_TEST(test_wrong_and_missing_key);
    RUN_TEST(test_compress_atrest);
    RUN_TEST(test_snapshot_isolation);
    RUN_TEST(test_unencrypted_still_works);
    return TEST_SUMMARY();
}
