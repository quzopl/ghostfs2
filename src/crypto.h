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
void gh_crypto_wipe(struct gh_cipher *c);
int  gh_read_password(const char *prompt, char *buf, size_t n);
#endif
