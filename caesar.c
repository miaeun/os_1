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

