# ghostfs pod-projekt C — szyfrowanie at-rest: plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> Subagenci dozwoleni; sekwencyjnie (wspólne pliki block.h/.c, super.c, fs.c, Makefile). Bramka: zielone testy + ASan.

**Goal:** Realne szyfrowanie at-rest (AES-256-XTS, PBKDF2-HMAC-SHA256) całej zawartości kontenera poza superblokiem, przezroczyste dla rdzenia i dziennika. Zgodnie ze spec `docs/superpowers/specs/2026-06-23-ghostfs-C-encryption-design.md`.

**Architecture:** Szyfrowanie wpięte w fizyczne I/O dysku (`gh_disk_read/write` w block.c), używane też przez journal.c. Na dysku zawsze szyfrogram, w pamięci jawny tekst. Klucz z hasła (`GHOSTFS_KEY`) przez PBKDF2, weryfikowany przy montowaniu. Superblok (blok 0) jawny. Kompatybilność wstecz: `flags==0` = tryb jawny.

**Tech Stack:** C11, OpenSSL libcrypto (`-lcrypto`), mini-harness, ASan, libfuse3.

## Global Constraints
- Testy przez `make`; gołe `cc` wymagają `-D_POSIX_C_SOURCE=200809L` ORAZ `-lcrypto`.
- Każde zadanie: zielone testy + ASan + commit.
- Superblok pozostaje w 4096 B (nowe pola z rezerwy).

## File Structure
| Plik | Zmiana | Odpowiedzialność |
|---|---|---|
| `src/crypto.h/.c` | nowe | OpenSSL: derive/verifier/random/encrypt/decrypt block |
| `src/ghostfs.h` | mod | pola superbloku flags/enc_*; `GH_SB_ENCRYPTED`; `struct gh_cipher` fwd nie tu |
| `src/block.h/.c` | mod | `gh_dev.cipher`; `gh_disk_read/write`; routing block_read/write |
| `src/journal.c` | mod | raw I/O przez `gh_disk_read/write` |
| `src/super.c/.h` | mod | `gh_format_enc`; `gh_format` = enc(NULL) |
| `src/fs.c/.h` | mod | `gh_fs_mount_key`; `gh_fs_mount` = key(getenv); unmount zwalnia cipher |
| `src/cli.c`, `src/fuse_main.c` | mod | hasło z `GHOSTFS_KEY` |
| `Makefile` | mod | crypto.c do CORE; `-lcrypto` do linkowania |
| `tests/test_crypto.c` | nowe | testy prymitywów |
| `tests/test_enc.c` | nowe | testy szyfrowanego FS (round-trip, at-rest, EACCES) |
| `tests/integration.sh` | mod | wariant zaszyfrowany |

---

## Task 1: Moduł kryptograficzny (OpenSSL)

**Files:** Create: `src/crypto.h`, `src/crypto.c`, `tests/test_crypto.c`; Modify: `Makefile`

- [ ] **Step 1: Utwórz `src/crypto.h`**

```c
#ifndef GH_CRYPTO_H
#define GH_CRYPTO_H
#include "ghostfs.h"
#include <stdint.h>
#include <stddef.h>

struct gh_cipher { uint8_t key[64]; };   /* klucz AES-256-XTS (2x256-bit) */

int  gh_crypto_derive(const char *passphrase, const uint8_t salt[16],
                      uint32_t iters, uint8_t key[64]);
void gh_crypto_verifier(const uint8_t key[64], const uint8_t salt[16], uint8_t out[32]);
int  gh_crypto_random(uint8_t *buf, size_t n);
int  gh_crypto_encrypt_block(const struct gh_cipher*, uint64_t blkno,
                             const uint8_t *in, uint8_t *out);
int  gh_crypto_decrypt_block(const struct gh_cipher*, uint64_t blkno,
                             const uint8_t *in, uint8_t *out);
#endif
```

- [ ] **Step 2: Napisz failujący test `tests/test_crypto.c`**

