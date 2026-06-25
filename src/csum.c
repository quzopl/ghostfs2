#include "csum.h"
static uint32_t t[8][256];
static int built = 0;
static void build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[0][i] = c;
    }
    /* slice-by-8: tablice pochodne (ten sam wielomian, identyczny wynik) */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = t[0][i];
        for (int k = 1; k < 8; k++) {
            c = (c >> 8) ^ t[0][c & 0xFF];
            t[k][i] = c;
        }
    }
    built = 1;
}
uint32_t gh_crc32_update(uint32_t crc, const void *buf, size_t len) {
    if (!built) build();
    const uint8_t *p = buf;
    while (len >= 8) {                       /* 8 bajtow na iteracje */
        crc ^= (uint32_t)p[0] | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
             | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        crc = t[7][crc & 0xFF]         ^ t[6][(crc >> 8) & 0xFF]
            ^ t[5][(crc >> 16) & 0xFF] ^ t[4][(crc >> 24) & 0xFF]
            ^ t[3][hi & 0xFF]          ^ t[2][(hi >> 8) & 0xFF]
            ^ t[1][(hi >> 16) & 0xFF]  ^ t[0][(hi >> 24) & 0xFF];
        p += 8; len -= 8;
    }
    while (len--) crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);   /* ogon: bajtowo */
    return crc;
}
uint32_t gh_crc32(const void *buf, size_t len) {
    return gh_crc32_update(0xFFFFFFFFu, buf, len) ^ 0xFFFFFFFFu;
}
