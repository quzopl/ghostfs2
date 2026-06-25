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

    uint8_t plain[GH_BLOCK_SIZE]; for (unsigned i = 0; i < GH_BLOCK_SIZE; i++) plain[i] = (uint8_t)(i*3+1);
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
