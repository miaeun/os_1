#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include "caesar.h"

#define BUFFER_SIZE 8192
#define QUEUE_SIZE 10
#define MAX_FILES 100
#define TIMEOUT_SEC 5
#define WORKERS_COUNT 4

volatile sig_atomic_t keep_running = 1;

typedef struct {
    double* file_times;
    char** file_names;
    int* file_success;
    int count;
    int current_idx;
    pthread_mutex_t mutex;
} statistics_t;

typedef struct queue_node {
    unsigned char* data;
    size_t size;
    struct queue_node* next;
} queue_node_t;

typedef struct {
    queue_node_t* head;
    queue_node_t* tail;
    int count;
    int max_size;
    int finished;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} queue_t;

typedef struct {
    FILE* fin;
    FILE* fout;
    queue_t* queue;
    size_t total_size;
    size_t processed;
} context_t;

typedef struct {
    char* input_files[MAX_FILES];
    char* output_dir;
    int file_count;
    int current_index;
    int completed_count;
    pthread_mutex_t mutex;
    FILE* log_file;
    uint8_t key;
    statistics_t* stats;
} job_context_t;

typedef struct {
    job_context_t* job_ctx;
    int thread_id;
} thread_context_t;

void queue_init(queue_t* q, int max_size);
int queue_push(queue_t* q, const unsigned char* data, size_t size);
int queue_pop(queue_t* q, unsigned char* buffer, size_t* size);
void queue_finish(queue_t* q);
void queue_clear(queue_t* q);
void queue_destroy(queue_t* q);
void handle_sigint(int sig);
void print_progress(size_t done, size_t total);
void* producer(void* arg);
void* consumer(void* arg);
void log_operation(job_context_t* ctx, int thread_id, const char* filename, const char* result, double elapsed_time);
char* get_next_file(job_context_t* ctx, int thread_id);
void increment_counter(job_context_t* ctx);
size_t get_file_size(FILE* f);
int copy_and_encrypt_file_simple(const char* input_path, const char* output_path, uint8_t key);
void* worker_thread(void* arg);
void process_sequential(job_context_t* job_ctx);
void process_parallel(job_context_t* job_ctx);
void print_statistics(statistics_t* stats, const char* mode_name, double real_time);
void print_comparison(statistics_t* stats1, const char* name1, double time1, 
                      statistics_t* stats2, const char* name2, double time2);

void queue_init(queue_t* q, int max_size)
{
    q->head = q->tail = NULL;
    q->count = 0;
    q->max_size = max_size;
    q->finished = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

int queue_push(queue_t* q, const unsigned char* data, size_t size)
{
    queue_node_t* node = malloc(sizeof(queue_node_t));
    if (!node) return -1;

    node->data = malloc(size);
    if (!node->data) {
        free(node);
        return -1;
    }

    memcpy(node->data, data, size);
    node->size = size;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);

    while (q->count >= q->max_size && keep_running)
        pthread_cond_wait(&q->not_full, &q->mutex);

    if (!keep_running) {
        pthread_mutex_unlock(&q->mutex);
        free(node->data);
        free(node);
        return -1;
    }

    if (q->tail == NULL)
        q->head = q->tail = node;
    else {
        q->tail->next = node;
        q->tail = node;
    }

    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int queue_pop(queue_t* q, unsigned char* buffer, size_t* size)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->finished && keep_running)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    queue_node_t* node = q->head;
    q->head = node->next;
    if (q->head == NULL)
        q->tail = NULL;
    q->count--;

    memcpy(buffer, node->data, node->size);
    *size = node->size;

    free(node->data);
    free(node);

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

void queue_finish(queue_t* q)
{
    pthread_mutex_lock(&q->mutex);
    q->finished = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}

void queue_clear(queue_t* q)
{
    pthread_mutex_lock(&q->mutex);
    while (q->head) {
        queue_node_t* tmp = q->head;
        q->head = q->head->next;
        free(tmp->data);
        free(tmp);
    }
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
}

void queue_destroy(queue_t* q)
{
    queue_clear(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
}

size_t get_file_size(FILE* f)
{
    long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, pos, SEEK_SET);
    return size > 0 ? size : 0;
}

void print_progress(size_t done, size_t total)
{
    if (total == 0) return;
    static struct timespec last = {0};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long diff = (now.tv_sec - last.tv_sec) * 1000 + 
                (now.tv_nsec - last.tv_nsec) / 1000000;
    if (diff < 100) return;
    last = now;
    size_t percent = (done * 100) / total;
    int bars = percent / 10;
    printf("\r[");
    for (int i = 0; i < 10; i++)
        printf("%c", i < bars ? '=' : ' ');
    printf("] %zu%%", percent);
    fflush(stdout);
}

