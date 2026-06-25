#include "crypto.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <termios.h>
#include <stdio.h>
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