```c
#include "test.h"
#include "../src/crypto.h"
#include "../src/ghostfs.h"
#include <string.h>

static void test_derive_deterministic(void) {
    uint8_t salt[16]; memset(salt, 7, sizeof(salt));
    uint8_t k1[64], k2[64], k3[64];
    CHECK_EQ(gh_crypto_derive("haslo", salt, 1000, k1), 0);
    CHECK_EQ(gh_crypto_derive("haslo", salt, 1000, k2), 0);
    CHECK_EQ(memcmp(k1, k2, 64), 0);               /* deterministyczny */
    CHECK_EQ(gh_crypto_derive("inne", salt, 1000, k3), 0);
    CHECK(memcmp(k1, k3, 64) != 0);                /* inne haslo -> inny klucz */
}

static void test_verifier(void) {
    uint8_t salt[16]; memset(salt, 3, sizeof(salt));
    uint8_t key[64]; gh_crypto_derive("pw", salt, 1000, key);
    uint8_t v1[32], v2[32];
    gh_crypto_verifier(key, salt, v1);
    gh_crypto_verifier(key, salt, v2);
    CHECK_EQ(memcmp(v1, v2, 32), 0);
    uint8_t bad[64]; gh_crypto_derive("zle", salt, 1000, bad);
    uint8_t vb[32]; gh_crypto_verifier(bad, salt, vb);
    CHECK(memcmp(v1, vb, 32) != 0);
}

static void test_block_roundtrip(void) {
    struct gh_cipher c; memset(&c, 0, sizeof(c));
    uint8_t salt[16]; memset(salt, 1, sizeof(salt));
    gh_crypto_derive("pw", salt, 1000, c.key);

    uint8_t plain[GH_BLOCK_SIZE]; for (int i = 0; i < GH_BLOCK_SIZE; i++) plain[i] = (uint8_t)(i*3+1);
    uint8_t enc[GH_BLOCK_SIZE], dec[GH_BLOCK_SIZE];
    CHECK_EQ(gh_crypto_encrypt_block(&c, 42, plain, enc), 0);
    CHECK(memcmp(enc, plain, GH_BLOCK_SIZE) != 0);          /* faktycznie zaszyfrowane */
    CHECK_EQ(gh_crypto_decrypt_block(&c, 42, enc, dec), 0);
    CHECK_EQ(memcmp(dec, plain, GH_BLOCK_SIZE), 0);         /* round-trip */

    /* ten sam jawny pod innym blkno -> inny szyfrogram (tweak) */
    uint8_t enc2[GH_BLOCK_SIZE];
    CHECK_EQ(gh_crypto_encrypt_block(&c, 43, plain, enc2), 0);
    CHECK(memcmp(enc, enc2, GH_BLOCK_SIZE) != 0);
}

static void test_random(void) {
    uint8_t a[16], b[16]; memset(a, 0, 16); memset(b, 0, 16);
    CHECK_EQ(gh_crypto_random(a, 16), 0);
    CHECK_EQ(gh_crypto_random(b, 16), 0);
    CHECK(memcmp(a, b, 16) != 0);   /* skrajnie nieprawdopodobna kolizja */
}

int main(void) {
    RUN_TEST(test_derive_deterministic);
    RUN_TEST(test_verifier);
    RUN_TEST(test_block_roundtrip);
    RUN_TEST(test_random);
    return TEST_SUMMARY();
}
```

- [ ] **Step 3: Zaktualizuj `Makefile`** — dodaj crypto.c do CORE i `-lcrypto` do linkowania:

```make
CORE    := src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c src/fs.c
LDLIBS  := -lcrypto
```

I dopisz `$(LDLIBS)` na końcu reguł linkowania:
```make
build/%: tests/%.c $(CORE)
	@mkdir -p build
	$(CC) $(CFLAGS) $< $(CORE) -o $@ $(LDLIBS)

cli: $(CORE) src/cli.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/cli.c $(CORE) -o build/ghostfs-cli $(LDLIBS)

fuse: $(CORE) src/fuse_main.c
	@mkdir -p build
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 src/fuse_main.c $(CORE) $(FUSEFLAGS) -o build/ghostfs $(LDLIBS)
```

- [ ] **Step 4: Uruchom — ma failować** (brak crypto.c):

Run: `make clean && make test 2>&1 | grep -i crypto | head`
Expected: błąd linkera / brak implementacji.

- [ ] **Step 5: Zaimplementuj `src/crypto.c`**