void* producer(void* arg)
{
    context_t* ctx = (context_t*)arg;
    unsigned char temp[BUFFER_SIZE];
    unsigned char encrypted[BUFFER_SIZE];
    while (keep_running) {
        size_t bytes = fread(temp, 1, BUFFER_SIZE, ctx->fin);
        if (bytes == 0) {
            if (feof(ctx->fin)) break;
            perror("read error");
            keep_running = 0;
            break;
        }
        caesar(temp, encrypted, bytes);
        if (queue_push(ctx->queue, encrypted, bytes) != 0) break;
        if (bytes < BUFFER_SIZE) break;
    }
    queue_finish(ctx->queue);
    return NULL;
}

void* consumer(void* arg)
{
    context_t* ctx = (context_t*)arg;
    unsigned char buffer[BUFFER_SIZE];
    size_t bytes;
    while (keep_running) {
        if (queue_pop(ctx->queue, buffer, &bytes) != 0) break;
        if (fwrite(buffer, 1, bytes, ctx->fout) != bytes) {
            perror("write error");
            keep_running = 0;
            break;
        }
        ctx->processed += bytes;
        print_progress(ctx->processed, ctx->total_size);
    }
    return NULL;
}

int copy_and_encrypt_file_simple(const char* input_path, const char* output_path, uint8_t key)
{
    FILE* fin = fopen(input_path, "rb");
    if (!fin) return -1;
    
    FILE* fout = fopen(output_path, "wb");
    if (!fout) {
        fclose(fin);
        return -1;
    }
    
    unsigned char buffer[BUFFER_SIZE];
    unsigned char encrypted[BUFFER_SIZE];
    size_t bytes;
    int result = 0;
    
    while ((bytes = fread(buffer, 1, BUFFER_SIZE, fin)) > 0) {
        caesar(buffer, encrypted, bytes);
        if (fwrite(encrypted, 1, bytes, fout) != bytes) {
            result = -1;
            break;
        }
    }
    
    fclose(fin);
    fclose(fout);
    return result;
}

void log_operation(job_context_t* ctx, int thread_id, const char* filename, 
                   const char* result, double elapsed_time)
{
    if (!ctx->log_file) return;
    
    pthread_mutex_lock(&ctx->mutex);
    
    time_t rawtime;
    struct tm* timeinfo;
    char time_buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    fprintf(ctx->log_file, "[%s] thread %d **** file: %s **** result: %s **** time: %.3f s\n",
            time_buffer, thread_id, filename, result, elapsed_time);
    fflush(ctx->log_file);
    
    pthread_mutex_unlock(&ctx->mutex);
}

char* get_next_file(job_context_t* ctx, int thread_id)
{
    (void)thread_id;
    pthread_mutex_lock(&ctx->mutex);
    
    if (ctx->current_index >= ctx->file_count) {
        pthread_mutex_unlock(&ctx->mutex);
        return NULL;
    }
    
    char* filename = ctx->input_files[ctx->current_index];
    ctx->current_index++;
    
    pthread_mutex_unlock(&ctx->mutex);
    return filename;
}

void increment_counter(job_context_t* ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->completed_count++;
    pthread_mutex_unlock(&ctx->mutex);
}

void* worker_thread(void* arg)
{
    thread_context_t* tctx = (thread_context_t*)arg;
    job_context_t* job_ctx = tctx->job_ctx;
    int thread_id = tctx->thread_id;
    
    while (keep_running) {
        char* filename = get_next_file(job_ctx, thread_id);
        if (!filename) break;
        
        char input_path[256];
        char output_path[256];
        strcpy(input_path, filename);
        
        const char* base_name = strrchr(filename, '/');
        if (base_name == NULL) base_name = filename;
        else base_name++;
        
        snprintf(output_path, sizeof(output_path), "%s/%s", 
                 job_ctx->output_dir, base_name);
        
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        int success = (copy_and_encrypt_file_simple(input_path, output_path, job_ctx->key) == 0);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        
        if (job_ctx->stats != NULL) {
            pthread_mutex_lock(&job_ctx->stats->mutex);
            int idx = job_ctx->stats->current_idx++;
            if (idx < job_ctx->stats->count) {
                job_ctx->stats->file_times[idx] = elapsed;
                job_ctx->stats->file_success[idx] = success ? 1 : 0;
                job_ctx->stats->file_names[idx] = strdup(base_name);
            }
            pthread_mutex_unlock(&job_ctx->stats->mutex);
        }
        
        printf("[Thread %d] [%d/%d] %s: %.3f sec %s\n", 
               thread_id, job_ctx->completed_count + 1, job_ctx->file_count, 
               base_name, elapsed, success ? "+" : "-");
        
        log_operation(job_ctx, thread_id, filename, success ? "success" : "error", elapsed);
        increment_counter(job_ctx);
    }
    
    return NULL;
}

