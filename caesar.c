#include "caesar.h"

static uint8_t encryption_key = 0;
void set_key(uint8_t key)
{
    encryption_key = key;
}

void caesar(const void* src, void* dst, size_t len)
{
    if (!src || !dst || len == 0)
        return;
    const uint8_t* src_ptr = (const uint8_t*)src;
    uint8_t* dst_ptr = (uint8_t*)dst;
    for (size_t i = 0; i < len; i++)
    {
        dst_ptr[i] = src_ptr[i] ^ encryption_key;
    }
}