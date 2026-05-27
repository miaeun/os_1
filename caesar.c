#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "caesar.h"

#define KEY_SIZE 16

static uint8_t* protected_key = NULL;
static pthread_mutex_t key_access_mutex = PTHREAD_MUTEX_INITIALIZER;

static void sigsegv_handler(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)context;
    if (protected_key && info && info->si_addr == protected_key) {
        fprintf(stderr, "\nERROR: Attempt to write to protected memory region (key storage)\n");
        _exit(1);
    }
    if (protected_key) {
        fprintf(stderr,
                "\nERROR: SIGSEGV at %p (not the protected key region at %p)\n",
                info ? info->si_addr : NULL, (void*)protected_key);
    } else {
        fprintf(stderr,
                "\nERROR: SIGSEGV at %p (protected key region not mapped)\n",
                info ? info->si_addr : NULL);
    }
    _exit(2);
}

static void setup_sigsegv_handler(void)
{
    struct sigaction sa;
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

void set_key(uint8_t key)
{
    if (!protected_key) {
        protected_key = mmap(NULL, KEY_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (protected_key == MAP_FAILED) {
            perror("mmap(key)");
            exit(EXIT_FAILURE);
        }

        setup_sigsegv_handler();

        if (mprotect(protected_key, KEY_SIZE, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect(key init)");
            exit(EXIT_FAILURE);
        }
        memset(protected_key, 0, KEY_SIZE);
        memcpy(protected_key, &key, 1);
        if (mprotect(protected_key, KEY_SIZE, PROT_READ) != 0) {
            perror("mprotect(key read-only)");
            exit(EXIT_FAILURE);
        }
    } else {
        pthread_mutex_lock(&key_access_mutex);
        if (mprotect(protected_key, KEY_SIZE, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect(key rewrite)");
            pthread_mutex_unlock(&key_access_mutex);
            exit(EXIT_FAILURE);
        }
        memcpy(protected_key, &key, 1);
        if (mprotect(protected_key, KEY_SIZE, PROT_READ) != 0) {
            perror("mprotect(key read-only after rewrite)");
            pthread_mutex_unlock(&key_access_mutex);
            exit(EXIT_FAILURE);
        }
        pthread_mutex_unlock(&key_access_mutex);
    }
}

void caesar(const void* src, void* dst, size_t len)
{
    if (!src || !dst || len == 0) 
        return;

    uint8_t key = 0;
    if (!protected_key || protected_key == MAP_FAILED) {
        key = 0;
    } else {
        pthread_mutex_lock(&key_access_mutex);
        if (mprotect(protected_key, KEY_SIZE, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect(caesar before read)");
            memcpy(&key, protected_key, 1);
        } else {
            memcpy(&key, protected_key, 1);
            if (mprotect(protected_key, KEY_SIZE, PROT_READ) != 0)
                perror("mprotect(caesar after read)");
        }
        pthread_mutex_unlock(&key_access_mutex);
    }

    const uint8_t* src_ptr = (const uint8_t*)src;
    uint8_t* dst_ptr = (uint8_t*)dst;
    for (size_t i = 0; i < len; i++)
    {
        dst_ptr[i] = src_ptr[i] ^ key;
    }
}

#define RC4_MASTER_MAX 256
#define RC4_STATE_SIZE 258

static uint8_t* protected_master = NULL;
static size_t protected_master_len = 0;
static uint8_t* protected_rc4 = NULL;
static pthread_mutex_t rc4_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rc4_ksa(const uint8_t* key, size_t key_len, uint8_t* S)
{
    int i;
    int j = 0;
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
    int i = *ii;
    int j = *jj;
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
        setup_sigsegv_handler();
    }

    pthread_mutex_lock(&rc4_mutex);
    if (mprotect(protected_master, RC4_MASTER_MAX, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect(master write)");
        pthread_mutex_unlock(&rc4_mutex);
        return;
    }
    memcpy(protected_master, key, len);
    protected_master_len = len;
    if (mprotect(protected_master, RC4_MASTER_MAX, PROT_READ) != 0)
        perror("mprotect(master read)");
    pthread_mutex_unlock(&rc4_mutex);
}

void rc4_crypt(const uint8_t salt[16], const void* src, void* dst, size_t len)
{
    if (!src || !dst || len == 0 || !salt)
        return;

    uint8_t keybuf[RC4_MASTER_MAX + 16];
    size_t klen = 0;

    pthread_mutex_lock(&rc4_mutex);

    if (protected_master && protected_master != MAP_FAILED && protected_master_len > 0) {
        if (mprotect(protected_master, RC4_MASTER_MAX, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect(master read for crypt)");
        } else {
            memcpy(keybuf, protected_master, protected_master_len);
            klen = protected_master_len;
            if (mprotect(protected_master, RC4_MASTER_MAX, PROT_READ) != 0)
                perror("mprotect(master read after crypt)");
        }
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

    if (mprotect(protected_rc4, RC4_STATE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect(rc4 write)");
        pthread_mutex_unlock(&rc4_mutex);
        return;
    }

    uint8_t* S = protected_rc4;
    int i = 0;
    int j = 0;
    rc4_ksa(keybuf, klen, S);
    rc4_prga(S, &i, &j, (const uint8_t*)src, (uint8_t*)dst, len);
    memset(S, 0, RC4_STATE_SIZE);
    if (mprotect(protected_rc4, RC4_STATE_SIZE, PROT_READ) != 0)
        perror("mprotect(rc4 read)");

    pthread_mutex_unlock(&rc4_mutex);
}

__attribute__((destructor))
static void cleanup_protected_key(void)
{
    if (!protected_key || protected_key == MAP_FAILED)
        return;

    pthread_mutex_lock(&key_access_mutex);
    if (mprotect(protected_key, KEY_SIZE, PROT_READ | PROT_WRITE) == 0)
        memset(protected_key, 0, KEY_SIZE);
    munmap(protected_key, KEY_SIZE); //free pamyat
    protected_key = NULL; //nulling ukazatel
    pthread_mutex_unlock(&key_access_mutex);
}

