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

// статистикс
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
    pthread_mutex_t mutex; // доступ к очереди
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
//новые
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

int queue_pop(queue_t* q, unsigned char* buffer, size_t* size) //потребитель извлечитель
{
    pthread_mutex_lock(&q->mutex); //захват мьютекса

    while (q->count == 0 && !q->finished && keep_running)
        pthread_cond_wait(&q->not_empty, &q->mutex); // освобождает мьютекс и ждет сигнал

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    queue_node_t* node = q->head;
    q->head = node->next;
    if (q->head == NULL)
        q->tail = NULL;
    q->count--;

    memcpy(buffer, node->data, node->size); // в буфер копи
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

// копи и шифр файла 
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
    
    fprintf(ctx->log_file, "[%s] Поток %d **** Файл: %s **** Результат: %s **** Время: %.3f сек\n",
            time_buffer, thread_id, filename, result, elapsed_time);
    fflush(ctx->log_file);
    
    pthread_mutex_unlock(&ctx->mutex);
}

char* get_next_file(job_context_t* ctx, int thread_id)
{
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

// рабочий поток
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
        
        // стата
        pthread_mutex_lock(&job_ctx->stats->mutex);
        int idx = job_ctx->stats->current_idx++;
        if (idx < job_ctx->stats->count) {
            job_ctx->stats->file_times[idx] = elapsed;
            job_ctx->stats->file_success[idx] = success ? 1 : 0;
            job_ctx->stats->file_names[idx] = strdup(base_name);
        }
        pthread_mutex_unlock(&job_ctx->stats->mutex);
        
        printf("[Поток %d] [%d/%d] %s: %.3f сек %s\n", 
               thread_id, job_ctx->completed_count + 1, job_ctx->file_count, 
               base_name, elapsed, success ? "+" : "-");
        
        log_operation(job_ctx, thread_id, filename, success ? "УСПЕХ" : "ОШИБКА", elapsed);
        increment_counter(job_ctx);
    }
    
    return NULL;
}

// последовательный
void process_sequential(job_context_t* job_ctx)
{
    printf("\nПОСЛЕДОВАТЕЛЬНЫЙ РЕЖИМ\n");
    
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
        
        // save
        pthread_mutex_lock(&job_ctx->stats->mutex);
        job_ctx->stats->file_times[i] = elapsed;
        job_ctx->stats->file_success[i] = success ? 1 : 0;
        job_ctx->stats->file_names[i] = strdup(base_name);
        pthread_mutex_unlock(&job_ctx->stats->mutex);
        
        printf("[%d/%d] %s: %.3f сек %s\n", i+1, job_ctx->file_count, base_name, elapsed, success ? "+" : "-");
        
        log_operation(job_ctx, 0, filename, success ? "УСПЕХ" : "ОШИБКА", elapsed);
        job_ctx->completed_count++;
    }
}

// параллельный
void process_parallel(job_context_t* job_ctx)
{
    printf("\nПАРАЛЛЕЛЬНЫЙ РЕЖИМ (%d threads)\n", WORKERS_COUNT);
    
    pthread_t workers[WORKERS_COUNT];
    thread_context_t tctx[WORKERS_COUNT];
    
    job_ctx->current_index = 0;
    job_ctx->completed_count = 0;
    
    // пул (1)
    for (int i = 0; i < WORKERS_COUNT; i++) {
        tctx[i].job_ctx = job_ctx;
        tctx[i].thread_id = i + 1;
        pthread_create(&workers[i], NULL, worker_thread, &tctx[i]);
    }
    
    // завершение
    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_join(workers[i], NULL);
    }
}

void print_statistics(statistics_t* stats, const char* mode_name, double real_time)
{
    double total_time = 0;
    int success_count = 0;
    
    for (int i = 0; i < stats->count; i++) {
        if (stats->file_success[i]) {
            total_time += stats->file_times[i];
            success_count++;
        }
    }
    
    double avg_time = (success_count > 0) ? total_time / success_count : 0;
    
    printf("СТАТИСТИКА (%s)\n", mode_name);
    printf("Общее время:               %10.3f сек\n", total_time);
    printf("Реальное время выполнения: %10.3f сек\n", real_time);
    printf("Среднее время на файл:     %10.3f сек\n", avg_time);
    printf("Обработано файлов:         %10d\n", stats->count);
    printf("Успешно:                   %10d\n", success_count);
}