```c
#include "crypto.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <string.h>
#include <errno.h>

int gh_crypto_derive(const char *passphrase, const uint8_t salt[16],
                     uint32_t iters, uint8_t key[64]) {
    if (!passphrase) return -EINVAL;
    if (PKCS5_PBKDF2_HMAC(passphrase, (int)strlen(passphrase), salt, 16,
                          (int)iters, EVP_sha256(), 64, key) != 1)
        return -EIO;
    return 0;
}

void gh_crypto_verifier(const uint8_t key[64], const uint8_t salt[16], uint8_t out[32]) {
    uint8_t buf[80];
    memcpy(buf, key, 64); memcpy(buf + 64, salt, 16);
    SHA256(buf, sizeof(buf), out);
}

int gh_crypto_random(uint8_t *buf, size_t n) {
    return RAND_bytes(buf, (int)n) == 1 ? 0 : -EIO;
}

static int xts(const struct gh_cipher *c, uint64_t blkno,
               const uint8_t *in, uint8_t *out, int enc) {
    uint8_t tweak[16]; memset(tweak, 0, sizeof(tweak));
    for (int i = 0; i < 8; i++) tweak[i] = (uint8_t)(blkno >> (8 * i));  /* LE */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -EIO;
    int ok, outl = 0, fin = 0;
    if (enc) ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_xts(), NULL, c->key, tweak);
    else     ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_xts(), NULL, c->key, tweak);
    if (ok == 1) {
        if (enc) ok = EVP_EncryptUpdate(ctx, out, &outl, in, (int)GH_BLOCK_SIZE);
        else     ok = EVP_DecryptUpdate(ctx, out, &outl, in, (int)GH_BLOCK_SIZE);
    }
    if (ok == 1) {
        if (enc) ok = EVP_EncryptFinal_ex(ctx, out + outl, &fin);
        else     ok = EVP_DecryptFinal_ex(ctx, out + outl, &fin);
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok == 1 ? 0 : -EIO;
}

int gh_crypto_encrypt_block(const struct gh_cipher *c, uint64_t blkno,
                            const uint8_t *in, uint8_t *out) { return xts(c, blkno, in, out, 1); }
int gh_crypto_decrypt_block(const struct gh_cipher *c, uint64_t blkno,
                            const uint8_t *in, uint8_t *out) { return xts(c, blkno, in, out, 0); }
```

- [ ] **Step 6: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_crypto|failed'`
Expected: `test_crypto` `0 failed`; reszta `0 failed`.

- [ ] **Step 7: ASan dla test_crypto**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_crypto.c src/crypto.c -o build/test_crypto_asan -lcrypto && ./build/test_crypto_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 8: Commit**

```bash
git add src/crypto.h src/crypto.c tests/test_crypto.c Makefile
git commit -m "feat: modul kryptograficzny AES-256-XTS + PBKDF2 (OpenSSL)"
```

---

## Task 2: Pola szyfrowania w superbloku (nagłówek)

**Files:** Modify: `src/ghostfs.h`

- [ ] **Step 1: Dodaj stałą i pola superbloku** — po `journal_blocks`:

```c
    uint64_t journal_blocks;
    uint32_t flags;             /* bit GH_SB_ENCRYPTED */
    uint32_t enc_kdf_iters;
    uint8_t  enc_salt[16];
    uint8_t  enc_verifier[32];
```

I stała przy innych `#define` (np. po `GH_ROOT_INO`):

```c
#define GH_SB_ENCRYPTED 0x1u
```

- [ ] **Step 2: Zbuduj — istniejące testy mają przejść** (pola w rezerwie, superblok < 4096):

Run: `make clean && make test`
Expected: wszystkie `0 failed` (nowe pola nie ruszają istniejącego layoutu; czytane jako 0 w starych kontenerach).

- [ ] **Step 3: Commit**

```bash
git add src/ghostfs.h
git commit -m "feat: pola szyfrowania w superbloku (flags/enc_salt/enc_verifier/iters)"
```

---

## Task 3: Świadome szyfru fizyczne I/O w warstwie bloków

**Files:** Modify: `src/block.h`, `src/block.c`; Test: `tests/test_block.c`

- [ ] **Step 1: Rozszerz `src/block.h`** — fwd-decl cipher, pole w gh_dev, deklaracje disk I/O:

```c
struct gh_cipher;   /* zdefiniowane w crypto.h */
struct gh_dev { int fd; uint64_t total_blocks; struct gh_txn *txn; struct gh_cipher *cipher; };

int gh_disk_read(struct gh_dev *dev, uint64_t blkno, void *buf);
int gh_disk_write(struct gh_dev *dev, uint64_t blkno, const void *buf);
```

(Zostaw istniejące deklaracje `gh_block_read/write` itd.)

