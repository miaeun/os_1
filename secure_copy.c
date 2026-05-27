#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "caesar.h"

#define IMAGE_THREADS 5
#define SALT_SIZE 16
#define IMAGE_MAX_FILES 4096
#define IMAGE_MAX_NAME_LEN 65536
#define IMAGE_RECORD_MIN (8 + SALT_SIZE)
#define MAX_FILES 100
#define MAX_WORKERS 5

typedef struct {
    char* input_files[MAX_FILES];
    char* image_names[MAX_FILES];
    int file_count;
    int current_index;
    int completed_count;
    pthread_mutex_t mutex;
    FILE* image;
    int image_errors;
    int worker_count;
} job_context_t;

typedef struct {
    job_context_t* job_ctx;
    int thread_id;
} thread_context_t;

typedef struct {
    char* disk_path;
    char* image_name;
} image_file_t;

typedef struct {
    char* name;
    uint32_t size;
} list_entry_t;

static pthread_mutex_t image_mutex = PTHREAD_MUTEX_INITIALIZER;

static int add_one_file_to_image(const char* disk_path, const char* image_name, FILE* img);

static void job_zero(job_context_t* job)
{
    memset(job, 0, sizeof(*job));
}

static size_t file_size(FILE* f)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return 0;
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0)
        return 0;
    return (size_t)sz;
}

static char* get_next_file(job_context_t* ctx, int thread_id)
{
    char* path = NULL;
    (void)thread_id;
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->current_index < ctx->file_count)
        path = ctx->input_files[ctx->current_index++];
    pthread_mutex_unlock(&ctx->mutex);
    return path;
}

static void inc_done(job_context_t* ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->completed_count++;
    pthread_mutex_unlock(&ctx->mutex);
}

static void* worker_thread(void* arg)
{
    thread_context_t* t = (thread_context_t*)arg;
    job_context_t* job = t->job_ctx;
    for (;;) {
        char* path = get_next_file(job, t->thread_id);
        if (!path)
            break;
        int idx = job->current_index - 1;
        if (add_one_file_to_image(path, job->image_names[idx], job->image) != 0)
            job->image_errors = 1;
        inc_done(job);
    }
    return NULL;
}

static void process_parallel(job_context_t* job)
{
    int nw = job->worker_count > 0 ? job->worker_count : 1;
    if (nw > MAX_WORKERS)
        nw = MAX_WORKERS;
    pthread_t th[MAX_WORKERS];
    thread_context_t tc[MAX_WORKERS];
    job->current_index = 0;
    job->completed_count = 0;
    for (int i = 0; i < nw; i++) {
        tc[i].job_ctx = job;
        tc[i].thread_id = i + 1;
        pthread_create(&th[i], NULL, worker_thread, &tc[i]);
    }
    for (int i = 0; i < nw; i++)
        pthread_join(th[i], NULL);
}

static int gen_salt(uint8_t salt[SALT_SIZE])
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/urandom");
        return -1;
    }
    ssize_t n = read(fd, salt, SALT_SIZE);
    close(fd);
    if (n != SALT_SIZE) {
        fprintf(stderr, "cannot read salt\n");
        return -1;
    }
    return 0;
}

static long img_rem(FILE* img)
{
    long pos = ftell(img);
    long end;
    if (pos < 0)
        return -1;
    if (fseek(img, 0, SEEK_END) != 0)
        return -1;
    end = ftell(img);
    if (end < 0 || fseek(img, pos, SEEK_SET) != 0)
        return -1;
    return end - pos;
}

static int record_ok(uint32_t flen, uint32_t nlen, long rem)
{
    uint64_t need;
    if (rem < 0 || nlen > IMAGE_MAX_NAME_LEN)
        return 0;
    need = (uint64_t)SALT_SIZE + (uint64_t)nlen + (uint64_t)flen;
    return need <= (uint64_t)rem;
}

