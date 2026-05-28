#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "caesar.h"

#define IMAGE_RECORD_HDR (8 + SALT_SIZE)
#define IMAGE_THREADS 5
#define SALT_SIZE 16
#define IMAGE_MAX_FILES 4096
#define IMAGE_MAX_NAME_LEN 65536
#define IMAGE_RECORD_MIN (8 + SALT_SIZE)
#define MAX_FILES 100
#define MAX_WORKERS 5
#define IO_CHUNK_SIZE (4 * 1024 * 1024)

typedef struct {
    char* input_files[MAX_FILES];
    char* image_names[MAX_FILES];
    off_t record_off[MAX_FILES];
    int file_count;
    int current_index;
    int completed_count;
    pthread_mutex_t mutex;
    int image_fd;
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

static int add_one_file_to_image(const char* disk_path, const char* image_name,
                                 job_context_t* job, int idx);
static int prepare_image_layout(job_context_t* job);

static void job_zero(job_context_t* job)
{
    memset(job, 0, sizeof(*job));
}

static int file_size_u64(const char* path, uint64_t* out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode))
        return -1;
    *out = (uint64_t)st.st_size;
    return 0;
}

static int take_next_job(job_context_t* ctx, int* idx_out, char** disk_path, char** image_name)
{
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->current_index < ctx->file_count) {
        int idx = ctx->current_index++;
        *idx_out = idx;
        *disk_path = ctx->input_files[idx];
        *image_name = ctx->image_names[idx];
        pthread_mutex_unlock(&ctx->mutex);
        return 0;
    }
    pthread_mutex_unlock(&ctx->mutex);
    return -1;
}