- [ ] **Step 2: Napisz failujący test w `tests/test_block.c`** (dodaj + RUN_TEST):

```c
#include "../src/crypto.h"
static void test_disk_encryption(void) {
    char tmp[] = "/tmp/ghost_encXXXXXX"; int fd = mkstemp(tmp); close(fd);
    struct gh_dev dev; CHECK_EQ(gh_dev_create(tmp, 16, &dev), 0);
    struct gh_cipher c; memset(&c, 0, sizeof(c));
    uint8_t salt[16]; memset(salt, 9, 16); gh_crypto_derive("pw", salt, 1000, c.key);
    dev.cipher = &c;

    char w[GH_BLOCK_SIZE]; memset(w, 0xBE, sizeof(w));
    CHECK_EQ(gh_disk_write(&dev, 5, w), 0);

    /* surowy odczyt bloku 5 z dysku (z pominieciem szyfru) != jawne */
    char raw[GH_BLOCK_SIZE];
    pread(dev.fd, raw, GH_BLOCK_SIZE, (off_t)5 * GH_BLOCK_SIZE);
    CHECK(memcmp(raw, w, GH_BLOCK_SIZE) != 0);     /* na dysku szyfrogram */

    /* gh_disk_read odszyfrowuje z powrotem */
    char r[GH_BLOCK_SIZE]; CHECK_EQ(gh_disk_read(&dev, 5, r), 0);
    CHECK_EQ(memcmp(r, w, GH_BLOCK_SIZE), 0);

    /* blok 0 (superblok) NIE jest szyfrowany */
    CHECK_EQ(gh_disk_write(&dev, 0, w), 0);
    pread(dev.fd, raw, GH_BLOCK_SIZE, 0);
    CHECK_EQ(memcmp(raw, w, GH_BLOCK_SIZE), 0);     /* jawne na dysku */

    dev.cipher = NULL; gh_dev_close(&dev); unlink(tmp);
}
```

Dodaj `RUN_TEST(test_disk_encryption);`.

- [ ] **Step 3: Uruchom — ma failować** (brak `gh_disk_read/write`):

Run: `make clean && make test 2>&1 | grep -iE 'gh_disk|test_block' | head`
Expected: błąd linkera lub FAIL.

- [ ] **Step 4: Zaimplementuj w `src/block.c`**

Dodaj `#include "crypto.h"` i `#include <stdlib.h>`. W `gh_dev_create`/`gh_dev_open` ustaw `dev->cipher = NULL;`. Dodaj funkcje i przekieruj `io_at` ścieżkę bez-txn:

```c
int gh_disk_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (blkno >= dev->total_blocks) return -EINVAL;
    off_t off = (off_t)blkno * GH_BLOCK_SIZE;
    if (dev->cipher && blkno != 0) {
        uint8_t tmp[GH_BLOCK_SIZE];
        int r = gh_crypto_encrypt_block(dev->cipher, blkno, buf, tmp); if (r) return r;
        ssize_t n = pwrite(dev->fd, tmp, GH_BLOCK_SIZE, off);
        return (n == (ssize_t)GH_BLOCK_SIZE) ? 0 : (n < 0 ? -errno : -EIO);
    }
    ssize_t n = pwrite(dev->fd, buf, GH_BLOCK_SIZE, off);
    return (n == (ssize_t)GH_BLOCK_SIZE) ? 0 : (n < 0 ? -errno : -EIO);
}
int gh_disk_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    if (blkno >= dev->total_blocks) return -EINVAL;
    off_t off = (off_t)blkno * GH_BLOCK_SIZE;
    if (dev->cipher && blkno != 0) {
        uint8_t tmp[GH_BLOCK_SIZE];
        ssize_t n = pread(dev->fd, tmp, GH_BLOCK_SIZE, off);
        if (n != (ssize_t)GH_BLOCK_SIZE) return (n < 0 ? -errno : -EIO);
        return gh_crypto_decrypt_block(dev->cipher, blkno, tmp, buf);
    }
    ssize_t n = pread(dev->fd, buf, GH_BLOCK_SIZE, off);
    return (n == (ssize_t)GH_BLOCK_SIZE) ? 0 : (n < 0 ? -errno : -EIO);
}
```

