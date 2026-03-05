#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "caesar.h"

#define BUFFER_SIZE 8192
#define QUEUE_SIZE 10

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
    context_t* ctx = arg;

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
    context_t* ctx = arg;

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

size_t get_file_size(FILE* f)
{
    long pos = ftell(f);

    fseek(f, 0, SEEK_END);
    long size = ftell(f);

    fseek(f, pos, SEEK_SET);

    return size > 0 ? size : 0;
}

int main(int argc, char* argv[])
{
    if (argc != 4) {
        fprintf(stderr,"Usage: %s input output key\n", argv[0]);
        return 1;
    }

    char* input = argv[1];
    char* output = argv[2];

    int key = atoi(argv[3]);

    if (key < 0 || key > 255) {
        fprintf(stderr,"Key must be 0..255\n");
        return 1;
    }

    FILE* fin = fopen(input,"rb");
    if (!fin) {
        perror("input file");
        return 1;
    }

    FILE* fout = fopen(output,"wb");
    if (!fout) {
        perror("output file");
        fclose(fin);
        return 1;
    }

    size_t total_size = get_file_size(fin);

    set_key((unsigned char)key);

    signal(SIGINT, handle_sigint);

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

    if (!keep_running)
        printf("\nОперация прервана пользователем\n");
    else
        printf("\nCompleted\n");

    return 0;
}