#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "tinyfile.h"
#include <pthread.h>
#include "snappy.h"
#include <signal.h>

#define NUM_THREADS 4

int debug_foreground = 0; // default val for running in foreground mode
tinyfile_service_t *tinyfile_service;
volatile sig_atomic_t keep_running = 1;

#define MAX_CLIENTS 32
static compress_job_t client_states[MAX_CLIENTS]; // should this staty here or go in tinyfile_service?
// static int curr_job_id = 0;
static pthread_mutex_t client_states_lock = PTHREAD_MUTEX_INITIALIZER;

static compress_job_t* get_client_state(int client_pid) {
    pthread_mutex_lock(&client_states_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_states[i].client_pid == client_pid) {
            pthread_mutex_unlock(&client_states_lock);
            return &client_states[i];
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_states[i].client_pid == 0) {
            client_states[i].client_pid = client_pid;
            pthread_mutex_unlock(&client_states_lock);
            return &client_states[i];
        }
    }
    
    pthread_mutex_unlock(&client_states_lock);
    return NULL; 
}

static void clear_client_state(int client_pid) {
    pthread_mutex_lock(&client_states_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_states[i].client_pid == client_pid) {
            if (client_states[i].output_buf) {
                free(client_states[i].output_buf);
            }
            if (client_states[i].input_buf) {
                free(client_states[i].input_buf);
            }
            memset(&client_states[i], 0, sizeof(compress_job_t));
            break;
        }
    }
    pthread_mutex_unlock(&client_states_lock);
}

void handle_signal() {
    keep_running = 0;
    if (tinyfile_service) {
        tinyfile_service->running = 0;
    }
}

static int init_request_queue(request_queue_t *queue, int capacity) {
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->total_requests = 0;
    queue->completed_requests = 0;

    /*=================================================================*/
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&queue->mutex, &mutex_attr);
    
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&queue->not_empty, &cond_attr);
    pthread_cond_init(&queue->not_full, &cond_attr);
    
    pthread_mutexattr_destroy(&mutex_attr);
    pthread_condattr_destroy(&cond_attr);
    /*=================================================================*/

    return 0;
}

static int init_daemon() {
    pid_t pid;
    pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }
    if (pid > 0) {
        printf("TinyFile service started");
        exit(0);
    }
    if (setsid() < 0) { // new session
        perror("setsid failed");
        return -1;
    }

    printf("TinyFile daemon: Initialization complete\n");
    return 0;
}

static void tinyfile_daemon_loop() {
    printf("TinyFile daemon running...\n");
    while(keep_running) {
        sleep(1);  // main thread waits, workers do work
       
        pthread_mutex_lock(&tinyfile_service->req_queue->mutex); // stats
        unsigned long total = tinyfile_service->req_queue->total_requests;
        unsigned long completed = tinyfile_service->req_queue->completed_requests;
        int pending = tinyfile_service->req_queue->count;
        pthread_mutex_unlock(&tinyfile_service->req_queue->mutex);
        if (debug_foreground)
            printf("Stats - Total: %lu, Completed: %lu, Pending: %d\n", 
                    total, completed, pending);
    }
    printf("Daemon loop exiting...\n");
}

/*
 * Create single shared mem obj and assign information to segment structure
 */ 
int create_shm_segment(shm_segment_t *segment, const char *name, size_t size) {

    // Create shared mem obj
    if ((segment->fd = shm_open(name, O_CREAT | O_RDWR, 0666)) == -1) {
        perror("shm_open failed");
        return -1;
    }

    if (ftruncate(segment->fd, size) == -1) { // set size of shared mem obj
        perror("ftruncate failed");
        close(segment->fd);
        shm_unlink(name);
        return -1;
    }

    // Map shared mem obj to process addr space
    segment->addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, segment->fd, 0);
    if (segment->addr == MAP_FAILED) {
        perror("mmap failed");
        close(segment->fd);
        shm_unlink(name);
        return -1;
    }

    // store info
    segment->size = size;
    strncpy(segment->name, name, sizeof(segment->name) - 1);
    segment->name[sizeof(segment->name) - 1] = '\0';
    segment->is_free = 1;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED); // setup lock for shared memeory
    pthread_mutex_init(&segment->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    // init mem segment
    memset(segment->addr, 0, size);

    printf("Created shared memory segment: %s, size: %zu bytes\n", name, size);
    return 0;
}