Następnie w `gh_block_read`/`gh_block_write` ścieżkę BEZ transakcji zamień na wywołanie
`gh_disk_read`/`gh_disk_write` (zamiast `io_at`). Logika transakcji (bufor) bez zmian.
Jeśli `io_at` nie jest już używane — usuń je lub zostaw martwe? USUŃ stare `io_at`, by
nie było ostrzeżenia `-Wunused-function`. Czyli:

```c
int gh_block_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) { memcpy(buf, t->images[i], GH_BLOCK_SIZE); return 0; }
    }
    return gh_disk_read(dev, blkno, buf);
}
int gh_block_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    if (dev->txn && dev->txn->active) {
        struct gh_txn *t = dev->txn;
        for (uint32_t i = 0; i < t->n; i++)
            if (t->blknos[i] == blkno) { memcpy(t->images[i], buf, GH_BLOCK_SIZE); return 0; }
        if (t->n >= t->cap) return -ENOSPC;
        t->blknos[t->n] = blkno; memcpy(t->images[t->n], buf, GH_BLOCK_SIZE); t->n++;
        return 0;
    }
    return gh_disk_write(dev, blkno, buf);
}
```

(Dodaj `#include <errno.h>` jeśli brak.)

- [ ] **Step 5: Uruchom test — ma przejść**

Run: `make clean && make test 2>&1 | grep -E 'test_block|failed'`
Expected: `test_block` `0 failed`; reszta `0 failed` (kontenery jawne — cipher NULL — działają jak dotąd).

- [ ] **Step 6: ASan dla test_block**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_block.c src/block.c src/crypto.c -o build/test_block_asan -lcrypto && ./build/test_block_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 7: Commit**

```bash
git add src/block.h src/block.c tests/test_block.c
git commit -m "feat: szyfrujace fizyczne I/O (gh_disk_read/write); block_read/write przez nie"
```

---

## Task 4: Dziennik przez szyfrujące I/O

**Files:** Modify: `src/journal.c`

- [ ] **Step 1: Zamień `raw_read`/`raw_write` w `src/journal.c`** na wywołania `gh_disk_*`

Zastąp definicje statycznych `raw_write`/`raw_read` tak, by delegowały do warstwy bloków
(albo usuń je i użyj `gh_disk_*` bezpośrednio). Najprościej — zmień ich ciała:

```c
static int raw_write(struct gh_dev *dev, uint64_t blkno, const void *buf) {
    return gh_disk_write(dev, blkno, buf);
}
static int raw_read(struct gh_dev *dev, uint64_t blkno, void *buf) {
    return gh_disk_read(dev, blkno, buf);
}
```

(Dzięki temu region dziennika jest szyfrowany tym samym kluczem; obrazy jawne w buforze
txn trafiają na dysk jako szyfrogram, a przy recover są odszyfrowywane spójnie.)

- [ ] **Step 2: Uruchom testy — mają przejść** (kontenery jawne: cipher NULL, brak zmiany zachowania):

Run: `make clean && make test 2>&1 | grep -E 'test_journal|failed'`
Expected: `test_journal` `0 failed`; cała reszta `0 failed`.

- [ ] **Step 3: ASan (test_journal)**

Run: `cc -std=c11 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -g -Itests tests/test_journal.c src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c src/fs.c -o build/test_journal_asan -lcrypto && ./build/test_journal_asan`
Expected: `0 failed`, brak raportów.

- [ ] **Step 4: Commit**

```bash
git add src/journal.c
git commit -m "feat: dziennik korzysta z szyfrujacego I/O (gh_disk_read/write)"
```

---

## Task 5: Format i montowanie z hasłem

**Files:** Modify: `src/super.h`, `src/super.c`, `src/fs.h`, `src/fs.c`; Create: `tests/test_enc.c`

- [ ] **Step 1: Deklaracje** — `src/super.h`:

```c
int gh_format_enc(const char *path, uint64_t total_blocks, uint64_t inode_count,
                  const char *passphrase);
```

`src/fs.h`:

```c
int gh_fs_mount_key(struct gh_fs*, const char *path, const char *passphrase);
```

- [ ] **Step 2: Napisz failujący test `tests/test_enc.c`**

