#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define REGION_SIZE 16

static void* protected_ptr = NULL;

static int region_contains(const void* addr)
{
    if (!protected_ptr || !addr)
        return 0;
    const unsigned char* p = (const unsigned char*)addr;
    const unsigned char* base = (const unsigned char*)protected_ptr;
    return p >= base && p < base + REGION_SIZE;
}

static void handler(int sig, siginfo_t* info, void* ctx)
{
    (void)sig;
    (void)ctx;
    if (info && region_contains(info->si_addr)) {
        printf("ERROR: Access to fully protected memory (read/write forbidden)\n");
        _exit(1);
    }
    printf("ERROR: SIGSEGV at %p\n", info ? info->si_addr : NULL);
    _exit(2);
}

int main(void)
{
    struct sigaction sa = {
        .sa_sigaction = handler,
        .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    protected_ptr = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (protected_ptr == MAP_FAILED)
        return 1;

    *(volatile char*)protected_ptr = 42;
    if (mprotect(protected_ptr, REGION_SIZE, PROT_NONE) != 0)
        return 1;

    printf("Attempting to read PROT_NONE memory (expect SIGSEGV)...\n");
    volatile char x = *(volatile char*)protected_ptr;
    (void)x;

    printf("This line will not be printed\n");
    return 0;
}