int create_data_segments(int n_segments, size_t segment_size, shm_segment_t **segments) {
    *segments = (shm_segment_t *) malloc(n_segments * sizeof(shm_segment_t));
    if (*segments == NULL) {
        perror("malloc failed");
        return -1;
    }
    
    for (int i = 0; i < n_segments; i++) {
        char segment_name[64];
        snprintf(segment_name, sizeof(segment_name), "/tinyfile_seg_%d", i);
        
        if (create_shm_segment(&(*segments)[i], segment_name, segment_size) == -1) {
            // error handling
            for (int j = 0; j < i; j++) {
                munmap((*segments)[j].addr, (*segments)[j].size);
                close((*segments)[j].fd);
                shm_unlink((*segments)[j].name);
            }
            free(*segments);
            return -1;
        }
    }
    
    return 0;
}

int create_control_segment(shm_segment_t *segment, int queue_capacity, int n_data_segs, size_t data_seg_size, request_queue_t **queue_ptr_out) {
    const char *name = "/tinyfile_control";
    size_t total_size = sizeof(control_segment_header_t);

    if ((segment->fd = shm_open(name, O_CREAT | O_RDWR, 0666)) == -1) {
        perror("shm_open failed for control segment");
        return -1;
    }
    if (ftruncate(segment->fd, total_size) == -1) {
        perror("ftruncate failed");
        close(segment->fd);
        shm_unlink(name);
        return -1;
    }
    segment->addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, segment->fd, 0);
    if (segment->addr == MAP_FAILED) {
        perror("mmap failed");
        close(segment->fd);
        shm_unlink(name);
        return -1;
    }

    segment->size = total_size;
    strncpy(segment->name, name, sizeof(segment->name) - 1);
    segment->name[sizeof(segment->name) - 1] = '\0';
    control_segment_header_t *header = (control_segment_header_t *)segment->addr;
    header->n_data_segments = n_data_segs;
    header->data_segment_size = data_seg_size;
    header->magic_number = 0x54494E59;  // is this necessary to make sure we are sane? Not sure
    request_queue_t *queue = &header->req_queue;
    // queue->requests = (request_t *)((char *)segment->addr + sizeof(control_segment_header_t));
    init_request_queue(queue, queue_capacity);
    *queue_ptr_out = queue;
    
    printf("Created control segment: %s, queue capacity: %d\n", 
           name, queue_capacity);
    
    return 0;

}

int find_free_segment(tinyfile_service_t *service) {
    for (int i = 0; i < service->n_segments; i++) {
        pthread_mutex_lock(&service->segments[i].lock);
        if (service->segments[i].is_free) {
            service->segments[i].is_free = 0;
            pthread_mutex_unlock(&service->segments[i].lock);
            return i;
        }
        pthread_mutex_unlock(&service->segments[i].lock);
    }
    return -1;
}

void release_segment(tinyfile_service_t *service, int segment_id) {
    if (segment_id >= 0 && segment_id < service->n_segments) {
        pthread_mutex_lock(&service->segments[segment_id].lock);
        service->segments[segment_id].is_free = 1;
        pthread_mutex_unlock(&service->segments[segment_id].lock);
    }
}

int compress_data(const void *input, size_t input_size, void **output, size_t *output_size) {
    size_t max_compressed_size = snappy_max_compressed_length(input_size);
    *output = malloc(max_compressed_size);
    if (!*output) {
        fprintf(stderr, "Output failed to malloc output buffer");
        return -1;
    }
    
    struct snappy_env env;
    snappy_init_env(&env);
    
    int result = snappy_compress(&env, (const char *)input, input_size, (char *)*output, output_size);
    
    snappy_free_env(&env);
    
    return (result == 0) ? 0 : -1;
}