static int append_record(FILE* img, const char* name, uint32_t nlen,
                         const uint8_t salt[SALT_SIZE], const uint8_t* data, uint32_t dlen)
{
    uint32_t flen = dlen;
    if (nlen > IMAGE_MAX_NAME_LEN)
        return -1;
    pthread_mutex_lock(&image_mutex);
    if (fwrite(&flen, 4, 1, img) != 1 || fwrite(&nlen, 4, 1, img) != 1 ||
        fwrite(salt, 1, SALT_SIZE, img) != SALT_SIZE ||
        fwrite(name, 1, nlen, img) != nlen ||
        fwrite(data, 1, dlen, img) != dlen) {
        perror("fwrite image");
        pthread_mutex_unlock(&image_mutex);
        return -1;
    }
    fflush(img);
    pthread_mutex_unlock(&image_mutex);
    return 0;
}

static char* name_in_dir(const char* base, const char* full)
{
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        bl--;
    const char* rel = full;
    if (strncmp(full, base, bl) == 0) {
        rel = full + bl;
        while (*rel == '/')
            rel++;
    }
    size_t n = strlen(rel) + 2;
    char* out = malloc(n);
    if (!out)
        return NULL;
    snprintf(out, n, "/%s", rel);
    return out;
}

static char* name_file(const char* path)
{
    const char* b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t n = strlen(b) + 2;
    char* out = malloc(n);
    if (!out)
        return NULL;
    snprintf(out, n, "/%s", b);
    return out;
}

static int collect_dir(const char* base, const char* dir, image_file_t** items, int* n, int* cap)
{
    DIR* d = opendir(dir);
    if (!d) {
        perror(dir);
        return -1;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            perror(full);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            (void)collect_dir(base, full, items, n, cap);
        } else if (S_ISREG(st.st_mode)) {
            if (*n >= *cap) {
                fprintf(stderr, "too many files (max %d), skipping rest\n", *cap);
                break;
            }
            (*items)[*n].disk_path = strdup(full);
            (*items)[*n].image_name = name_in_dir(base, full);
            if (!(*items)[*n].disk_path || !(*items)[*n].image_name) {
                closedir(d);
                return -1;
            }
            (*n)++;
        }
    }
    closedir(d);
    return 0;
}

static int collect_arg(const char* arg, image_file_t** items, int* n, int* cap)
{
    struct stat st;
    if (stat(arg, &st) != 0) {
        perror(arg);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        char base[1024];
        snprintf(base, sizeof(base), "%s", arg);
        size_t len = strlen(base);
        while (len > 1 && base[len - 1] == '/')
            base[--len] = '\0';
        return collect_dir(base, base, items, n, cap);
    }
    if (*n >= *cap)
        return -1;
    (*items)[*n].disk_path = strdup(arg);
    (*items)[*n].image_name = name_file(arg);
    if (!(*items)[*n].disk_path || !(*items)[*n].image_name)
        return -1;
    (*n)++;
    return 0;
}

static int add_one_file_to_image(const char* disk_path, const char* image_name, FILE* img)
{
    size_t nl = strlen(image_name);
    if (nl > IMAGE_MAX_NAME_LEN) {
        fprintf(stderr, "file name too long: %s\n", image_name);
        return -1;
    }
    FILE* fin = fopen(disk_path, "rb");
    if (!fin) {
        perror(disk_path);
        return -1;
    }
    size_t sz = file_size(fin);
    uint8_t* plain = sz ? malloc(sz) : NULL;
    uint8_t* cipher = sz ? malloc(sz) : NULL;
    if (sz && (!plain || !cipher)) {
        perror("malloc");
        free(plain);
        free(cipher);
        fclose(fin);
        return -1;
    }
    if (sz && fread(plain, 1, sz, fin) != sz) {
        perror("fread");
        free(plain);
        free(cipher);
        fclose(fin);
        return -1;
    }
    fclose(fin);
    uint8_t salt[SALT_SIZE];
    if (gen_salt(salt) != 0) {
        free(plain);
        free(cipher);
        return -1;
    }
    if (sz)
        rc4_crypt(salt, plain, cipher, sz);
    int r = append_record(img, image_name, (uint32_t)nl, salt, sz ? cipher : (const uint8_t*)"", (uint32_t)sz);
    free(plain);
    free(cipher);
    return r;
}

