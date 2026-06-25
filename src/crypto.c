#include "crypto.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

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

/* ----------------------------------------------------------------------------
 * Thread-local AES-256-XTS context (re-key RAZ na watek, re-tweak per blok).
 *
 * Per-blok pelny EVP_*Init_ex(key) robi caly key-schedule AES-256 — to dominujacy
 * koszt deszyfrowania sekwencyjnego. Trzymamy kontekst EVP per-watek (TLS) zainic-
 * jowany kluczem RAZ; per blok ustawiamy tylko tweak (IV) bez re-key. Bajt-exact
 * wzgledem starej sciezki (ten sam klucz + ten sam tweak = ten sam wynik XTS).
 *
 * Trzymamy DWA konteksty (enc i dec) by uniknac re-key przy przeplataniu enc/dec
 * w tym samym watku. Spojnosc klucza pilnuje 64-bitowy FNV-1a z 64-bajtowego klucza.
 *
 * pthread_key_create z destruktorem zwalnia konteksty przy wyjsciu watku (ASan-clean);
 * pthread_once inicjalizuje klucz TLS raz na proces.
 * -------------------------------------------------------------------------- */

struct tl_xts {
    EVP_CIPHER_CTX *ctx[2];   /* [0]=dec, [1]=enc */
    uint64_t keyhash[2];      /* hash klucza, ktorym zainicjowano ctx[i]; 0 = niezainicj. */
    int have[2];              /* czy ctx[i] zainicjowano kluczem chocby raz */
};

static pthread_key_t tl_key;
static pthread_once_t tl_once = PTHREAD_ONCE_INIT;

#ifdef GH_CRYPTO_REKEY_COUNT
/* Test-only: licznik re-key (EVP_*Init_ex z kluczem). Zero-cost w produkcji. */
#include <stdatomic.h>
_Atomic unsigned long gh_crypto_rekey_count = 0;
#endif

static void tl_xts_free(void *p) {
    struct tl_xts *t = p;
    if (!t) return;
    if (t->ctx[0]) EVP_CIPHER_CTX_free(t->ctx[0]);
    if (t->ctx[1]) EVP_CIPHER_CTX_free(t->ctx[1]);
    free(t);
}

static void tl_make_key(void) {
    pthread_key_create(&tl_key, tl_xts_free);
}

static uint64_t key_fnv1a(const uint8_t key[64]) {
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a offset basis (64-bit) */
    for (int i = 0; i < 64; i++) {
        h ^= key[i];
        h *= 1099511628211ULL;             /* FNV prime */
    }
    return h;
}

/* Sciezka awaryjna: stara semantyka (lokalny ctx, pelny init z key+tweak). */
static int xts_fallback(const struct gh_cipher *c, const uint8_t tweak[16],
                        const uint8_t *in, uint8_t *out, int enc) {
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

static int xts(const struct gh_cipher *c, uint64_t blkno,
               const uint8_t *in, uint8_t *out, int enc) {
    uint8_t tweak[16]; memset(tweak, 0, sizeof(tweak));
    for (int i = 0; i < 8; i++) tweak[i] = (uint8_t)(blkno >> (8 * i));  /* LE */

    pthread_once(&tl_once, tl_make_key);
    struct tl_xts *t = pthread_getspecific(tl_key);
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t || pthread_setspecific(tl_key, t) != 0) {
            free(t);
            return xts_fallback(c, tweak, in, out, enc);   /* OOM -> poprawnosc */
        }
    }

    int slot = enc ? 1 : 0;
    EVP_CIPHER_CTX *ctx = t->ctx[slot];
    if (!ctx) {
        ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return xts_fallback(c, tweak, in, out, enc);
        t->ctx[slot] = ctx;
    }

    uint64_t kh = key_fnv1a(c->key);
    /* Re-key tylko gdy inny klucz niz ostatnio (lub pierwszy raz). Key-schedule RAZ. */
    if (!t->have[slot] || t->keyhash[slot] != kh) {
        int ik = enc ? EVP_EncryptInit_ex(ctx, EVP_aes_256_xts(), NULL, c->key, NULL)
                     : EVP_DecryptInit_ex(ctx, EVP_aes_256_xts(), NULL, c->key, NULL);
        if (ik != 1) {
            t->have[slot] = 0;   /* nie psuj stanu: wymus re-key nastepnym razem */
            return xts_fallback(c, tweak, in, out, enc);
        }
        t->keyhash[slot] = kh;
        t->have[slot] = 1;
#ifdef GH_CRYPTO_REKEY_COUNT
        atomic_fetch_add(&gh_crypto_rekey_count, 1);
#endif
    }

    /* Per blok: ustaw tylko tweak (IV), BEZ re-key. */
    int outl = 0, fin = 0;
    int it = enc ? EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, tweak)
                 : EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, tweak);
    if (it != 1) return xts_fallback(c, tweak, in, out, enc);

    int ok = enc ? EVP_EncryptUpdate(ctx, out, &outl, in, (int)GH_BLOCK_SIZE)
                 : EVP_DecryptUpdate(ctx, out, &outl, in, (int)GH_BLOCK_SIZE);
    if (ok == 1)
        ok = enc ? EVP_EncryptFinal_ex(ctx, out + outl, &fin)
                 : EVP_DecryptFinal_ex(ctx, out + outl, &fin);
    if (ok != 1) {
        /* Stan ctx moze byc nadwyrezony — wymus re-key nastepnym razem, fallback teraz. */
        t->have[slot] = 0;
        return xts_fallback(c, tweak, in, out, enc);
    }
    return 0;
}

int gh_crypto_encrypt_block(const struct gh_cipher *c, uint64_t blkno,
                            const uint8_t *in, uint8_t *out) { return xts(c, blkno, in, out, 1); }
int gh_crypto_decrypt_block(const struct gh_cipher *c, uint64_t blkno,
                            const uint8_t *in, uint8_t *out) { return xts(c, blkno, in, out, 0); }

void gh_crypto_wipe(struct gh_cipher *c) {
    if (c) OPENSSL_cleanse(c->key, sizeof(c->key));
}

int gh_read_password(const char *prompt, char *buf, size_t n) {
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return -ENOTTY;
    struct termios old, noecho;
    fputs(prompt, tty); fflush(tty);
    if (tcgetattr(fileno(tty), &old)) { fclose(tty); return -EIO; }
    noecho = old; noecho.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(fileno(tty), TCSANOW, &noecho);
    char *r = fgets(buf, (int)n, tty);
    tcsetattr(fileno(tty), TCSANOW, &old);
    fputc('\n', tty); fclose(tty);
    if (!r) return -EIO;
    size_t L = strlen(buf);
    if (L && buf[L-1] == '\n') buf[L-1] = '\0';
    return 0;
}