void process_request(request_t *req) {
    if (req->req_type == REQ_ALLOC_SEGMENT) {
        // req->required_size // create a buffer of this size. 
        int seg_id = find_free_segment(tinyfile_service);
        if (seg_id == -1) { // no free segments request fails
            snprintf(req->error_msg, sizeof(req->error_msg), 
                    "No free segment available");
            req->status = STATUS_ERROR;
            req->allocated_segment_id = -1;
            
        } else {
            
            compress_job_t *state = get_client_state(req->client_pid);
            if (!state) {
                req->status = STATUS_ERROR;
                snprintf(req->error_msg, sizeof(req->error_msg), "Too many clients");
                release_segment(tinyfile_service, seg_id);
                req->allocated_segment_id = -1;
                return;
            }
            state->total_packets = (req->required_size / tinyfile_service->segment_size);
            if ((req->required_size % tinyfile_service->segment_size) != 0)
                state->total_packets++;
            state->packets_received = 0;
            state->packets_sent = 0;
            req->allocated_segment_id = seg_id;
            state->segment_id = seg_id;
            
            state->input_buf = (void*) malloc((size_t)state->total_packets * tinyfile_service->segment_size); 
            if (!state->input_buf) {
                req->status = STATUS_ERROR;
                snprintf(req->error_msg, sizeof(req->error_msg), "Memory allocation failure");
                release_segment(tinyfile_service, seg_id);
                req->allocated_segment_id = -1;
                clear_client_state(req->client_pid);
                return;
            }
            req->status = STATUS_COMPLETED;
            
            printf("Server: Allocated segment %d to client %d (request %d)\n", 
                   seg_id, req->client_pid, req->request_id);
        }
        return;
    }

    if (req->req_type == REQ_SENT_DATA) {

        
        compress_job_t *state = get_client_state(req->client_pid);
        if (!state || !state->input_buf) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "Invalid client state");
            return;
        }
        if(state->packets_received < state->total_packets) {
            pthread_mutex_lock(&tinyfile_service->segments[state->segment_id].lock);
            void *src = tinyfile_service->segments[state->segment_id].addr;
            void *dst = (char*)state->input_buf + (state->packets_received * tinyfile_service->segment_size);
            memcpy(dst, src, tinyfile_service->segment_size);
            pthread_mutex_unlock(&tinyfile_service->segments[state->segment_id].lock);

            state->packets_received++;
            req->status = STATUS_COMPLETED;

            printf("Server: Received chunk from client %d at offset %zu (%zu bytes)\n",
                    req->client_pid, 
                    (size_t)(state->packets_received * tinyfile_service->segment_size), 
                    tinyfile_service->segment_size);
            return;
        } else {
            // error stuff since we've already received all the data but they sent more
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "Total packets for input size exceeded");
            release_segment(tinyfile_service, state->segment_id);
            req->allocated_segment_id = -1;
            clear_client_state(req->client_pid);
            return;
        }
    }
    
    if (req->req_type == REQ_FREE_SEGMENT) {
        compress_job_t *state = get_client_state(req->client_pid);
        if (!state || !state->input_buf) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "Invalid client state");
            return;
        }
        int seg_id = state->segment_id;
        release_segment(tinyfile_service, seg_id);
        clear_client_state(req->client_pid);
        req->status = STATUS_COMPLETED;
        printf("Server: Released segment %d from client %d (request %d)\n", 
               seg_id, req->client_pid, req->request_id);
        return;
    }
    
    if (req->req_type == REQ_COMPRESS) {
        compress_job_t *state = get_client_state(req->client_pid);
        if (!state || !state->input_buf) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "Invalid client state");
            return;
        }
        if (state->packets_received != state->total_packets) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg),
                    "Incomplete data: got %d, expected %d packets",
                    state->packets_received, state->total_packets);
            release_segment(tinyfile_service, state->segment_id);
            clear_client_state(req->client_pid);
            
            return;
        }

        if (compress_data(state->input_buf, req->required_size, &state->output_buf, &state->output_size) == -1) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "Compression failed");
            release_segment(tinyfile_service, state->segment_id);
            clear_client_state(req->client_pid); 
            return;
        }
        printf("Server: Successfully compressed data! Input: %zu bytes -> Output: %zu bytes\n",
                    req->required_size, state->output_size);
        // calc how many output packets we need
        state->total_output_packets = state->output_size / tinyfile_service->segment_size;
        if (state->output_size % tinyfile_service->segment_size != 0) {
            state->total_output_packets++;
        }
        state->packets_sent = 0;  // reset counter
        pthread_mutex_lock(&tinyfile_service->segments[state->segment_id].lock);
        void *dst = tinyfile_service->segments[state->segment_id].addr;
        void *src = (char *)state->output_buf + (state->packets_sent * tinyfile_service->segment_size);
        size_t bytes_remaining = state->output_size - (state->packets_sent * tinyfile_service->segment_size);
        size_t bytes_to_copy = (bytes_remaining < tinyfile_service->segment_size) ? bytes_remaining : tinyfile_service->segment_size;

        printf("Packet %d: offset=%zu, bytes_remaining=%zu, bytes_to_copy=%zu\n", 
            state->packets_sent, (state->packets_sent * tinyfile_service->segment_size), bytes_remaining, bytes_to_copy);
        printf("Destination addr: %p, Src addr: %p\n", dst, src);

        memcpy(dst, src, bytes_to_copy);
        
        req->allocated_segment_id = state->segment_id;
        req->output_size = state->output_size;
        state->packets_sent++;
        req->status = STATUS_COMPLETED;
        printf("Server: Sent compressed chunk to client %d at offset %zu (%zu bytes)\n", 
                req->client_pid, 
                (size_t)(state->packets_received * tinyfile_service->segment_size), 
                tinyfile_service->segment_size);
        
        // send response saying we sent first segment of output data

        pthread_mutex_unlock(&tinyfile_service->segments[state->segment_id].lock);
        return;
    }

    if (req->req_type == REQ_REC_DATA) {
        // client received a segment, send the next one
        compress_job_t *state = get_client_state(req->client_pid);
        if (!state || !state->output_buf) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "Invalid client state");
            return;
        }
        if (state->packets_sent >= state->total_output_packets) {
            req->status = STATUS_ERROR;
            snprintf(req->error_msg, sizeof(req->error_msg), "All output already sent");
            return;
        }
    
        pthread_mutex_lock(&tinyfile_service->segments[state->segment_id].lock);
        void *dst = tinyfile_service->segments[state->segment_id].addr;
        void *src = (char *)state->output_buf + (state->packets_sent * tinyfile_service->segment_size);
        size_t bytes_sent = state->packets_sent * tinyfile_service->segment_size;
        size_t bytes_remaining = state->output_size - bytes_sent;
        size_t bytes_to_copy = (bytes_remaining < tinyfile_service->segment_size) ? bytes_remaining : tinyfile_service->segment_size;
        memcpy(dst, src, bytes_to_copy);
        req->allocated_segment_id = state->segment_id;
        req->output_size = state->output_size;
        state->packets_sent++;
        req->status = STATUS_COMPLETED;
        printf("Server: Sent compressed chunk to client %d at offset %zu (%zu bytes)\n", 
                req->client_pid, 
                (size_t)(state->packets_received * tinyfile_service->segment_size), 
                tinyfile_service->segment_size);
        
        // send response saying we sent first segment of output data

        pthread_mutex_unlock(&tinyfile_service->segments[state->segment_id].lock);
        return;

    }
}