```c
#include "test.h"
#include "../src/fs.h"
#include "../src/super.h"
#include "../src/ghostfs.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

static void test_encrypted_roundtrip(void) {
    char tmp[] = "/tmp/ghost_efsXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 2048, 128, "tajne-haslo"), 0);

    /* zle/brak hasla -> EACCES */
    struct gh_fs fs;
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, "zle"), -EACCES);
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, NULL), -EACCES);

    /* poprawne haslo -> montuje */
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, "tajne-haslo"), 0);
    CHECK_EQ(gh_fs_mkdir(&fs, "/d", 0755), 0);
    CHECK_EQ(gh_fs_create(&fs, "/d/f.txt", 0644), 0);
    const char *secret = "POUFNY-TEKST-1234567890";
    CHECK_EQ(gh_fs_write(&fs, "/d/f.txt", secret, strlen(secret), 0), (ssize_t)strlen(secret));
    char buf[64] = {0};
    CHECK_EQ(gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0), (ssize_t)strlen(secret));
    CHECK_EQ(memcmp(buf, secret, strlen(secret)), 0);
    int issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs);

    /* at-rest: surowe przeszukanie kontenera NIE zawiera jawnego sekretu */
    int rf = open(tmp, O_RDONLY);
    off_t sz = lseek(rf, 0, SEEK_END); lseek(rf, 0, SEEK_SET);
    char *all = malloc((size_t)sz); ssize_t got = read(rf, all, (size_t)sz); close(rf);
    CHECK(got == sz);
    int found = 0;
    for (off_t i = 0; i + (off_t)strlen(secret) <= sz; i++)
        if (memcmp(all + i, secret, strlen(secret)) == 0) { found = 1; break; }
    CHECK_EQ(found, 0);   /* sekretu nie ma jawnie na dysku */
    free(all);

    /* remount z haslem -> trwalosc */
    CHECK_EQ(gh_fs_mount_key(&fs, tmp, "tajne-haslo"), 0);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(gh_fs_read(&fs, "/d/f.txt", buf, sizeof(buf), 0), (ssize_t)strlen(secret));
    CHECK_EQ(memcmp(buf, secret, strlen(secret)), 0);
    issues = -1; CHECK_EQ(gh_fsck(&fs, 0, &issues), 0); CHECK_EQ(issues, 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

static void test_plain_still_works(void) {
    char tmp[] = "/tmp/ghost_plXXXXXX"; int fd = mkstemp(tmp); close(fd);
    CHECK_EQ(gh_format_enc(tmp, 1024, 64, NULL), 0);   /* NULL = jawny */
    struct gh_fs fs; CHECK_EQ(gh_fs_mount_key(&fs, tmp, NULL), 0);  /* haslo ignorowane */
    CHECK_EQ(gh_fs_create(&fs, "/x", 0644), 0);
    gh_fs_unmount(&fs); unlink(tmp);
}

int main(void) {
    RUN_TEST(test_encrypted_roundtrip);
    RUN_TEST(test_plain_still_works);
    return TEST_SUMMARY();
}
```

- [ ] **Step 3: Uruchom — ma failować** (brak `gh_format_enc`/`gh_fs_mount_key`):

Run: `make clean && make test 2>&1 | grep -iE 'gh_format_enc|mount_key|test_enc' | head`
Expected: błąd linkera.

- [ ] **Step 4: Zaimplementuj `gh_format_enc` w `src/super.c`**

Dodaj `#include "crypto.h"`. Wydziel obecne `gh_format` do `gh_format_enc` z parametrem
`passphrase`, a `gh_format` niech woła `gh_format_enc(path, tb, ic, NULL)`. W
`gh_format_enc`, po zbudowaniu `struct gh_dev dev` i PRZED zapisaniem bloków:

```c
    struct gh_cipher cipher; int encrypted = 0;
    /* ... po memset(&sb,...) i ustawieniu pol layoutu ... */
    if (passphrase && passphrase[0]) {
        encrypted = 1;
        sb.flags = GH_SB_ENCRYPTED;
        sb.enc_kdf_iters = 200000;
        if (gh_crypto_random(sb.enc_salt, sizeof(sb.enc_salt))) { gh_dev_close(&dev); return -EIO; }
        if (gh_crypto_derive(passphrase, sb.enc_salt, sb.enc_kdf_iters, cipher.key)) { gh_dev_close(&dev); return -EIO; }
        gh_crypto_verifier(cipher.key, sb.enc_salt, sb.enc_verifier);
    }
```

Następnie zapisz superblok (blok 0) JAK DOTĄD (jawnie, przez `gh_block_write(&dev,0,blk)`).
ZARAZ PO zapisaniu superbloku ustaw szyfr na urządzeniu, tak by kolejne zapisy
(mapa/i-węzły/dziennik/root) szły szyfrem:

```c
    if (encrypted) dev.cipher = &cipher;
```

(Reszta `gh_format` bez zmian: zerowanie mapy/i-węzłów, zaznaczanie bitów, root z
`.`/`..` — wszystko teraz przez `gh_block_write`/`gh_inode_pwrite`, więc szyfrowane.)
Na końcu, przed `return 0`, wyzeruj `dev.cipher` przed `gh_dev_close` nie jest konieczne
(dev lokalny), ale ustaw `dev.cipher = NULL;` dla higieny.

**UWAGA:** `gh_block_write(&dev, 0, blk)` dla superbloku — blok 0 jest wyłączony ze
szyfrowania w `gh_disk_write`, więc nawet po ustawieniu `dev.cipher` superblok pozostaje
jawny. Kolejność (zapis superbloku, potem ustawienie cipher) i tak jest bezpieczna.

- [ ] **Step 5: Zaimplementuj `gh_fs_mount_key`/`gh_fs_mount` w `src/fs.c`**

Dodaj `#include "crypto.h"` i `#include <stdlib.h>` (getenv). Zastąp `gh_fs_mount`:

```c
int gh_fs_mount_key(struct gh_fs *fs, const char *path, const char *passphrase) {
    int r = gh_dev_open(path, &fs->dev); if (r) return r;
    r = gh_mount_sb(&fs->dev, &fs->sb);
    if (r) { gh_dev_close(&fs->dev); return r; }
    fs->dev.cipher = NULL;
    if (fs->sb.flags & GH_SB_ENCRYPTED) {
        if (!passphrase || !passphrase[0]) { gh_dev_close(&fs->dev); return -EACCES; }
        struct gh_cipher *c = malloc(sizeof(*c));
        if (!c) { gh_dev_close(&fs->dev); return -ENOMEM; }
        if (gh_crypto_derive(passphrase, fs->sb.enc_salt, fs->sb.enc_kdf_iters, c->key)) {
            free(c); gh_dev_close(&fs->dev); return -EIO;
        }
        uint8_t v[32]; gh_crypto_verifier(c->key, fs->sb.enc_salt, v);
        if (memcmp(v, fs->sb.enc_verifier, 32) != 0) {   /* zle haslo */
            free(c); gh_dev_close(&fs->dev); return -EACCES;
        }
        fs->dev.cipher = c;
    }
    r = gh_jrnl_recover(&fs->dev, &fs->sb);
    if (r) { free(fs->dev.cipher); fs->dev.cipher = NULL; gh_dev_close(&fs->dev); return r; }
    return 0;
}
int gh_fs_mount(struct gh_fs *fs, const char *path) {
    return gh_fs_mount_key(fs, path, getenv("GHOSTFS_KEY"));
}
```

W `gh_fs_unmount` zwolnij szyfr:

```c
void gh_fs_unmount(struct gh_fs *fs) {
    free(fs->dev.cipher); fs->dev.cipher = NULL;
    gh_dev_close(&fs->dev);
}
```

- [ ] **Step 6: Uruchom testy — mają przejść**

Run: `make clean && make test`
Expected: wszystkie zestawy `0 failed` (w tym `test_enc`). Istniejące testy A/B (jawne
kontenery) bez zmian.

- [ ] **Step 7: Pełny ASan**

Run: `make test-asan`
Expected: wszystkie `0 failed`, brak raportów (zero wycieków: cipher zwalniany w unmount,
konteksty EVP zwalniane w crypto.c).

- [ ] **Step 8: Commit**

```bash
git add src/super.h src/super.c src/fs.h src/fs.c tests/test_enc.c
git commit -m "feat: format i montowanie z haslem (gh_format_enc/gh_fs_mount_key) + at-rest"
```

---

## Task 6: CLI/FUSE z hasłem + integracja zaszyfrowana

**Files:** Modify: `src/cli.c`, `src/fuse_main.c`, `tests/integration.sh`

- [ ] **Step 1: CLI `format` szyfruje, gdy ustawiony `GHOSTFS_KEY` (`src/cli.c`)**

W `cmd_format` zmień wywołanie z `gh_format(...)` na uwzględniające hasło ze środowiska:

```c
    const char *key = getenv("GHOSTFS_KEY");
    int r = gh_format_enc(argv[2], strtoull(argv[3],0,10), strtoull(argv[4],0,10),
                          (key && key[0]) ? key : NULL);
```

