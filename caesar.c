#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "caesar.h"

#define RC4_MASTER_MAX 256
#define RC4_STATE_SIZE 258

static uint8_t* protected_master = NULL;
static size_t protected_master_len = 0;
static uint8_t* protected_rc4 = NULL;
static pthread_mutex_t rc4_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rc4_ksa(const uint8_t* key, size_t key_len, uint8_t* S)
{
    int i, j = 0;
    for (i = 0; i < 256; i++)
        S[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % key_len]) & 255;
        uint8_t t = S[i];
        S[i] = S[j];
        S[j] = t;
    }
}

static void rc4_prga(uint8_t* S, int* ii, int* jj,
                     const uint8_t* src, uint8_t* dst, size_t len)
{
    int i = *ii, j = *jj;
    size_t n;
    for (n = 0; n < len; n++) {
        i = (i + 1) & 255;
        j = (j + S[i]) & 255;
        uint8_t t = S[i];
        S[i] = S[j];
        S[j] = t;
        dst[n] = src[n] ^ S[(S[i] + S[j]) & 255];
    }
    *ii = i;
    *jj = j;
}

void rc4_set_master_key(const char* key, size_t len)
{
    if (!key)
        return;
    if (len > RC4_MASTER_MAX)
        len = RC4_MASTER_MAX;
    if (!protected_master) {
        protected_master = mmap(NULL, RC4_MASTER_MAX, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (protected_master == MAP_FAILED) {
            perror("mmap(master)");
            return;
        }
    }
    pthread_mutex_lock(&rc4_mutex);
    memcpy(protected_master, key, len);
    protected_master_len = len;
    pthread_mutex_unlock(&rc4_mutex);
}

void rc4_crypt(const uint8_t salt[16], const void* src, void* dst, size_t len)
{
    uint8_t keybuf[RC4_MASTER_MAX + 16];
    size_t klen = 0;
    if (!src || !dst || len == 0 || !salt)
        return;
    pthread_mutex_lock(&rc4_mutex);
    if (protected_master && protected_master != MAP_FAILED && protected_master_len > 0) {
        memcpy(keybuf, protected_master, protected_master_len);
        klen = protected_master_len;
    }
    memcpy(keybuf + klen, salt, 16);
    klen += 16;
    if (!protected_rc4) {
        protected_rc4 = mmap(NULL, RC4_STATE_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (protected_rc4 == MAP_FAILED) {
            perror("mmap(rc4)");
            pthread_mutex_unlock(&rc4_mutex);
            return;
        }
    }
    {
        uint8_t* S = protected_rc4;
        int i = 0, j = 0;
        rc4_ksa(keybuf, klen, S);
        rc4_prga(S, &i, &j, (const uint8_t*)src, (uint8_t*)dst, len);
        memset(S, 0, RC4_STATE_SIZE);
    }
    pthread_mutex_unlock(&rc4_mutex);
}