void cleanup_shm_segment(shm_segment_t *segment) {
    if (segment->addr != MAP_FAILED && segment->addr != NULL) {
        munmap(segment->addr, segment->size);
    }
    if (segment->fd != -1) {
        close(segment->fd);
    }
}

void destroy_shm_segment(shm_segment_t *segment) {
    cleanup_shm_segment(segment);
    shm_unlink(segment->name);
    printf("Destroyed shared memory segment: %s\n", segment->name);
}

void parse_args(int argc, char* argv[])
{
    int inx;
    int n_sms = 5; //defaults
    int sms_size = 4096;

    for (inx = 1; inx < argc; inx++) 
    {
        if (strcmp(argv[inx], "--n_sms") == 0)
        {
            if (inx + 1 < argc) {
                n_sms = atoi(argv[++inx]);
            }
        }
        else if (strcmp(argv[inx], "--sms_size") == 0)
        {
            if (inx + 1 < argc) {
                sms_size = atoi(argv[++inx]);
            }
        }
        else if (strcmp(argv[inx], "-f") == 0)
        {
            debug_foreground = 1;
        }
    }

    tinyfile_service->n_segments = n_sms;
    tinyfile_service->segment_size = sms_size; 
    printf("n_sms = %d\n", n_sms);
    printf("sms_size = %d\n", sms_size);
}