(Dodaj `#include "super.h"` jeśli brak — `gh_format_enc` jest w super.h. Pozostałe
komendy CLI montują przez `gh_fs_mount`, które samo czyta `GHOSTFS_KEY` — bez zmian.)

- [ ] **Step 2: FUSE — bez zmian w kodzie** (montuje przez `gh_fs_mount` → `getenv`).

Zweryfikuj tylko, że `src/fuse_main.c` używa `gh_fs_mount` (tak). Hasło przekazuje się
przez środowisko procesu sterownika.

- [ ] **Step 3: Zbuduj CLI i FUSE**

Run: `make clean && make cli fuse`
Expected: budują się czysto z `-lcrypto`.

- [ ] **Step 4: Dodaj wariant zaszyfrowany do `tests/integration.sh`**

Na końcu skryptu (po sekcji fsck, przed finalnym echo) dodaj osobny scenariusz
zaszyfrowany (nowy kontener i mount z `GHOSTFS_KEY`):

```bash
# 14) wariant zaszyfrowany (AES-256-XTS)
ECONT=$(mktemp /tmp/ghost_enc.XXXXXX.gfs); EMNT=$(mktemp -d)
SECRET="POUFNY-MARKER-$$"
GHOSTFS_KEY="haslo-testowe" "$CLI" format "$ECONT" 8192 256
GHOSTFS_KEY="haslo-testowe" "$GFS" "$ECONT" "$EMNT" -f &
EPID=$!; sleep 1
echo "$SECRET" > "$EMNT/tajne.txt"
ok 'test "$(cat "$EMNT/tajne.txt")" = "$SECRET"' 'zaszyfrowany round-trip'
sync
fusermount3 -u "$EMNT" 2>/dev/null || true; wait $EPID 2>/dev/null || true
ok '! grep -aq "$SECRET" "$ECONT"' 'at-rest: brak jawnego sekretu w kontenerze'
ok '! GHOSTFS_KEY="zle" "$CLI" ls "$ECONT" / 2>/dev/null' 'zle haslo odmawia'
rm -f "$ECONT"; rmdir "$EMNT" 2>/dev/null || true
```

(Funkcja `ok` istnieje od A11. Krok wstaw PRZED finalnym `echo "WSZYSTKIE TESTY..."`.)

- [ ] **Step 5: Uruchom pełną integrację**

Run: `make clean && make test && make cli fuse && ./tests/integration.sh`
Expected: wszystkie `OK:` (w tym `OK: zaszyfrowany round-trip`, `OK: at-rest...`,
`OK: zle haslo odmawia`) i `WSZYSTKIE TESTY INTEGRACYJNE PRZESZŁY`. Posprzątaj mounty.

- [ ] **Step 6: Commit**

```bash
git add src/cli.c tests/integration.sh
git commit -m "feat: CLI format szyfruje z GHOSTFS_KEY + integracja zaszyfrowana"
```

---

## Self-Review (przy pisaniu planu)

**Pokrycie spec C:** modul crypto (Task 1) ✓; pola superbloku (Task 2) ✓; szyfrujące I/O
w block.c (Task 3) ✓; dziennik przez szyfrujące I/O (Task 4) ✓; format/mount z hasłem +
at-rest (Task 5) ✓; CLI/FUSE + integracja (Task 6) ✓. Kompatybilność wstecz (`flags==0`,
cipher NULL) obsłużona ✓. Klucz tylko w pamięci, weryfikator dla hasła ✓.

**Placeholdery:** brak; kod crypto.c i block.c kompletny.

**Spójność typów:** `struct gh_cipher` (crypto.h), `gh_dev.cipher` (block.h fwd-decl),
`gh_disk_read/write` (block.h), `gh_crypto_*` (crypto.h), `gh_format_enc` (super.h),
`gh_fs_mount_key` (fs.h), `GH_SB_ENCRYPTED` (ghostfs.h) — spójne między zadaniami.

**Ryzyka odnotowane:** (1) blok 0 zawsze jawny (magic/sól/weryfikator) — wymuszone w
`gh_disk_*` i kolejności w formacie; (2) journaling+szyfrowanie składają się, bo journal
używa `gh_disk_*` (tweak per-lokalizacja) — obrazy jawne w buforze, szyfrogram na dysku;
(3) wszystkie testy A/B na jawnych kontenerach (cipher NULL) działają bez zmian.
