#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "../src/crypto.h"
#include "../src/ghostfs.h"
#include <openssl/evp.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

/* ============================================================================
 * test_cryptoreuse — bramka dla thread-local EVP context w xts():
 *  - BAJT-EXACT: nowy gh_crypto_(en|de)crypt_block == REFERENCYJNA impl per-blok
 *    (lokalny EVP_CIPHER_CTX_new + init(key,tweak) + update + final + free);
 *  - round-trip dec(enc(x))==x; wiele blokow -> rozne tweaki -> rozny szyfrogram;
 *  - zmiana klucza w watku (naprzemiennie 2 klucze) -> kazdy poprawny (re-key);
 *  - N=8 watkow rownolegle enc/dec rozne bloki tym samym kluczem -> bajt-exact.
 * ========================================================================== */

/* REFERENCYJNA implementacja: dokladnie stara sciezka (pelny init per blok). */
static int ref_xts(const struct gh_cipher *c, uint64_t blkno,
                   const uint8_t *in, uint8_t *out, int enc) {
    uint8_t tweak[16]; memset(tweak, 0, sizeof(tweak));
    for (int i = 0; i < 8; i++) tweak[i] = (uint8_t)(blkno >> (8 * i));  /* LE */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
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
    return ok == 1 ? 0 : -1;
}

static void rand_fill(uint8_t *p, size_t n, unsigned seed) {
    /* deterministyczny PRNG (xorshift) — powtarzalne testy */
    uint32_t s = seed ? seed : 0x9e3779b9u;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        p[i] = (uint8_t)(s & 0xff);
    }
}

static void make_key(struct gh_cipher *c, const char *pw) {
    uint8_t salt[16]; memset(salt, 0x5a, sizeof(salt));
    gh_crypto_derive(pw, salt, 1000, c->key);
}

/* ============================ BAJT-EXACT enc+dec vs referencja ============================ */
static void test_byte_exact_vs_ref(void) {
    struct gh_cipher c; make_key(&c, "exact-key");
    uint8_t plain[GH_BLOCK_SIZE];
    uint8_t enc_new[GH_BLOCK_SIZE], enc_ref[GH_BLOCK_SIZE];
    uint8_t dec_new[GH_BLOCK_SIZE], dec_ref[GH_BLOCK_SIZE];

    for (int t = 0; t < 64; t++) {
        rand_fill(plain, sizeof(plain), (unsigned)(t * 2654435761u + 1));
        uint64_t blk = (uint64_t)t * 7919u + 13u;

        /* encrypt: nowy == referencyjny (BAJT-EXACT) */
        CHECK_EQ(gh_crypto_encrypt_block(&c, blk, plain, enc_new), 0);
        CHECK_EQ(ref_xts(&c, blk, plain, enc_ref, 1), 0);
        CHECK_EQ(memcmp(enc_new, enc_ref, GH_BLOCK_SIZE), 0);

        /* decrypt szyfrogramu: nowy == referencyjny (BAJT-EXACT) */
        CHECK_EQ(gh_crypto_decrypt_block(&c, blk, enc_ref, dec_new), 0);
        CHECK_EQ(ref_xts(&c, blk, enc_ref, dec_ref, 0), 0);
        CHECK_EQ(memcmp(dec_new, dec_ref, GH_BLOCK_SIZE), 0);

        /* round-trip: dec(enc(x)) == x */
        CHECK_EQ(memcmp(dec_new, plain, GH_BLOCK_SIZE), 0);
    }
}

/* ============================ wiele blokow -> rozne tweaki ============================ */
static void test_many_blocks_tweak(void) {
    struct gh_cipher c; make_key(&c, "tweak-key");
    uint8_t plain[GH_BLOCK_SIZE];
    rand_fill(plain, sizeof(plain), 12345u);

    enum { N = 64 };
    static uint8_t enc[N][GH_BLOCK_SIZE];
    for (int b = 0; b < N; b++) {
        CHECK_EQ(gh_crypto_encrypt_block(&c, (uint64_t)b, plain, enc[b]), 0);
        /* round-trip kazdego bloku */
        uint8_t dec[GH_BLOCK_SIZE];
        CHECK_EQ(gh_crypto_decrypt_block(&c, (uint64_t)b, enc[b], dec), 0);
        CHECK_EQ(memcmp(dec, plain, GH_BLOCK_SIZE), 0);
    }
    /* ten sam plaintext, rozne blkno -> rozne szyfrogramy (tweak per blok dziala) */
    for (int b = 1; b < N; b++)
        CHECK(memcmp(enc[0], enc[b], GH_BLOCK_SIZE) != 0);
}