void process_sequential(job_context_t* job_ctx)
{
    printf("\nseq mode\n");
    
    for (int i = 0; i < job_ctx->file_count; i++) {
        char* filename = job_ctx->input_files[i];
        char output_path[256];
        const char* base_name = strrchr(filename, '/');
        if (base_name == NULL) base_name = filename;
        else base_name++;
        snprintf(output_path, sizeof(output_path), "%s/%s", job_ctx->output_dir, base_name);
        
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        int success = (copy_and_encrypt_file_simple(filename, output_path, job_ctx->key) == 0);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        
        if (job_ctx->stats != NULL) {
            pthread_mutex_lock(&job_ctx->stats->mutex);
            if (i < job_ctx->stats->count) {
                job_ctx->stats->file_times[i] = elapsed;
                job_ctx->stats->file_success[i] = success ? 1 : 0;
                job_ctx->stats->file_names[i] = strdup(base_name);
            }
            pthread_mutex_unlock(&job_ctx->stats->mutex);
        }
        
        printf("[%d/%d] %s: %.3f sec %s\n", i+1, job_ctx->file_count, base_name, elapsed, success ? "+" : "-");
        
        log_operation(job_ctx, 0, filename, success ? "success" : "error", elapsed);
        job_ctx->completed_count++;
    }
}

void process_parallel(job_context_t* job_ctx)
{
    printf("\nparallel mode (%d threads)\n", WORKERS_COUNT);
    
    pthread_t workers[WORKERS_COUNT];
    thread_context_t tctx[WORKERS_COUNT];
    
    job_ctx->current_index = 0;
    job_ctx->completed_count = 0;
    
    for (int i = 0; i < WORKERS_COUNT; i++) {
        tctx[i].job_ctx = job_ctx;
        tctx[i].thread_id = i + 1;
        pthread_create(&workers[i], NULL, worker_thread, &tctx[i]);
    }
    
    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_join(workers[i], NULL);
    }
}

void print_statistics(statistics_t* stats, const char* mode_name, double real_time)
{
    double total_time = 0;
    int success_count = 0;
    
    for (int i = 0; i < stats->count; i++) {
        total_time += stats->file_times[i];
        if (stats->file_success[i]) {
            success_count++;
        }
    }
    
    printf("\n stats (%s)\n", mode_name);
    printf("  all time:   %.3f s\n", total_time);
    printf("  real time: %.3f s\n", real_time);
    printf("  amount of files:         %d\n", stats->count);
    printf("  successfull:                   %d\n", success_count);
}

void print_comparison(statistics_t* stats1, const char* name1, double time1,
                      statistics_t* stats2, const char* name2, double time2)
{
    printf("\n| %-12s | %10s |\n", "mode", "time (s)");
    printf("|--------------|------------|\n");
    printf("| %-12s | %10.3f |\n", name1, time1);
    printf("| %-12s | %10.3f |\n", name2, time2);
    
    if (time2 > 0) {
        double speedup = time1 / time2;
        printf("\nAcceleration: %s faster %s for %.2f times\n", 
               (speedup > 1) ? name2 : name1,
               (speedup > 1) ? name1 : name2,
               (speedup > 1) ? speedup : 1.0/speedup);
    }
}

