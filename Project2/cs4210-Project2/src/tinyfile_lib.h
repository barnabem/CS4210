#ifndef TINYFILE_LIB_H
#define TINYFILE_LIB_H

#include "tinyfile.h"

#define MAX_ASYNC_JOBS 16

typedef struct async_job async_job_t; // for circular dependency

typedef void (*compress_callback_t)(int status, const char *output_file, void *user_data);

typedef struct {
    int client_pid;
    shm_segment_t response_segment;
    response_queue_t *response_queue;
    int next_request_id;

    request_queue_t *service_req_queue;
    shm_segment_t *data_segments;
    int n_segments;
    size_t segment_size;
 
    async_job_t **async_jobs;
    int n_async_jobs;
    int max_async_jobs;
    pthread_mutex_t async_jobs_lock;
    pthread_mutex_t compress_lock;
} client_context_t;

struct async_job {
    pthread_t thread;
    client_context_t *ctx;
    char *input_file;
    char *output_file;
    compress_callback_t callback;
    void *user_data;
    int status;
    int job_id;
};

int tinyfile_compress();
int tinyfile_init(client_context_t **ctx_out);

int tinyfile_compress_sync(client_context_t *ctx, char *output_file, char *input_file);
int tinyfile_compress_async(client_context_t *ctx, char *output_file, char *input_file, compress_callback_t callback, void *user_data);

void cleanup_client_context(client_context_t *ctx);
void tinyfile_wait_all_async(client_context_t *ctx);

#endif