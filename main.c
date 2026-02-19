#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>


typedef void (*caesar_func)(const char*, const char*, int);
int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("usage: %s <path_to_lib> <key> <input> <output>\n", argv[0]);
        printf("example: %s ./libcaesar.so 3 input.txt output.txt\n", argv[0]);
        return 1;
    }
    char *lib_path = argv[1];
    int key = atoi(argv[2]);
    char *input_file = argv[3];
    char *output_file = argv[4];
    void *handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "lib error: %s\n", dlerror());
        return 1;
    }
    caesar_func encrypt = (caesar_func)dlsym(handle, "caesar_encrypt");
    caesar_func decrypt = (caesar_func)dlsym(handle, "caesar_decrypt");
    char *error = dlerror();
    if (error) {
        fprintf(stderr, "function error: %s\n", error);
        dlclose(handle);
        return 1;
    }
    printf("lib downloaded successfully\n");
    printf("key: %d\n", key);
    printf("input file: %s\n", input_file);
    printf("output file: %s\n", output_file);
    printf("\nprocess...\n");
    encrypt(input_file, output_file, key);
    printf("done! file: %s\n", output_file);
    char decrypted_file[] = "decrypted_output.txt";
    printf("\ndecipher back %s...\n", decrypted_file);
    decrypt(output_file, decrypted_file, key);
    printf("done!\n");
    dlclose(handle);
    return 0;
}