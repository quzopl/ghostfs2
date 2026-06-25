#include "test.h"
#include "../src/csum.h"
#include <string.h>
static void test_crc(void) {
    CHECK_EQ(gh_crc32("123456789", 9), 0xCBF43926u);   /* znana wartosc referencyjna */
    CHECK(gh_crc32("a", 1) != gh_crc32("b", 1));
    char z[4096]; memset(z, 0, sizeof(z));
    char o[4096]; memset(o, 0, sizeof(o)); o[100] = 1;
    CHECK(gh_crc32(z, 4096) != gh_crc32(o, 4096));     /* jednobitowa zmiana -> inny CRC */
}
/* referencyjny CRC bajt-po-bajcie (stary algorytm) — slice-by-8 MUSI dac identyczny wynik */
static uint32_t ref_crc(const void *buf, size_t len) {
    static uint32_t tb[256]; static int b = 0;
    if (!b) { for (uint32_t i=0;i<256;i++){ uint32_t c=i; for(int k=0;k<8;k++) c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1); tb[i]=c; } b=1; }
    const uint8_t *p = buf; uint32_t crc = 0xFFFFFFFFu;
    for (size_t i=0;i<len;i++) crc = tb[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
static void test_crc_slice_matches(void) {
    size_t lens[] = {0,1,2,7,8,9,15,16,17,255,256,257,1000,4095,4096,4097};
    uint8_t buf[4100];
    uint32_t s = 0x12345678u;
    for (size_t i=0;i<sizeof(buf);i++){ s = s*1103515245u + 12345u; buf[i] = (uint8_t)(s >> 16); }
    for (size_t li=0; li<sizeof(lens)/sizeof(lens[0]); li++) {
        size_t L = lens[li];
        CHECK_EQ(gh_crc32(buf, L), ref_crc(buf, L));   /* slice-by-8 == bajt-po-bajcie */
    }
    /* wektor znany (standardowy CRC32) — kompatybilnosc formatu */
    CHECK_EQ(gh_crc32("123456789", 9), 0xCBF43926u);
    /* inkrementalnosc: podzielony update == jeden update (CRC dziennika) */
    uint32_t a = 0xFFFFFFFFu;
    a = gh_crc32_update(a, buf, 13);
    a = gh_crc32_update(a, buf+13, 4096-13);
    a = gh_crc32_update(a, buf+4096, 4);
    uint32_t whole = gh_crc32_update(0xFFFFFFFFu, buf, 4100);
    CHECK_EQ(a, whole);
}
int main(void) { RUN_TEST(test_crc); RUN_TEST(test_crc_slice_matches); return TEST_SUMMARY(); }
