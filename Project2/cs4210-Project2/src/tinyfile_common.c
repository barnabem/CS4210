#include "tinyfile.h"
#include <stdio.h>

int enqueue_request(request_queue_t *queue, request_t *req) {
    pthread_mutex_lock(&queue->mutex);
    // printf("Check queue capacity...\n");
    // wait for thread queue to not be full (queue->count >= queue->capacity) 
    while (queue->count >= queue->capacity) {
        printf("Queue full, waiting... (count: %d, capacity: %d)\n", 
               queue->count, queue->capacity);
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
    // printf("Room in queue (cnt: %d, cap: %d), adding request...\n",queue->count, queue->capacity);
    // Add to queue (queue->requests[queue->tail] = *reques)
    queue->requests[queue->tail] = *req;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    queue->total_requests++;

    // printf("Request added, enqueue done, signal and unlock.\n");
    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);
    return 0;
}