/* 1=EOF, 0=ok, -1=err */
static int read_record(FILE* img, uint32_t* flen, uint32_t* nlen, uint8_t salt[SALT_SIZE], char** name)
{
    long rem;
    *name = NULL;
    rem = img_rem(img);
    if (rem < 0)
        return -1;
    if (rem == 0)
        return 1;
    if (rem < IMAGE_RECORD_MIN)
        return 1;
    if (rem < 8)
        return -1;
    if (fread(flen, 4, 1, img) != 1 || fread(nlen, 4, 1, img) != 1)
        return -1;
    rem = img_rem(img);
    if (rem < 0 || !record_ok(*flen, *nlen, rem))
        return -1;
    if (fread(salt, 1, SALT_SIZE, img) != SALT_SIZE)
        return -1;
    if (*nlen == 0) {
        *name = malloc(1);
        if (!*name)
            return -1;
        (*name)[0] = '\0';
        return 0;
    }
    *name = malloc((size_t)*nlen + 1);
    if (!*name)
        return -1;
    if (fread(*name, 1, *nlen, img) != *nlen) {
        free(*name);
        *name = NULL;
        return -1;
    }
    (*name)[*nlen] = '\0';
    return 0;
}

static int cmp_entry(const void* a, const void* b)
{
    return strcmp(((const list_entry_t*)a)->name, ((const list_entry_t*)b)->name);
}

static int cmd_add(int argc, char* argv[])
{
    const char *key = NULL, *image_path = NULL;
    int path_start = -1, i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-key") && i + 1 < argc)
            key = argv[++i];
        else if (!strcmp(argv[i], "-image") && i + 1 < argc)
            image_path = argv[++i];
        else if (path_start < 0)
            path_start = i;
    }
    if (!key || !image_path || path_start < 0) {
        fprintf(stderr, "Usage: %s -add -key \"secret\" -image disk.img file1 ...\n", argv[0]);
        return 1;
    }
    rc4_set_master_key(key, strlen(key));
    image_file_t* items = calloc(IMAGE_MAX_FILES, sizeof(image_file_t));
    if (!items) {
        perror("calloc");
        return 1;
    }
    int count = 0, cap = IMAGE_MAX_FILES;
    for (i = path_start; i < argc; i++) {
        int before = count;
        int cr = collect_arg(argv[i], &items, &count, &cap);
        if (cr != 0) {
            if (count == before)
                fprintf(stderr, "collect failed: %s\n", argv[i]);
            else
                fprintf(stderr, "collect warning: %s (some paths skipped)\n", argv[i]);
        }
    }
    if (count == 0) {
        fprintf(stderr, "no files to add\n");
        free(items);
        return 1;
    }
    if (count > MAX_FILES) {
        fprintf(stderr, "too many files (max %d)\n", MAX_FILES);
        for (i = 0; i < count; i++) {
            free(items[i].disk_path);
            free(items[i].image_name);
        }
        free(items);
        return 1;
    }
    FILE* img = fopen(image_path, "ab");
    if (!img) {
        perror(image_path);
        for (i = 0; i < count; i++) {
            free(items[i].disk_path);
            free(items[i].image_name);
        }
        free(items);
        return 1;
    }
    job_context_t job;
    job_zero(&job);
    job.file_count = count;
    job.image = img;
    job.worker_count = count < IMAGE_THREADS ? count : IMAGE_THREADS;
    for (i = 0; i < count; i++) {
        job.input_files[i] = items[i].disk_path;
        job.image_names[i] = items[i].image_name;
    }
    pthread_mutex_init(&job.mutex, NULL);
    process_parallel(&job);
    pthread_mutex_destroy(&job.mutex);
    fclose(img);
    for (i = 0; i < count; i++) {
        free(items[i].disk_path);
        free(items[i].image_name);
    }
    free(items);
    return job.image_errors ? 1 : 0;
}

