#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "caesar.h"

#define RC4_MASTER_MAX 256
#define RC4_S_SIZE 256
#define RC4_STATE_BYTES (RC4_S_SIZE + 2 * (int)sizeof(int))

struct rc4_stream {
    uint8_t* state;
    size_t map_len;
};

static uint8_t* protected_master = NULL;
static size_t protected_master_len = 0;
static size_t protected_master_map = 0;
static pthread_mutex_t rc4_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t page_size(void)
{
    static size_t psz;
    if (!psz) {
        long p = sysconf(_SC_PAGESIZE);
        psz = p > 0 ? (size_t)p : 4096;
    }
    return psz;
}
static size_t map_round(size_t need)
{
    size_t psz = page_size();
    return (need + psz - 1) & ~(psz - 1);
}
static int seal_mem(void* addr, size_t len)
{
    if (mprotect(addr, len, PROT_NONE) != 0) {
        perror("mprotect(PROT_NONE)");
        return -1;
    }
    return 0;
}
static int unseal_mem(void* addr, size_t len)
{
    if (mprotect(addr, len, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect(RW)");
        return -1;
    }
    return 0;
}
static int* rc4_i(rc4_stream_t* ctx)
{
    return (int*)(ctx->state + RC4_S_SIZE);
}
static int* rc4_j(rc4_stream_t* ctx)
{
    return (int*)(ctx->state + RC4_S_SIZE + sizeof(int));
}
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

rc4_stream_t* rc4_stream_create(void)
{
    rc4_stream_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        perror("calloc(rc4_stream)");
    return ctx;
}

void rc4_stream_destroy(rc4_stream_t* ctx)
{
    if (!ctx)
        return;
    if (ctx->state && ctx->map_len > 0) {
        if (unseal_mem(ctx->state, ctx->map_len) == 0)
            memset(ctx->state, 0, RC4_STATE_BYTES);
        munmap(ctx->state, ctx->map_len);
        ctx->state = NULL;
        ctx->map_len = 0;
    }
    free(ctx);
}

void rc4_set_master_key(const char* key, size_t len)
{
    if (!key)
        return;
    if (len > RC4_MASTER_MAX)
        len = RC4_MASTER_MAX;
    if (!protected_master) {
        protected_master_map = map_round(RC4_MASTER_MAX);
        protected_master = mmap(NULL, protected_master_map, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (protected_master == MAP_FAILED) {
            perror("mmap(master)");
            protected_master = NULL;
            return;
        }
    }
    pthread_mutex_lock(&rc4_mutex);
    if (unseal_mem(protected_master, protected_master_map) != 0) {
        pthread_mutex_unlock(&rc4_mutex);
        return;
    }
    memcpy(protected_master, key, len);
    protected_master_len = len;
    if (len < protected_master_map)
        memset(protected_master + len, 0, protected_master_map - len);
    seal_mem(protected_master, protected_master_map);
    pthread_mutex_unlock(&rc4_mutex);
}

void rc4_stream_begin(rc4_stream_t* ctx, const uint8_t salt[16])
{
    uint8_t keybuf[RC4_MASTER_MAX + 16];
    size_t klen = 0;

    if (!ctx || !salt)
        return;
    if (ctx->state) {
        if (unseal_mem(ctx->state, ctx->map_len) == 0)
            memset(ctx->state, 0, RC4_STATE_BYTES);
        munmap(ctx->state, ctx->map_len);
        ctx->state = NULL;
        ctx->map_len = 0;
    }
    ctx->map_len = map_round(RC4_STATE_BYTES);
    ctx->state = mmap(NULL, ctx->map_len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ctx->state == MAP_FAILED) {
        perror("mmap(rc4 state)");
        ctx->state = NULL;
        ctx->map_len = 0;
        return;
    }
    pthread_mutex_lock(&rc4_mutex);
    if (protected_master && protected_master != MAP_FAILED && protected_master_len > 0) {
        if (unseal_mem(protected_master, protected_master_map) == 0) {
            memcpy(keybuf, protected_master, protected_master_len);
            klen = protected_master_len;
            seal_mem(protected_master, protected_master_map);
        }
    }
    pthread_mutex_unlock(&rc4_mutex);
    memcpy(keybuf + klen, salt, 16);
    klen += 16;
    rc4_ksa(keybuf, klen, ctx->state);
    memset(keybuf, 0, sizeof(keybuf));
    *rc4_i(ctx) = 0;
    *rc4_j(ctx) = 0;
}

void rc4_stream_crypt(rc4_stream_t* ctx, const uint8_t* src, uint8_t* dst, size_t len)
{
    if (!ctx || !ctx->state || !src || !dst || len == 0)
        return;
    rc4_prga(ctx->state, rc4_i(ctx), rc4_j(ctx), src, dst, len);
}

void rc4_stream_end(rc4_stream_t* ctx)
{
    if (!ctx || !ctx->state || ctx->map_len == 0)
        return;
    if (unseal_mem(ctx->state, ctx->map_len) == 0) {
        memset(ctx->state, 0, RC4_STATE_BYTES);
        seal_mem(ctx->state, ctx->map_len);
    }
}

void rc4_crypt(const uint8_t salt[16], const void* src, void* dst, size_t len)
{
    rc4_stream_t* stream;

    if (!src || !dst || len == 0 || !salt)
        return;
    stream = rc4_stream_create();
    if (!stream)
        return;
    rc4_stream_begin(stream, salt);
    rc4_stream_crypt(stream, (const uint8_t*)src, (uint8_t*)dst, len);
    rc4_stream_destroy(stream);
}
