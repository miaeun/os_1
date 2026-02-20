#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>

typedef void (*set_key_t)(char);
typedef void (*caesar_t)(void*, void*, int);

int main(int argc, char* argv[])
{
    char* lib_path;
    char key;
    char* input_path;
    char* output_path;

    if (argc != 5) {
        fprintf(stderr, "Usage: %s <lib_path> <key> <input_file> <output_file>\n",
                argv[0]);
        fprintf(stderr, "  lib_path   - path to libcaesar.so\n");
        fprintf(stderr, "  key        - encryption key (character)\n");
        fprintf(stderr, "  input_file - file to encrypt/decrypt\n");
        fprintf(stderr, "  output_file - output file path\n");
        return 1;
    }

    lib_path = argv[1];
    key = argv[2][0];
    input_path = argv[3];
    output_path = argv[4];

    printf("Caesar XOR Encryption Test\n");
    printf("Library: %s\n", lib_path);
    printf("Key: '%c' (0x%02X)\n", (key >= 32 && key < 127) ? key : '?', (unsigned char)key);
    printf("Input: %s\n", input_path);
    printf("Output: %s\n", output_path);

    void* handle = dlopen(lib_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "Error: Failed to load library: %s\n", dlerror());
        return 1;
    }
    printf("Library loaded successfully\n");

    dlerror();

    set_key_t set_key = (set_key_t)dlsym(handle, "set_key");
    const char* error = dlerror();
    if (error) {
        fprintf(stderr, "Error: Failed to find set_key: %s\n", error);
        dlclose(handle);
        return 1;
    }

    caesar_t caesar = (caesar_t)dlsym(handle, "caesar");
    error = dlerror();
    if (error) {
        fprintf(stderr, "Error: Failed to find caesar: %s\n", error);
        dlclose(handle);
        return 1;
    }
    printf("Functions resolved successfully\n");

    FILE* fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "Error: Cannot open input file '%s': %s\n",
                input_path, strerror(errno));
        dlclose(handle);
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    if (file_size < 0) {
        fprintf(stderr, "Error: Cannot determine file size\n");
        fclose(fin);
        dlclose(handle);
        return 1;
    }
    printf("Input file size: %ld bytes\n", file_size);
    unsigned char* input_data = (unsigned char*)malloc(file_size);
    unsigned char* output_data = (unsigned char*)malloc(file_size);
    if (!input_data || !output_data) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        free(input_data);
        free(output_data);
        fclose(fin);
        dlclose(handle);
        return 1;
    }

    size_t bytes_read = fread(input_data, 1, file_size, fin);
    fclose(fin);

    if (bytes_read != (size_t)file_size) {
        fprintf(stderr, "Error: Failed to read entire file\n");
        free(input_data);
        free(output_data);
        dlclose(handle);
        return 1;
    }
    printf("Read %zu bytes from input file\n", bytes_read);

    set_key(key);
    printf("Key set to '%c'\n", (key >= 32 && key < 127) ? key : '?');

    caesar(input_data, output_data, (int)file_size);
    printf("Encryption completed\n");

    FILE* fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Cannot open output file '%s': %s\n",
                output_path, strerror(errno));
        free(input_data);
        free(output_data);
        dlclose(handle);
        return 1;
    }

    size_t bytes_written = fwrite(output_data, 1, file_size, fout);
    fclose(fout);

    if (bytes_written != (size_t)file_size) {
        fprintf(stderr, "Error: Failed to write entire file\n");
        free(input_data);
        free(output_data);
        dlclose(handle);
        return 1;
    }
    printf("Wrote %zu bytes to output file\n", bytes_written);

    printf("\n Verification: Double encryption test\n");
    unsigned char* decrypted = (unsigned char*)malloc(file_size);
    if (decrypted) {
        caesar(output_data, decrypted, (int)file_size);

        int match = (memcmp(input_data, decrypted, file_size) == 0);
        printf("Double encryption test: %s\n", match ? "PASSED" : "FAILED");

        free(decrypted);
    }

    free(input_data);
    free(output_data);
    dlclose(handle);

    printf("\nTest completed successfully\n");
    return 0;
}
