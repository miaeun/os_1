#ifndef CAESAR_H
#define CAESAR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void rc4_set_master_key(const char* key, size_t len);
void rc4_crypt(const uint8_t salt[16], const void* src, void* dst, size_t len);

#ifdef __cplusplus
}
#endif
#endif
