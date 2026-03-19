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

volatile sig_atomic_t keep_running = 1;

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
void increment_counter(job_context_t* ctx, int thread_id);
size_t get_file_size(FILE* f);
void* file_processor(void* arg);

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
    long diff = 
        (now.tv_sec - last.tv_sec) * 1000 + 
        (now.tv_nsec - last.tv_nsec) / 1000000;
    if (diff < 100)
     return;
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
            if (feof(ctx->fin)) 
                break;
            perror("read error");
            keep_running = 0;
            break;
        }
        caesar(temp, encrypted, bytes);
        if (queue_push(ctx->queue, encrypted, bytes) != 0) 
            break;
        if (bytes < BUFFER_SIZE) 
            break;
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
        if (queue_pop(ctx->queue, buffer, &bytes) != 0) 
            break;
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

void log_operation(job_context_t* ctx, int thread_id, const char* filename, 
                   const char* result, double elapsed_time) // логирование операции с файлом
{
    if (!ctx->log_file) return;
    
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += TIMEOUT_SEC;
    
    int lock_result = pthread_mutex_timedlock(&ctx->mutex, &timeout);
    if (lock_result == ETIMEDOUT) {
        fprintf(stderr, "возможная взаимоблокировка: поток %d ожидает мьютекс более %d секунд\n", 
                thread_id, TIMEOUT_SEC);
        return;
    }
    
    time_t rawtime;
    struct tm* timeinfo;
    char time_buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    fprintf(ctx->log_file, "[%s] Поток %d **** Файл: %s **** Результат: %s **** Время: %.3f сек\n",
            time_buffer, thread_id, filename, result, elapsed_time); //запись в лог
    fflush(ctx->log_file);
    
    pthread_mutex_unlock(&ctx->mutex);
}

char* get_next_file(job_context_t* ctx, int thread_id)
{
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += TIMEOUT_SEC;
    
    int lock_result = pthread_mutex_timedlock(&ctx->mutex, &timeout);
    if (lock_result == ETIMEDOUT) {
        fprintf(stderr, "возможная взаимоблокировка: поток %d ожидает мьютекс более %d секунд\n", 
                thread_id, TIMEOUT_SEC);
        return NULL;
    }
    
    if (ctx->current_index >= ctx->file_count) {
        pthread_mutex_unlock(&ctx->mutex);
        return NULL;
    }
    
    char* filename = ctx->input_files[ctx->current_index];
    ctx->current_index++;
    
    pthread_mutex_unlock(&ctx->mutex);
    return filename;
}

void increment_counter(job_context_t* ctx, int thread_id)
{
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += TIMEOUT_SEC;
    
    int lock_result = pthread_mutex_timedlock(&ctx->mutex, &timeout);
    if (lock_result == ETIMEDOUT) {
        fprintf(stderr, "возможная взаимоблокировка: поток %d ожидает мьютекс более %d секунд\n", 
                thread_id, TIMEOUT_SEC);
        return;
    }
    
    ctx->completed_count++;
    
    pthread_mutex_unlock(&ctx->mutex);
}

void* file_processor(void* arg)
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
        
        FILE* fin = fopen(input_path, "rb");
        if (!fin) {
            clock_gettime(CLOCK_MONOTONIC, &end);
            double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
            log_operation(job_ctx, thread_id, filename, "ОШИБКА (открытие входа)", elapsed);
            continue;
        }
        
        FILE* fout = fopen(output_path, "wb");
        if (!fout) {
            clock_gettime(CLOCK_MONOTONIC, &end);
            double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
            log_operation(job_ctx, thread_id, filename, "ОШИБКА (открытие выхода)", elapsed);
            fclose(fin);
            continue;
        }
        
        size_t total_size = get_file_size(fin);
        
        queue_t queue;
        queue_init(&queue, QUEUE_SIZE);
        
        context_t ctx = {
            .fin = fin,
            .fout = fout,
            .queue = &queue,
            .total_size = total_size,
            .processed = 0
        };
        
        pthread_t producer_thread;
        pthread_t consumer_thread;
        
        pthread_create(&producer_thread,NULL,producer,&ctx);
        pthread_create(&consumer_thread,NULL,consumer,&ctx);
        pthread_join(producer_thread,NULL);
        pthread_join(consumer_thread,NULL);
        queue_destroy(&queue);
        fclose(fin);
        fclose(fout);
        increment_counter(job_ctx, thread_id);
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        log_operation(job_ctx, thread_id, filename, "УСПЕХ", elapsed);
    }
    
    return NULL;
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s file1.txt file2.txt ... output_dir/ key\n", argv[0]);
        return 1;
    }
    
    int key = atoi(argv[argc - 1]);
    if (key < 0 || key > 255) {
        fprintf(stderr, "Key must be 0..255\n");
        return 1;
    }
    
    char* output_dir = argv[argc - 2];
    int file_count = argc - 3;
    
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
    
    FILE* log_file = fopen("log.txt", "a");
    if (!log_file) {
        perror("Cannot open log.txt");
        return 1;
    }
    
    job_context_t job_ctx;
    job_ctx.output_dir = output_dir;
    job_ctx.file_count = file_count;
    job_ctx.current_index = 0;
    job_ctx.completed_count = 0;
    job_ctx.key = key;
    job_ctx.log_file = log_file;
    
    for (int i = 0; i < file_count; i++) {
        job_ctx.input_files[i] = argv[i + 1];
    }
    
    pthread_mutex_init(&job_ctx.mutex, NULL);
    
    pthread_t threads[3];
    thread_context_t tctx[3];
    
    for (int i = 0; i < 3; i++) {
        tctx[i].job_ctx = &job_ctx;
        tctx[i].thread_id = i + 1;
        pthread_create(&threads[i], NULL, file_processor, &tctx[i]);
    }
    
    for (int i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("\nобработано файлов: %d из %d\n", job_ctx.completed_count, file_count);
    
    pthread_mutex_destroy(&job_ctx.mutex);
    fclose(log_file);
    
    return 0;
}