#ifndef CAESAR_H
#define CAESAR_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

void set_key(char key);

void caesar(void* src, void* dst, int len);

#ifdef __cplusplus
}
#endif

#endif
