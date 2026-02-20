#include "caesar.h"

static char encryption_key = 0;

void set_key(char key)
{
    encryption_key = key;
}

void caesar(void* src, void* dst, int len)
{
    unsigned char* src_ptr = (unsigned char*)src;
    unsigned char* dst_ptr = (unsigned char*)dst;
    unsigned char key = (unsigned char)encryption_key;

    for (int i = 0; i < len; i++) {
        dst_ptr[i] = src_ptr[i] ^ key;
    }
}