int main(int argc, char* argv[])
{
    int mode = -1;
    int file_start_index = 1;
    
    if (argc > 1 && strncmp(argv[1], "--mode=", 7) == 0) {
        if (strcmp(argv[1] + 7, "sequential") == 0) {
            mode = 0;
        } else if (strcmp(argv[1] + 7, "parallel") == 0) {
            mode = 1;
        } else {
            fprintf(stderr, "unknown mode, use --mode=sequential or --mode=parallel\n");
            return 1;
        }
        file_start_index = 2;
    }
    
    if (argc - file_start_index < 3) {
        fprintf(stderr, "Usage: %s [--mode=sequential|parallel] file1 file2 ... output_dir key\n", argv[0]);
        return 1;
    }
    
    int key = atoi(argv[argc - 1]);
    if (key < 0 || key > 255) {
        fprintf(stderr, "Key must be 0..255\n");
        return 1;
    }
    
    char* output_dir = argv[argc - 2];
    int file_count = argc - file_start_index - 2;
    
    if (file_count < 1) {
        fprintf(stderr, "Need at least one input file\n");
        return 1;
    }
    
    struct stat st = {0};
    if (stat(output_dir, &st) == -1) {
        mkdir(output_dir, 0700);
    }
    
    set_key((unsigned char)key);
    signal(SIGINT, handle_sigint);
    
    int is_auto = (mode == -1);
    
    if (is_auto) {
        if (file_count < 5) {
            printf("\nAuto: sequential recommended (files: %d < 5)\n", file_count);
        } else {
            printf("\nAuto: parallel recommended (files: %d >= 5)\n", file_count);
        }
    }
    
    FILE* log_file = fopen("log.txt", "a");
    if (!log_file) {
        perror("Cannot open log.txt");
        return 1;
    }
    
    statistics_t seq_stats;
    statistics_t par_stats;
    
    seq_stats.count = file_count;
    seq_stats.current_idx = 0;
    seq_stats.file_times = malloc(file_count * sizeof(double));
    seq_stats.file_names = malloc(file_count * sizeof(char*));
    seq_stats.file_success = malloc(file_count * sizeof(int));
    
    par_stats.count = file_count;
    par_stats.current_idx = 0;
    par_stats.file_times = malloc(file_count * sizeof(double));
    par_stats.file_names = malloc(file_count * sizeof(char*));
    par_stats.file_success = malloc(file_count * sizeof(int));
    
    if (!seq_stats.file_times || !seq_stats.file_names || !seq_stats.file_success ||
        !par_stats.file_times || !par_stats.file_names || !par_stats.file_success) {
        perror("malloc failed");
        fclose(log_file);
        return 1;
    }
    
    pthread_mutex_init(&seq_stats.mutex, NULL);
    pthread_mutex_init(&par_stats.mutex, NULL);
    
    if (is_auto) {
        printf("\nwarm-up\n");
        
        char warm_dir[] = "/tmp/caesar_warmup_XXXXXX";
        if (mkdtemp(warm_dir)) {
            job_context_t warm_seq;
            warm_seq.output_dir = warm_dir;
            warm_seq.file_count = file_count;
            warm_seq.current_index = 0;
            warm_seq.completed_count = 0;
            warm_seq.key = (uint8_t)key;
            warm_seq.log_file = fopen("/dev/null", "w");
            warm_seq.stats = NULL;
            
            for (int i = 0; i < file_count; i++) {
                warm_seq.input_files[i] = argv[file_start_index + i];
            }
            pthread_mutex_init(&warm_seq.mutex, NULL);
            
            process_sequential(&warm_seq);
            
            for (int i = 0; i < file_count; i++) {
                char path[512];
                const char* base_name = strrchr(argv[file_start_index + i], '/');
                if (base_name == NULL) base_name = argv[file_start_index + i];
                else base_name++;
                snprintf(path, sizeof(path), "%s/%s", warm_dir, base_name);
                unlink(path);
            }
            
            job_context_t warm_par;
            warm_par.output_dir = warm_dir;
            warm_par.file_count = file_count;
            warm_par.current_index = 0;
            warm_par.completed_count = 0;
            warm_par.key = (uint8_t)key;
            warm_par.log_file = fopen("/dev/null", "w");
            warm_par.stats = NULL;
            
            for (int i = 0; i < file_count; i++) {
                warm_par.input_files[i] = argv[file_start_index + i];
            }
            pthread_mutex_init(&warm_par.mutex, NULL);
            
            process_parallel(&warm_par);
            
            for (int i = 0; i < file_count; i++) {
                char path[512];
                const char* base_name = strrchr(argv[file_start_index + i], '/');
                if (base_name == NULL) base_name = argv[file_start_index + i];
                else base_name++;
                snprintf(path, sizeof(path), "%s/%s", warm_dir, base_name);
                unlink(path);
            }
            
            rmdir(warm_dir);
            
            if (warm_seq.log_file) fclose(warm_seq.log_file);
            if (warm_par.log_file) fclose(warm_par.log_file);
            pthread_mutex_destroy(&warm_seq.mutex);
            pthread_mutex_destroy(&warm_par.mutex);
            
            printf("Warm-up complete. Starting real measurements...\n");
        } else {
            printf("Warning: Could not create warmup directory, continuing without warmup\n");
        }
        
        seq_stats.current_idx = 0;
        par_stats.current_idx = 0;
        for (int i = 0; i < file_count; i++) {
            seq_stats.file_times[i] = 0;
            seq_stats.file_success[i] = 0;
            if (seq_stats.file_names[i]) {
                free(seq_stats.file_names[i]);
                seq_stats.file_names[i] = NULL;
            }
            par_stats.file_times[i] = 0;
            par_stats.file_success[i] = 0;
            if (par_stats.file_names[i]) {
                free(par_stats.file_names[i]);
                par_stats.file_names[i] = NULL;
            }
        }
    }
    
    printf("\nSEQUENTIAL\n");
    
    char seq_output_dir[256];
    snprintf(seq_output_dir, sizeof(seq_output_dir), "%s_seq", output_dir);
    if (stat(seq_output_dir, &st) == -1) {
        mkdir(seq_output_dir, 0700);
    }
    
    job_context_t seq_ctx;
    seq_ctx.output_dir = seq_output_dir;
    seq_ctx.file_count = file_count;
    seq_ctx.current_index = 0;
    seq_ctx.completed_count = 0;
    seq_ctx.key = (uint8_t)key;
    seq_ctx.log_file = fopen("/dev/null", "w");
    seq_ctx.stats = &seq_stats;
    
    for (int i = 0; i < file_count; i++) {
        seq_ctx.input_files[i] = argv[file_start_index + i];
    }
    pthread_mutex_init(&seq_ctx.mutex, NULL);
    
    struct timespec seq_start, seq_end;
    clock_gettime(CLOCK_MONOTONIC, &seq_start);
    process_sequential(&seq_ctx);
    clock_gettime(CLOCK_MONOTONIC, &seq_end);
    double seq_elapsed = (seq_end.tv_sec - seq_start.tv_sec) + 
                         (seq_end.tv_nsec - seq_start.tv_nsec) / 1e9;
    
    print_statistics(&seq_stats, "sequential", seq_elapsed);
    
    printf("\nPARALLEL\n");
    
    char par_output_dir[256];
    snprintf(par_output_dir, sizeof(par_output_dir), "%s_par", output_dir);
    if (stat(par_output_dir, &st) == -1) {
        mkdir(par_output_dir, 0700);
    }
    
    job_context_t par_ctx;
    par_ctx.output_dir = par_output_dir;
    par_ctx.file_count = file_count;
    par_ctx.current_index = 0;
    par_ctx.completed_count = 0;
    par_ctx.key = (uint8_t)key;
    par_ctx.log_file = fopen("/dev/null", "w");
    par_ctx.stats = &par_stats;
    
    for (int i = 0; i < file_count; i++) {
        par_ctx.input_files[i] = argv[file_start_index + i];
    }
    pthread_mutex_init(&par_ctx.mutex, NULL);
    
    struct timespec par_start, par_end;
    clock_gettime(CLOCK_MONOTONIC, &par_start);
    process_parallel(&par_ctx);
    clock_gettime(CLOCK_MONOTONIC, &par_end);
    double par_elapsed = (par_end.tv_sec - par_start.tv_sec) + 
                         (par_end.tv_nsec - par_start.tv_nsec) / 1e9;
    
    print_statistics(&par_stats, "parallel", par_elapsed);
    
    printf("\nCOMPARISON\n");
    print_comparison(&seq_stats, "sequential", seq_elapsed,
                    &par_stats, "parallel", par_elapsed);
    
    for (int i = 0; i < seq_stats.count; i++) {
        if (seq_stats.file_names[i]) free(seq_stats.file_names[i]);
    }
    free(seq_stats.file_times);
    free(seq_stats.file_names);
    free(seq_stats.file_success);
    pthread_mutex_destroy(&seq_stats.mutex);
    pthread_mutex_destroy(&seq_ctx.mutex);
    if (seq_ctx.log_file) fclose(seq_ctx.log_file);
    
    for (int i = 0; i < par_stats.count; i++) {
        if (par_stats.file_names[i]) free(par_stats.file_names[i]);
    }
    free(par_stats.file_times);
    free(par_stats.file_names);
    free(par_stats.file_success);
    pthread_mutex_destroy(&par_stats.mutex);
    pthread_mutex_destroy(&par_ctx.mutex);
    if (par_ctx.log_file) fclose(par_ctx.log_file);
    
    fclose(log_file);
    
    return 0;
}