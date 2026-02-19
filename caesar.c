#include "caesar.h"
#include <stdio.h>
#include <ctype.h>


static void process_file(const char *input_file, const char *output_file, int shift, int mode) {
    FILE *in = fopen(input_file, "r");
    if (!in) {
        printf("error: cant open input file %s\n", input_file);
        return;
    }
    FILE *out = fopen(output_file, "w");
    if (!out) {
        printf("error: cant create output file %s\n", output_file);
        fclose(in);
        return;
    }
    int ch;
    while ((ch = fgetc(in)) != EOF) {
        char result = ch;
        if (isalpha(ch)) {
            char base = isupper(ch) ? 'A' : 'a';
            if (mode == 1) { 
                result = ((ch - base + shift) % 26 + 26) % 26 + base;
            } else { 
                result = ((ch - base - shift) % 26 + 26) % 26 + base;
            }
        }
        fputc(result, out);
    }
    fclose(in);
    fclose(out);
}
void caesar_encrypt(const char *input_file, const char *output_file, int shift) {
    process_file(input_file, output_file, shift, 1);
}
void caesar_decrypt(const char *input_file, const char *output_file, int shift) {
    process_file(input_file, output_file, shift, 0);
}