static int cmd_list(int argc, char* argv[])
{
    const char* image_path = NULL;
    int i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-image") && i + 1 < argc)
            image_path = argv[++i];
    }
    if (!image_path) {
        fprintf(stderr, "Usage: %s -list -image disk.img\n", argv[0]);
        return 1;
    }
    FILE* img = fopen(image_path, "rb");
    if (!img) {
        perror(image_path);
        return 1;
    }
    list_entry_t* entries = NULL;
    int n = 0, cap = 0;
    for (;;) {
        uint32_t flen, nlen;
        uint8_t salt[SALT_SIZE];
        char* name = NULL;
        int rr = read_record(img, &flen, &nlen, salt, &name);
        if (rr == 1)
            break;
        if (rr != 0 || !name) {
            fprintf(stderr, "corrupt image\n");
            free(name);
            fclose(img);
            return 1;
        }
        (void)salt;
        if (fseek(img, (long)flen, SEEK_CUR) != 0) {
            free(name);
            fprintf(stderr, "corrupt image\n");
            fclose(img);
            return 1;
        }
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            list_entry_t* ne = realloc(entries, (size_t)cap * sizeof(list_entry_t));
            if (!ne) {
                free(name);
                perror("realloc");
                fclose(img);
                return 1;
            }
            entries = ne;
        }
        entries[n].name = name;
        entries[n].size = flen;
        n++;
    }
    if (img_rem(img) >= IMAGE_RECORD_MIN) {
        fprintf(stderr, "corrupt image\n");
        for (i = 0; i < n; i++)
            free(entries[i].name);
        free(entries);
        fclose(img);
        return 1;
    }
    fclose(img);
    if (n > 0)
        qsort(entries, (size_t)n, sizeof(list_entry_t), cmp_entry);
    for (i = 0; i < n; i++)
        printf("%s %u\n", entries[i].name, entries[i].size);
    for (i = 0; i < n; i++)
        free(entries[i].name);
    free(entries);
    return 0;
}

static int cmd_get(int argc, char* argv[])
{
    const char *key = NULL, *image_path = NULL, *out_path = NULL, *file_name = NULL;
    int i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-key") && i + 1 < argc)
            key = argv[++i];
        else if (!strcmp(argv[i], "-image") && i + 1 < argc)
            image_path = argv[++i];
        else if (!strcmp(argv[i], "-out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!file_name)
            file_name = argv[i];
    }
    if (!key || !image_path || !out_path || !file_name) {
        fprintf(stderr, "Usage: %s -get -image disk.img -key \"secret\" -out out.txt name\n", argv[0]);
        return 1;
    }
    rc4_set_master_key(key, strlen(key));
    FILE* img = fopen(image_path, "rb");
    if (!img) {
        perror(image_path);
        return 1;
    }
    for (;;) {
        uint32_t flen, nlen;
        uint8_t salt[SALT_SIZE];
        char* name = NULL;
        int rr = read_record(img, &flen, &nlen, salt, &name);
        if (rr == 1)
            break;
        if (rr != 0 || !name) {
            fprintf(stderr, "corrupt image\n");
            free(name);
            fclose(img);
            return 1;
        }
        if (!strcmp(name, file_name)) {
            long rem = img_rem(img);
            uint8_t *cipher = NULL, *plain = NULL;
            if (rem < 0 || (uint64_t)flen > (uint64_t)rem) {
                fprintf(stderr, "corrupt image\n");
                free(name);
                fclose(img);
                return 1;
            }
            cipher = malloc(flen ? flen : 1);
            plain = malloc(flen ? flen : 1);
            if (!cipher || !plain) {
                perror("malloc");
                free(name);
                free(cipher);
                free(plain);
                fclose(img);
                return 1;
            }
            if (fread(cipher, 1, flen, img) != flen) {
                fprintf(stderr, "corrupt image\n");
                free(name);
                free(cipher);
                free(plain);
                fclose(img);
                return 1;
            }
            rc4_crypt(salt, cipher, plain, flen);
            FILE* out = fopen(out_path, "wb");
            if (!out) {
                perror(out_path);
                free(name);
                free(cipher);
                free(plain);
                fclose(img);
                return 1;
            }
            if (flen > 0 && fwrite(plain, 1, flen, out) != flen) {
                perror("fwrite");
                fclose(out);
                free(name);
                free(cipher);
                free(plain);
                fclose(img);
                return 1;
            }
            fclose(out);
            free(name);
            free(cipher);
            free(plain);
            fclose(img);
            return 0;
        }
        free(name);
        if (fseek(img, (long)flen, SEEK_CUR) != 0) {
            fprintf(stderr, "corrupt image\n");
            fclose(img);
            return 1;
        }
    }
    fclose(img);
    fprintf(stderr, "file not found in image: %s\n", file_name);
    return 1;
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && strcmp(argv[1], "-add") == 0)
        return cmd_add(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "-list") == 0)
        return cmd_list(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "-get") == 0)
        return cmd_get(argc, argv);
    fprintf(stderr, "Usage: %s -add|-list|-get ...\n", argv[0]);
    return 1;
}