extern int dequeue_request(request_queue_t *queue, request_t *req) {
    pthread_mutex_lock(&queue->mutex);

    // wait for thread queue to not be empty (queue->count == 0) 
    while (queue->count == 0 && tinyfile_service->running) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    // This is so if threads are still waiting when its time to terminate the server, they will exit
    // If we receive broadcast but queue is empty, we return
    if (queue->count == 0 || !tinyfile_service->running) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    *req = queue->requests[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    
    printf("Dequeued request ID: %d, client PID: %d (queue count: %d)\n",
           req->request_id, req->client_pid, queue->count);

    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);
    return 0;

}


void *worker_thread(void *arg) {
    int thread_id = *((int *)arg);
    printf("Worker thread %d started\n", thread_id);
    
    while (tinyfile_service->running) {
        request_t req;
        if (dequeue_request(tinyfile_service->req_queue, &req) == -1) {
            break;
        }
        
        req.status = STATUS_PROCESSING;

        process_request(&req);

        char response_queue_name[64];
        snprintf(response_queue_name, sizeof(response_queue_name), 
                "/tinyfile_resp_%d", req.client_pid);
        
        int client_shm_fd = shm_open(response_queue_name, O_RDWR, 0666);
        if (client_shm_fd != -1) {
            size_t response_queue_size = sizeof(response_queue_t);
            void *client_shm = mmap(NULL, response_queue_size, PROT_READ | PROT_WRITE, 
                                   MAP_SHARED, client_shm_fd, 0);
            
            if (client_shm != MAP_FAILED) {
                response_queue_t *resp_queue = (response_queue_t *)client_shm;
                printf("Server: Mapped client response queue at %p (capacity=%d, count=%d)\n",
                        client_shm, resp_queue->capacity, resp_queue->count);
                response_t response;
                response.request_id = req.request_id;
                response.client_pid = req.client_pid;
                response.status = req.status;
                response.output_size = req.output_size;
                response.output_offset = req.output_offset;
                strncpy(response.error_msg, req.error_msg, sizeof(response.error_msg));
                
                
                if (req.req_type == REQ_ALLOC_SEGMENT) {
                    response.output_offset = req.allocated_segment_id;
                }
                
                // enqueue response
                pthread_mutex_lock(&resp_queue->mutex);
                while (resp_queue->count >= resp_queue->capacity) {
                    pthread_cond_wait(&resp_queue->not_full, &resp_queue->mutex);
                }
                resp_queue->responses[resp_queue->tail] = response;
                
                resp_queue->tail = (resp_queue->tail + 1) % resp_queue->capacity;
                resp_queue->count++;

                printf("Server: Enqueued response for client %d (req_id=%d, status=%s, queue_count=%d)\n",
                    req.client_pid, req.request_id,
                    response.status == STATUS_COMPLETED ? "COMPLETED" :
                    response.status == STATUS_ERROR ? "ERROR" :
                    response.status == STATUS_PROCESSING ? "PROCESSING" : "PENDING",
                    resp_queue->count);
                pthread_cond_signal(&resp_queue->not_empty);
                pthread_mutex_unlock(&resp_queue->mutex);
                
                munmap(client_shm, response_queue_size);
            }
            close(client_shm_fd);
        }
        
        pthread_mutex_lock(&tinyfile_service->req_queue->mutex);
        tinyfile_service->req_queue->completed_requests++;
        pthread_mutex_unlock(&tinyfile_service->req_queue->mutex);
    }
    
    printf("Worker thread %d exiting\n", thread_id);
    return NULL;
}