void print_comparison(statistics_t* stats1, const char* name1, double time1,
                      statistics_t* stats2, const char* name2, double time2)
{
    double total1 = 0, total2 = 0;
    for (int i = 0; i < stats1->count; i++) {
        if (stats1->file_success[i]) total1 += stats1->file_times[i];
    }
    for (int i = 0; i < stats2->count; i++) {
        if (stats2->file_success[i]) total2 += stats2->file_times[i];
    }
    
    printf("СРАВНИТЕЛЬНАЯ ТАБЛИЦА\n");
    printf("│ Режим            │ Время (сек)   │ Среднее (сек)  │\n");
    
    double avg1 = (stats1->count > 0) ? total1 / stats1->count : 0;
    double avg2 = (stats2->count > 0) ? total2 / stats2->count : 0;
    
    printf("│ %-16s │ %13.3f │ %14.3f │\n", name1, time1, avg1);
    printf("│ %-16s │ %13.3f │ %14.3f │\n", name2, time2, avg2);
    
    if (time2 > 0) {
        double speedup = time1 / time2;
        printf("Ускорение: %s быстрее %s в %.2f раза\n", 
               (speedup > 1) ? name2 : name1,
               (speedup > 1) ? name1 : name2,
               (speedup > 1) ? speedup : 1.0/speedup);
    }
}

int main(int argc, char* argv[])
{
    int mode = -1;  // -1: auto, 0: seq, 1: parallel
    int file_start_index = 1;
    
    // parsin --mode 
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
    
    // auto
    if (mode == -1) {
        if (file_count < 5) {
            mode = 0;
            printf("\nauto: sequential\n");
            printf("(файлов: %d < 5)\n", file_count);
        } else {
            mode = 1;
            printf("\nauto: parallel\n");
            printf("(файлов: %d >= 5)\n", file_count);
        }
    }
    
    FILE* log_file = fopen("log.txt", "a");
    if (!log_file) {
        perror("Cannot open log.txt");
        return 1;
    }
    
    // stat initializing
    statistics_t selected_stats;
    selected_stats.count = file_count;
    selected_stats.current_idx = 0;
    selected_stats.file_times = malloc(file_count * sizeof(double));
    selected_stats.file_names = malloc(file_count * sizeof(char*));
    selected_stats.file_success = malloc(file_count * sizeof(int));
    pthread_mutex_init(&selected_stats.mutex, NULL);
    
    job_context_t job_ctx;
    job_ctx.output_dir = output_dir;
    job_ctx.file_count = file_count;
    job_ctx.current_index = 0;
    job_ctx.completed_count = 0;
    job_ctx.key = (uint8_t)key;
    job_ctx.log_file = log_file;
    job_ctx.stats = &selected_stats;
    
    for (int i = 0; i < file_count; i++) {
        job_ctx.input_files[i] = argv[file_start_index + i];
    }
    
    pthread_mutex_init(&job_ctx.mutex, NULL);
    
    struct timespec total_start, total_end;
    clock_gettime(CLOCK_MONOTONIC, &total_start);
    
    if (mode == 0) {
        process_sequential(&job_ctx);
    } else {
        process_parallel(&job_ctx);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &total_end);
    double total_elapsed = (total_end.tv_sec - total_start.tv_sec) + 
                           (total_end.tv_nsec - total_start.tv_nsec) / 1e9;
    
    printf("\n");
    print_statistics(&selected_stats, mode == 0 ? "sequential" : "parallel", total_elapsed);
    
    // cleanup
    for (int i = 0; i < selected_stats.count; i++) {
        if (selected_stats.file_names[i]) free(selected_stats.file_names[i]);
    }
    free(selected_stats.file_times);
    free(selected_stats.file_names);
    free(selected_stats.file_success);
    pthread_mutex_destroy(&selected_stats.mutex);
    
    pthread_mutex_destroy(&job_ctx.mutex);
    fclose(log_file);
    
    return 0;
}