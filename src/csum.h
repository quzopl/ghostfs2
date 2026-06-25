#ifndef GH_CSUM_H
#define GH_CSUM_H
#include <stdint.h>
#include <stddef.h>
uint32_t gh_crc32(const void *buf, size_t len);
uint32_t gh_crc32_update(uint32_t crc, const void *buf, size_t len);
#endif