int start_worker_threads(int n_workers) {
    tinyfile_service->n_workers = n_workers;
    tinyfile_service->worker_threads = malloc(n_workers * sizeof(pthread_t));
    
    if (!tinyfile_service->worker_threads) {
        perror("Failed to allocate worker threads");
        return -1;
    }
    
    int *thread_ids = malloc(n_workers * sizeof(int));
    
    for (int i = 0; i < n_workers; i++) {
        thread_ids[i] = i;
        if (pthread_create(&tinyfile_service->worker_threads[i], NULL, 
                          worker_thread, &thread_ids[i]) != 0) {
            perror("Failed to create worker thread");
            return -1;
        }
    }
    
    printf("Started %d worker threads\n", n_workers);
    return 0;
}

void stop_worker_threads() {
    printf("Stopping worker threads...\n");
    pthread_cond_broadcast(&tinyfile_service->req_queue->not_empty);
    for (int i = 0; i < tinyfile_service->n_workers; i++) {
        pthread_join(tinyfile_service->worker_threads[i], NULL);
    }
    
    free(tinyfile_service->worker_threads);
    printf("All worker threads stopped\n");
}

void cleanup_service() {
    printf("Cleaning up TinyFile service...\n");
    
    if (tinyfile_service) {
        // stop threads
        if (tinyfile_service->worker_threads) {
            stop_worker_threads();
        }
        if (tinyfile_service->segments) {
            for (int i = 0; i < tinyfile_service->n_segments; i++) {
                destroy_shm_segment(&tinyfile_service->segments[i]);
            }
            free(tinyfile_service->segments);
        }
        destroy_shm_segment(&tinyfile_service->control_segment);
        
        free(tinyfile_service);
    }
    
    printf("Cleanup complete\n");
}

// uthread_arg_t uargs[NUM_THREADS];
// uthread_t utids[NUM_THREADS];

int main(int argc, char* argv[])
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);


    tinyfile_service = (tinyfile_service_t *) malloc(sizeof(tinyfile_service_t));
    if (!tinyfile_service) {
        fprintf(stderr, "Failed to allocate service structure\n");
        exit(1);
    }
    memset(tinyfile_service, 0, sizeof(tinyfile_service_t));
    tinyfile_service->running = 1;

    parse_args(argc, argv);
    if (tinyfile_service->n_segments == 0 || tinyfile_service->segment_size == 0) {
        fprintf(stderr, "Invalid arguments: --n_sms and --sms_size are required\n");
        exit(1);
    }

    if (!debug_foreground && init_daemon() != 0) {
        fprintf(stderr, "Failed to initialize daemon\n");
        exit(1);
    }
    if (create_control_segment(&(tinyfile_service->control_segment), REQUEST_QUEUE_CAPACITY, tinyfile_service->n_segments, tinyfile_service->segment_size, &(tinyfile_service->req_queue)) != 0) {
        fprintf(stderr, "Failed to initialize control segment\n");
        exit(1);
    }

    if (create_data_segments(tinyfile_service->n_segments, tinyfile_service->segment_size, &(tinyfile_service->segments)) != 0) {
        fprintf(stderr, "Failed to initialize data segments\n");
        exit(1);
    }

    if (start_worker_threads(NUM_THREADS) != 0) { // start threads for parallel processing of requests
        fprintf(stderr, "Failed to start worker threads\n");
        cleanup_service();
        exit(1);
    }

    tinyfile_daemon_loop();

    cleanup_service();

    return 0;
}