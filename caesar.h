#ifndef CAESAR_H
#define CAESAR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc4_stream rc4_stream_t;
void rc4_set_master_key(const char* key, size_t len);
rc4_stream_t* rc4_stream_create(void);
void rc4_stream_destroy(rc4_stream_t* ctx);
void rc4_stream_begin(rc4_stream_t* ctx, const uint8_t salt[16]);
void rc4_stream_crypt(rc4_stream_t* ctx, const uint8_t* src, uint8_t* dst, size_t len);
void rc4_stream_end(rc4_stream_t* ctx);
void rc4_crypt(const uint8_t salt[16], const void* src, void* dst, size_t len);

#ifdef __cplusplus
}
#endif
#endif
