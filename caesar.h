#ifndef CAESAR_H
#define CAESAR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void set_key(uint8_t key);
void caesar(const void* src, void* dst, size_t len);

#ifdef __cplusplus
}
#endif
#endif