/* ============================ zmiana klucza w watku (re-key) ============================ */
static void test_key_switch_in_thread(void) {
    struct gh_cipher c1, c2;
    make_key(&c1, "key-one");
    make_key(&c2, "key-two");
    CHECK(memcmp(c1.key, c2.key, 64) != 0);

    uint8_t plain[GH_BLOCK_SIZE];
    rand_fill(plain, sizeof(plain), 777u);
    uint8_t e1[GH_BLOCK_SIZE], e2[GH_BLOCK_SIZE], ref[GH_BLOCK_SIZE], dec[GH_BLOCK_SIZE];

    /* naprzemiennie 2 klucze w jednym watku — kazdy musi byc poprawny (re-key na zmianie) */
    for (int i = 0; i < 32; i++) {
        uint64_t blk = (uint64_t)i;
        CHECK_EQ(gh_crypto_encrypt_block(&c1, blk, plain, e1), 0);
        CHECK_EQ(ref_xts(&c1, blk, plain, ref, 1), 0);
        CHECK_EQ(memcmp(e1, ref, GH_BLOCK_SIZE), 0);
        CHECK_EQ(gh_crypto_decrypt_block(&c1, blk, e1, dec), 0);
        CHECK_EQ(memcmp(dec, plain, GH_BLOCK_SIZE), 0);

        CHECK_EQ(gh_crypto_encrypt_block(&c2, blk, plain, e2), 0);
        CHECK_EQ(ref_xts(&c2, blk, plain, ref, 1), 0);
        CHECK_EQ(memcmp(e2, ref, GH_BLOCK_SIZE), 0);
        CHECK_EQ(gh_crypto_decrypt_block(&c2, blk, e2, dec), 0);
        CHECK_EQ(memcmp(dec, plain, GH_BLOCK_SIZE), 0);

        /* dwa rozne klucze -> rozne szyfrogramy tego samego plaintextu/bloku */
        CHECK(memcmp(e1, e2, GH_BLOCK_SIZE) != 0);
    }
}

/* ============================ N watkow rownolegle (thread-safety) ============================ */
#define NTHREAD 8
#define NBLK_PER 128

struct worker_arg {
    struct gh_cipher *c;
    int id;
    int failed;     /* >0 = niezgodnosc bajtowa lub blad krypto */
};

static void *worker(void *vp) {
    struct worker_arg *a = vp;
    uint8_t plain[GH_BLOCK_SIZE], enc_new[GH_BLOCK_SIZE], enc_ref[GH_BLOCK_SIZE];
    uint8_t dec[GH_BLOCK_SIZE];
    for (int i = 0; i < NBLK_PER; i++) {
        /* rozne bloki per watek; ten sam klucz dla wszystkich watkow */
        uint64_t blk = (uint64_t)a->id * 100000u + (uint64_t)i;
        rand_fill(plain, sizeof(plain), (unsigned)(a->id * 31u + i + 1));

        if (gh_crypto_encrypt_block(a->c, blk, plain, enc_new) != 0) { a->failed++; continue; }
        if (ref_xts(a->c, blk, plain, enc_ref, 1) != 0) { a->failed++; continue; }
        if (memcmp(enc_new, enc_ref, GH_BLOCK_SIZE) != 0) { a->failed++; continue; }

        if (gh_crypto_decrypt_block(a->c, blk, enc_new, dec) != 0) { a->failed++; continue; }
        if (memcmp(dec, plain, GH_BLOCK_SIZE) != 0) { a->failed++; continue; }
    }
    return NULL;
}

static void test_parallel_threads(void) {
    struct gh_cipher c; make_key(&c, "parallel-key");
    pthread_t th[NTHREAD];
    struct worker_arg args[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        args[i].c = &c; args[i].id = i; args[i].failed = 0;
        CHECK_EQ(pthread_create(&th[i], NULL, worker, &args[i]), 0);
    }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK_EQ(args[i].failed, 0);   /* kazdy watek: kazdy blok bajt-exact, brak wyscigu */
    }
}

/* ============================ dowod: re-key RAZ na watek (opcjonalny) ============================
 * Kompilowany tylko z -DGH_CRYPTO_REKEY_COUNT (test-only licznik w crypto.c). */
#ifdef GH_CRYPTO_REKEY_COUNT
#include <stdatomic.h>
extern _Atomic unsigned long gh_crypto_rekey_count;
static void test_rekey_once_per_key(void) {
    struct gh_cipher c; make_key(&c, "rekey-proof-key");
    uint8_t plain[GH_BLOCK_SIZE], enc[GH_BLOCK_SIZE];
    rand_fill(plain, sizeof(plain), 4242u);

    /* N sekwencyjnych szyfrowan tym samym kluczem w 1 watku -> dokladnie 1 re-key (enc slot) */
    unsigned long before = atomic_load(&gh_crypto_rekey_count);
    for (int i = 0; i < 256; i++)
        CHECK_EQ(gh_crypto_encrypt_block(&c, (uint64_t)i, plain, enc), 0);
    unsigned long after = atomic_load(&gh_crypto_rekey_count);
    CHECK_EQ(after - before, 1);   /* re-key tylko przy pierwszym bloku, dalej re-tweak */
}
#endif

int main(void) {
    RUN_TEST(test_byte_exact_vs_ref);
    RUN_TEST(test_many_blocks_tweak);
    RUN_TEST(test_key_switch_in_thread);
    RUN_TEST(test_parallel_threads);
#ifdef GH_CRYPTO_REKEY_COUNT
    RUN_TEST(test_rekey_once_per_key);
#endif
    return TEST_SUMMARY();
}