static void set_image_error(job_context_t* ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->image_errors = 1;
    pthread_mutex_unlock(&ctx->mutex);
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
        int idx = 0;
        char *path = NULL, *iname = NULL;
        if (take_next_job(job, &idx, &path, &iname) != 0)
            break;
        if (add_one_file_to_image(path, iname, job, idx) != 0)
            set_image_error(job);
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

static off_t img_rem(FILE* img) // off_t для больших файлов
{
    off_t pos = ftello(img);
    off_t end;
    if (pos < 0)
        return -1;
    if (fseeko(img, 0, SEEK_END) != 0)
        return -1;
    end = ftello(img);
    if (end < 0 || fseeko(img, pos, SEEK_SET) != 0)
        return -1;
    return end - pos;
}

static int record_ok(uint32_t flen, uint32_t nlen, off_t rem)
{
    uint64_t need;
    if (rem < 0 || nlen > IMAGE_MAX_NAME_LEN)
        return 0;
    need = (uint64_t)SALT_SIZE + (uint64_t)nlen + (uint64_t)flen;
    return need <= (uint64_t)rem;
}

static uint64_t record_byte_size(uint32_t nlen, uint32_t dlen)
{
    return (uint64_t)IMAGE_RECORD_HDR + (uint64_t)nlen + (uint64_t)dlen;
}

static void write_record_header_mem(void* base, const char* name, uint32_t nlen,
                                    const uint8_t salt[SALT_SIZE], uint32_t dlen)
{
    uint32_t flen = dlen;
    uint8_t* p = (uint8_t*)base;
    memcpy(p, &flen, 4);
    memcpy(p + 4, &nlen, 4);
    memcpy(p + 8, salt, SALT_SIZE);
    memcpy(p + IMAGE_RECORD_HDR, name, nlen);
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

static int prepare_image_layout(job_context_t* job)
{
    off_t base;              
    off_t end_off;           
    uint64_t add_bytes = 0; 
    int i;

    // размер файла щас
    base = lseek(job->image_fd, 0, SEEK_END);
    if (base < 0) {
        perror("lseek image");
        return -1;
    }

    // проходим по всем файлам и вычисляем для каждого смещение
    for (i = 0; i < job->file_count; i++) {
        uint64_t sz64;
        uint32_t nlen;
        uint32_t dlen;
        nlen = (uint32_t)strlen(job->image_names[i]);
        if (nlen > IMAGE_MAX_NAME_LEN) {
            fprintf(stderr, "file name too long: %s\n", job->image_names[i]);
            return -1;
        }
        if (file_size_u64(job->input_files[i], &sz64) != 0) {
            perror(job->input_files[i]);
            return -1;
        }
        if (sz64 > UINT32_MAX) {
            fprintf(stderr, "file too large (max 4 GiB): %s\n", job->input_files[i]);
            return -1;
        }
        dlen = (uint32_t)sz64;

        // СОХРАНЯЕМ СМЕЩЕНИЕ ЗАПИСИ ДЛЯ ЭТОГО ФАЙЛА
        job->record_off[i] = base + (off_t)add_bytes;

        // НАКАПЛИВАЕМ ОБЩИЙ РАЗМЕР
        add_bytes += record_byte_size(nlen, dlen);
    }

    if (add_bytes > SIZE_MAX) {
        fprintf(stderr, "image batch too large\n");
        return -1;
    }

    end_off = base + (off_t)add_bytes;
    // РАСШИРЯЕМ ФАЙЛ ДО НОВОГО РАЗМЕРА
    if (ftruncate(job->image_fd, end_off) != 0) {
        perror("ftruncate image");
        return -1;
    }

    return 0;
}

static long page_size_cached(void)
{
    static long psz = 0;
    if (psz <= 0)
        psz = sysconf(_SC_PAGESIZE);
    return psz;
}

static void* mmap_window(int fd, off_t file_off, size_t len, void** map_base, size_t* map_len_out)
{
    long psz = page_size_cached();
    off_t map_off;
    size_t shift, map_len;
    void* map;
    if (psz <= 0 || len == 0)
        return MAP_FAILED;
    map_off = file_off & ~(off_t)(psz - 1);
    shift = (size_t)(file_off - map_off);
    map_len = shift + len;
    if (map_len % (size_t)psz)
        map_len += (size_t)psz - (map_len % (size_t)psz);
    map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_off);
    if (map == MAP_FAILED)
        return map;
    *map_base = map;
    *map_len_out = map_len;
    return (uint8_t*)map + shift;
}

static int mmap_write_header(int fd, off_t rec_off, const char* name, uint32_t nlen,
                             const uint8_t salt[SALT_SIZE], uint32_t dlen)
{
    size_t hdr_len = (size_t)IMAGE_RECORD_HDR + (size_t)nlen;
    size_t map_len = 0;
    void* map_base = NULL;
    void* rec = mmap_window(fd, rec_off, hdr_len, &map_base, &map_len);

    if (rec == MAP_FAILED) {
        perror("mmap header");
        return -1;
    }
    write_record_header_mem(rec, name, nlen, salt, dlen);
    if (msync(map_base, map_len, MS_SYNC) != 0)
        perror("msync header");
    if (munmap(map_base, map_len) != 0) {
        perror("munmap header");
        return -1;
    }
    return 0;
}

//окна в 4 мб
static int encrypt_into_image(int fd, off_t data_off, uint32_t dlen, const uint8_t salt[SALT_SIZE],
                              FILE* fin, uint8_t* buf)
{
    rc4_stream_t* stream = rc4_stream_create();
    uint64_t left = dlen;
    uint64_t done = 0;

    if (!stream)
        return -1;
    if (dlen == 0) {
        rc4_stream_destroy(stream);
        return 0;
    }
    rc4_stream_begin(stream, salt); //иниц
    while (left > 0) {
        size_t chunk = left > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : (size_t)left;
        off_t pos = data_off + (off_t)done;
        size_t map_len = 0;
        void* map_base = NULL;
        void* dst;

        if (fread(buf, 1, chunk, fin) != chunk) { //fin исх файл на диске
            perror("fread");
            rc4_stream_destroy(stream);
            return -1;
        }
        dst = mmap_window(fd, pos, chunk, &map_base, &map_len);
        if (dst == MAP_FAILED) {
            perror("mmap chunk");
            rc4_stream_destroy(stream);
            return -1;
        }
        rc4_stream_crypt(stream, buf, (uint8_t*)dst, chunk);
        if (msync(map_base, map_len, MS_SYNC) != 0)
            perror("msync chunk");
        if (munmap(map_base, map_len) != 0) {
            perror("munmap chunk");
            rc4_stream_destroy(stream);
            return -1;
        }
        done += chunk;
        left -= chunk;
    }
    rc4_stream_destroy(stream);
    return 0;
}

static int add_one_file_to_image(const char* disk_path, const char* image_name,
                                 job_context_t* job, int idx)
{
    uint64_t sz64;
    uint32_t dlen;
    uint32_t nlen = (uint32_t)strlen(image_name);
    uint8_t salt[SALT_SIZE];
    uint8_t* buf = NULL;
    FILE* fin = NULL;
    off_t rec_off;
    off_t data_off;
    if (nlen > IMAGE_MAX_NAME_LEN) {
        fprintf(stderr, "file name too long: %s\n", image_name);
        return -1;
    } 
    if (file_size_u64(disk_path, &sz64) != 0) { // получение размера файла
        perror(disk_path);
        return -1;
    }
    if (sz64 > UINT32_MAX) { // проверка на максимальный размер файла
        fprintf(stderr, "file too large (max 4 GiB): %s\n", disk_path);
        return -1;
    }
    dlen = (uint32_t)sz64;
    rec_off = job->record_off[idx];
    data_off = rec_off + (off_t)IMAGE_RECORD_HDR + (off_t)nlen;
    fin = fopen(disk_path, "rb");
    if (!fin) {
        perror(disk_path);
        return -1;
    }
    if (dlen > 0) { // выделение буфера для чтения (если файл не пустой)
        buf = malloc(IO_CHUNK_SIZE);
        if (!buf) { // проверка на ошибку выделения памяти
            perror("malloc");
            fclose(fin);
            return -1;
        }
    }
    if (gen_salt(salt) != 0) { // генерация соли
        free(buf);
        fclose(fin);
        return -1;
    }
    if (mmap_write_header(job->image_fd, rec_off, image_name, nlen, salt, dlen) != 0) {
        free(buf);
        fclose(fin);
        return -1;
    }
    if (encrypt_into_image(job->image_fd, data_off, dlen, salt, fin, buf) != 0) {
        free(buf);
        fclose(fin);
        return -1;
    }
    fclose(fin);
    free(buf);
    return 0;
}

/* 1=EOF, 0=ok, -1=err */
static int read_record(FILE* img, uint32_t* flen, uint32_t* nlen, uint8_t salt[SALT_SIZE], char** name)
{
    off_t rem;
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
    
    int img_fd = open(image_path, O_RDWR | O_CREAT, 0644);
    if (img_fd < 0) {
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
    job.image_fd = img_fd;
    job.worker_count = count < IMAGE_THREADS ? count : IMAGE_THREADS;
    for (i = 0; i < count; i++) {
        job.input_files[i] = items[i].disk_path;
        job.image_names[i] = items[i].image_name;
    }
    pthread_mutex_init(&job.mutex, NULL);
    if (prepare_image_layout(&job) != 0) {
        pthread_mutex_destroy(&job.mutex);
        close(img_fd);
        for (i = 0; i < count; i++) {
            free(items[i].disk_path);
            free(items[i].image_name);
        }
        free(items);
        return 1;
    }
    process_parallel(&job)
    pthread_mutex_destroy(&job.mutex);
    close(img_fd);
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
        if (fseeko(img, (off_t)flen, SEEK_CUR) != 0) {
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
            off_t rem = img_rem(img);
            uint8_t* buf = NULL;
            rc4_stream_t* stream = NULL;
            FILE* out = NULL;
            uint64_t left;
            int ok = 0;
            if (rem < 0 || (uint64_t)flen > (uint64_t)rem) {
                fprintf(stderr, "corrupt image\n");
                free(name);
                fclose(img);
                return 1;
            }
            if (flen > 0) {
                buf = malloc(IO_CHUNK_SIZE);
                if (!buf) {
                    perror("malloc");
                    free(name);
                    fclose(img);
                    return 1;
                }
            }
            out = fopen(out_path, "wb");
            if (!out) {
                perror(out_path);
                free(name);
                free(buf);
                fclose(img);
                return 1;
            }
            stream = rc4_stream_create();
            if (!stream) {
                free(name);
                free(buf);
                fclose(out);
                fclose(img);
                return 1;
            }
            rc4_stream_begin(stream, salt);
            left = flen;
            while (left > 0) {
                size_t chunk = left > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : (size_t)left;
                if (fread(buf, 1, chunk, img) != chunk) {
                    fprintf(stderr, "corrupt image\n");
                    ok = -1;
                    break;
                }
                rc4_stream_crypt(stream, buf, buf, chunk);
                if (fwrite(buf, 1, chunk, out) != chunk) {
                    perror("fwrite");
                    ok = -1;
                    break;
                }
                left -= chunk;
            }
            rc4_stream_destroy(stream);
            fclose(out);
            free(name);
            free(buf);
            fclose(img);
            return ok == 0 ? 0 : 1;
        }
        free(name);
        if (fseeko(img, (off_t)flen, SEEK_CUR) != 0) {
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
