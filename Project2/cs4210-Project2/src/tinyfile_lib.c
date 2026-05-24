#include "tinyfile_lib.h"
#include "tinyfile.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
/**
 * Principle of the tinyfile_lib is to be able to setup the necessary comms between client and server
 * The comms will go as such to compress a full file:
 *      - client -> server "I need a segment" (REQ_ALLOC_SEGMENT)
 *      - server -> client "Here you go" (RESP)
 *      - client -> server "Sent data over" (loop until end of file) (REQ_SENT_DATA)
 *      - server -> client "I got the data" (loop until end of file)
 *      - client -> server "Compress my file" (REQ_COMPRESS)
 *      - server -> client "Sent output data over" (loop until end of file) (RESP_SENT_DATA)
 *      - client -> server "I got the data" (loop until end of file) (REQ_REC_DATA)
 *      - server -> client "All data sent" ()
 *      - client -> server "Free my block now" (REQ_FREE_SEGMENT)
 * 
 * The response and req system have separate queues (service has request, client has response)
 */


////////////////////////////////////// HELPERS //////////////////////////////////////
static int init_response_queue(response_queue_t *queue, int capacity) {
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    
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
    
    return 0;
}

static int wait_for_response(client_context_t *ctx, int request_id, response_t *resp_out) {
    pthread_mutex_lock(&ctx->response_queue->mutex);
    
    // printf("Client %d: Waiting for response %d (current queue count=%d)\n",
    //        ctx->client_pid, request_id, ctx->response_queue->count);
   
    while (1) {
        // search for our rseponse in the queue
        for (int i = 0; i < ctx->response_queue->count; i++) {
            int idx = (ctx->response_queue->head + i) % ctx->response_queue->capacity;
            response_t *resp = &ctx->response_queue->responses[idx];
            
            // printf("Client %d: Checking response at idx %d: req_id=%d (looking for %d)\n",
            //        ctx->client_pid, idx, resp->request_id, request_id);
           
            if (resp->request_id == request_id) {
                *resp_out = *resp; //found
                
                // printf("Client %d: Found matching response! (req_id=%d)\n",
                //        ctx->client_pid, request_id);
               
                // remove from queue
                for (int j = i; j < ctx->response_queue->count - 1; j++) {
                    int cur_idx = (ctx->response_queue->head + j) % ctx->response_queue->capacity;
                    int next_idx = (ctx->response_queue->head + j + 1) % ctx->response_queue->capacity;
                    ctx->response_queue->responses[cur_idx] = ctx->response_queue->responses[next_idx];
                }
                ctx->response_queue->count--;
                ctx->response_queue->head++;
                pthread_cond_signal(&ctx->response_queue->not_full);
                pthread_mutex_unlock(&ctx->response_queue->mutex);
                return 0;
            }
        }
       
        // wait for new responses
        // printf("Client %d: Response %d not found, waiting...\n", ctx->client_pid, request_id);
        pthread_cond_wait(&ctx->response_queue->not_empty, &ctx->response_queue->mutex);
    }
   
    pthread_mutex_unlock(&ctx->response_queue->mutex);
    return -1;
}

// "I need a segment" -> "here is your segment"
static int client_request_segment(client_context_t *ctx, size_t required_size) {
    request_t req = {0};
    req.request_id = ctx->next_request_id++;
    req.client_pid = ctx->client_pid;
    req.req_type = REQ_ALLOC_SEGMENT;
    req.status = STATUS_PENDING;
    req.required_size = required_size;
    
    printf("Client %d: REQUEST - Allocate segment (size=%zu, req_id=%d)\n", ctx->client_pid, required_size, req.request_id);
    if (enqueue_request(ctx->service_req_queue, &req) != 0) {
        fprintf(stderr, "Failed to enqueue allocation request\n");
        return -1;
    }
    
    // printf("Enqueued request sucessfully, wait for response\n");
    response_t resp;
    if (wait_for_response(ctx, req.request_id, &resp) != 0) {
        fprintf(stderr, "Failed to get allocation response\n");
        return -1;
    }
    if (resp.status != STATUS_COMPLETED) {
        fprintf(stderr, "Allocation failed: %s\n", resp.error_msg);
        return -1;
    }
    
    int segment_id = (int)resp.output_offset;  // server puts seg_id here
    printf("Client %d: RESPONSE - Allocated segment %d\n", ctx->client_pid, segment_id);

    ////////////////////////// MAP SEGMENT//////////////////////////////
    if (segment_id < 0 || segment_id >= ctx->n_segments) {
        fprintf(stderr, "Invalid segment ID: %d\n", segment_id);
        return -1;
    }
    // Already mapped ?
    if (ctx->data_segments[segment_id].addr != NULL) {
        printf("Client %d: Segment %d already mapped\n", ctx->client_pid, segment_id);
        return segment_id;
    }
    // get segment name
    char segment_name[64];
    snprintf(segment_name, sizeof(segment_name), "/tinyfile_seg_%d", segment_id);
    int shm_fd = shm_open(segment_name, O_RDWR, 0666); // open via shm_opem
    if (shm_fd == -1) {
        perror("Failed to open shared memory segment");
        return -1;
    }
    // map to client addr space
    void *addr = mmap(NULL, ctx->segment_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    if (addr == MAP_FAILED) {
        perror("Failed to map shared memory segment");
        close(shm_fd);
        return -1;
    }
    // storer mapping info
    ctx->data_segments[segment_id].fd = shm_fd;
    ctx->data_segments[segment_id].addr = addr;
    ctx->data_segments[segment_id].size = ctx->segment_size;
    // strncpy(ctx->data_segments[segment_id].name, segment_name,sizeof(ctx->data_segments[segment_id].name) - 1);
    snprintf(ctx->data_segments[segment_id].name, sizeof(ctx->data_segments[segment_id].name), "%s", segment_name);
    printf("Client %d: Mapped segment %d at address %p\n", ctx->client_pid, segment_id, addr);
    return segment_id;
}
// informs the server that the client has acutally sent the data
static int client_request_sent_data(client_context_t *ctx, size_t required_size) {
    request_t req = {0};
    req.request_id = ctx->next_request_id++;
    req.client_pid = ctx->client_pid;
    req.req_type = REQ_SENT_DATA;
    req.status = STATUS_PENDING;
    req.required_size = required_size;

    
    printf("Client %d: REQUEST - Read the sent_data\n", ctx->client_pid);
    if (enqueue_request(ctx->service_req_queue, &req) != 0) {
        fprintf(stderr, "Failed to enqueue sent_data request\n");
        return -1;
    }
    printf("Request sent, waiting for response id=%d for client=%d....\n", req.request_id, req.client_pid);
    response_t resp;
    if (wait_for_response(ctx, req.request_id, &resp) != 0) {
        fprintf(stderr, "Failed to get sent_data response\n");
        return -1;
    }
    if (resp.status != STATUS_COMPLETED) {
        fprintf(stderr, "sent_data failed: %s\n", resp.error_msg);
        return -1;
    }
    return 0;
}

static int client_request_rec_data(client_context_t *ctx, size_t required_size) {
    request_t req = {0};
    req.request_id = ctx->next_request_id++;
    req.client_pid = ctx->client_pid;
    req.req_type = REQ_REC_DATA;
    req.status = STATUS_PENDING;
    req.required_size = required_size;

    printf("Client %d: REQUEST - Send more output data\n", 
           ctx->client_pid);
    if (enqueue_request(ctx->service_req_queue, &req) != 0) {
        fprintf(stderr, "Failed to enqueue sent_data request\n");
        return -1;
    }
    response_t resp;
    if (wait_for_response(ctx, req.request_id, &resp) != 0) {
        fprintf(stderr, "Failed to get sent_data response\n");
        return -1;
    }
    if (resp.status != STATUS_COMPLETED) {
        fprintf(stderr, "sent_data failed: %s\n", resp.error_msg);
        return -1;
    }
    return 0;
}

// "compress my data" -> "compressed"
static int client_request_compress(client_context_t *ctx, size_t input_size, size_t* output_size) {
    request_t req = {0};
    req.request_id = ctx->next_request_id++;
    req.client_pid = ctx->client_pid;
    req.req_type = REQ_COMPRESS;
    req.status = STATUS_PENDING;
    req.input_size = input_size;
    req.required_size = input_size;
    // req.input_offset = input_seg_id * ctx->segment_size;
    // req.output_offset = output_seg_id * ctx->segment_size;
    
    printf("Client %d: REQUEST - Compress data (req_id=%d)\n", ctx->client_pid, req.request_id);
    if (enqueue_request(ctx->service_req_queue, &req) != 0) {
        fprintf(stderr, "Failed to enqueue compression request\n");
        return -1;
    }
    response_t resp;
    if (wait_for_response(ctx, req.request_id, &resp) != 0) {
        fprintf(stderr, "Failed to get compression response\n");
        return -1;
    }
    if (resp.status != STATUS_COMPLETED) {
        fprintf(stderr, "Compression failed: %s\n", resp.error_msg);
        return -1;
    }
    *output_size = resp.output_size;
    printf("Client %d: RESPONSE - Compressed %zu -> %zu bytes\n", 
           ctx->client_pid, input_size, *output_size);
    return 0;
}

// eelease segment
static int client_release_segment(client_context_t *ctx, int segment_id) {
    request_t req = {0};
    req.request_id = ctx->next_request_id++;
    req.client_pid = ctx->client_pid;
    req.req_type = REQ_FREE_SEGMENT;
    req.status = STATUS_PENDING;
    req.allocated_segment_id = segment_id;
    req.output_offset = segment_id;  // store in output_offset for consistency
    
    printf("Client %d: REQUEST - Release segment %d (req_id=%d)\n", 
           ctx->client_pid, segment_id, req.request_id);
    if (enqueue_request(ctx->service_req_queue, &req) != 0) {
        fprintf(stderr, "Failed to enqueue free request\n");
        return -1;
    }
    response_t resp;
    if (wait_for_response(ctx, req.request_id, &resp) != 0) {
        fprintf(stderr, "Failed to get free response\n");
        return -1;
    }
    printf("Client %d: RESPONSE - Released segment %d\n", ctx->client_pid, segment_id);
    return 0;
}

static void* async_compress_worker(void *arg) {
    async_job_t *job = (async_job_t*)arg;
    
    printf("Async worker thread started for job %d\n", job->job_id);
    job->status = tinyfile_compress_sync(job->ctx, job->output_file, job->input_file);
    
    printf("Async worker thread completed for job %d (status=%d)\n", job->job_id, job->status);
    
    if (job->callback) { // callback
        job->callback(job->status, job->output_file, job->user_data);
    }
    
    pthread_mutex_lock(&job->ctx->async_jobs_lock);
    for (int i = 0; i < job->ctx->n_async_jobs; i++) {
        if (job->ctx->async_jobs[i] == job) {
            // shift remaining jobs down
            for (int j = i; j < job->ctx->n_async_jobs - 1; j++) {
                job->ctx->async_jobs[j] = job->ctx->async_jobs[j + 1];
            }
            job->ctx->n_async_jobs--;
            break;
        }
    }
    pthread_mutex_unlock(&job->ctx->async_jobs_lock);
    
    //cleanup
    free(job->input_file);
    free(job->output_file);
    free(job);
    
    return NULL;
}

static int init_async_job_tracking(client_context_t *ctx) {
    ctx->max_async_jobs = MAX_ASYNC_JOBS;
    ctx->async_jobs = calloc(ctx->max_async_jobs, sizeof(async_job_t*));
    if (!ctx->async_jobs) {
        return -1;
    }
    ctx->n_async_jobs = 0;
    
    pthread_mutex_init(&ctx->async_jobs_lock, NULL);
    return 0;
}
/////////////////////////////////////////////////////////////////////////////////////

// inint tinyfile service client ctx
int tinyfile_init(client_context_t **ctx_out) {
    client_context_t *ctx = malloc(sizeof(client_context_t));
    if (!ctx) {
        perror("Failed to allocate client context");
        return -1;
    }
    memset(ctx, 0, sizeof(client_context_t));

    ctx->client_pid = getpid();
    ctx->next_request_id = 0;

    printf("Client %d: Initializing...\n", ctx->client_pid);

    char response_queue_name[64];
    snprintf(response_queue_name, sizeof(response_queue_name), 
            "/tinyfile_resp_%d", ctx->client_pid);
    
    size_t response_queue_size = sizeof(response_queue_t);

    ctx->response_segment.fd = shm_open(response_queue_name, O_CREAT | O_RDWR, 0666);
    if (ctx->response_segment.fd == -1) {
        perror("Failed to create response queue");
        free(ctx);
        return -1;
    }

    if (ftruncate(ctx->response_segment.fd, response_queue_size) == -1) {
        perror("Failed to set response queue size");
        close(ctx->response_segment.fd);
        shm_unlink(response_queue_name);
        free(ctx);
        return -1;
    }
    
    ctx->response_segment.addr = mmap(NULL, response_queue_size, 
                                     PROT_READ | PROT_WRITE, MAP_SHARED, 
                                     ctx->response_segment.fd, 0);
                                      
    if (ctx->response_segment.addr == MAP_FAILED) {
        perror("Failed to map response queue");
        close(ctx->response_segment.fd);
        shm_unlink(response_queue_name);
        free(ctx);
        return -1;
    }
    
    ctx->response_segment.size = response_queue_size;
    // strncpy(ctx->response_segment.name, response_queue_name, sizeof(ctx->response_segment.name) - 1);
    snprintf(ctx->response_segment.name, sizeof(ctx->response_segment.name), "%s", response_queue_name);


    // init response queue
    ctx->response_queue = (response_queue_t *)ctx->response_segment.addr;
    // ctx->response_queue->responses = (response_t *)((char *)ctx->response_segment.addr + 
    //                                                 sizeof(response_queue_t));
    init_response_queue(ctx->response_queue, RESPONSE_QUEUE_CAPACITY);

    printf("Client %d: Created response queue at %s\n", ctx->client_pid, response_queue_name);

    int control_fd = shm_open("/tinyfile_control", O_RDWR, 0666);
    if (control_fd == -1) {
        perror("Failed to open service control segment - is server running?");
        cleanup_client_context(ctx);
        return -1;
    }
    
    // First map just the header to read configuration
    // size_t header_size = sizeof(control_segment_header_t) + (REQUEST_QUEUE_CAPACITY * sizeof(request_t));
    size_t header_size = sizeof(control_segment_header_t);
    void *header_addr = mmap(NULL, header_size, PROT_READ | PROT_WRITE, 
                            MAP_SHARED, control_fd, 0);
    
    if (header_addr == MAP_FAILED) {
        perror("Failed to map control segment header");
        close(control_fd);
        cleanup_client_context(ctx);
        return -1;
    }
    control_segment_header_t *header = (control_segment_header_t *)header_addr;
    if (header->magic_number != 0x54494E59) {
        fprintf(stderr, "Invalid magic number in control segment (got 0x%X, expected 0x54494E59)\n",header->magic_number);
        fprintf(stderr, "Service may not be initialized properly.\n");
        munmap(header_addr, header_size);
        close(control_fd);
        cleanup_client_context(ctx);
        return -1;
    }
    ctx->n_segments = header->n_data_segments;
    ctx->segment_size = header->data_segment_size;
    printf("Client %d: Connected to service - %d segments of %zu bytes each\n",
           ctx->client_pid, ctx->n_segments, ctx->segment_size);
    
    // get ptr to the requset queue
    ctx->service_req_queue = &header->req_queue;
    // ctx->service_req_queue->requests = (request_t *)((char *)header_addr + sizeof(control_segment_header_t));
    printf("Client %d: Fixed request queue pointer to %p\n", 
       ctx->client_pid, (void*)ctx->service_req_queue->requests);
    close(control_fd);  // can close fd after mapping
    
    printf("Client %d: Connected to service request queue\n", ctx->client_pid);
    
    // allocates them to us via REQ_ALLOC_SEGMENT requests
    ctx->data_segments = calloc(ctx->n_segments, sizeof(shm_segment_t));
    if (!ctx->data_segments) {
        perror("Failed to allocate data segments array");
        munmap(header_addr, header_size);
        cleanup_client_context(ctx);
        return -1;
    }
    // init all segments as useless
    for (int i = 0; i < ctx->n_segments; i++) {
        ctx->data_segments[i].fd = -1;
        ctx->data_segments[i].addr = NULL;
        ctx->data_segments[i].size = 0;
    }

    if (init_async_job_tracking(ctx) != 0) {
        fprintf(stderr, "Failed to initialize async job tracking\n");
        cleanup_client_context(ctx);
        return -1;
    }
    pthread_mutex_init(&ctx->compress_lock, NULL);

    printf("Client %d: Ready to map segments on-demand\n", ctx->client_pid);

    *ctx_out = ctx;
    printf("Client %d: Initialization complete!\n\n", ctx->client_pid);
    return 0;
}


int tinyfile_compress_sync(client_context_t *ctx, char *output_file, char *input_file) {
    pthread_mutex_lock(&ctx->compress_lock);

    int data_seg_id = -1;
    int result = -1;
    
    // step 1: Read input file
    FILE *fp = fopen(input_file, "rb");
    if (!fp) {
        pthread_mutex_unlock(&ctx->compress_lock);
        fprintf(stderr, "Failed to open input file: %s\n", input_file);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    printf("\n=== Client %d: Starting compression of %s (%zu bytes) ===\n", 
           ctx->client_pid, input_file, file_size);
    
    // step 2: Client -> "I need a segment" (for input)
    //         Server -> "here is your segment"
    

    time_t start_time = time(NULL);
    int timeout_seconds = 120;
    int retry_delay_ms = 100;
    while(data_seg_id == -1) {    
        data_seg_id = client_request_segment(ctx, file_size); // Doesn't actuall give a segment of file size, but uses that to determine how many packets we will send
        
        if (data_seg_id == -1) {
            if(difftime(time(NULL), start_time) >= timeout_seconds) {
                pthread_mutex_unlock(&ctx->compress_lock);
                fprintf(stderr, "Failed to allocate input segment\n");
                fclose(fp);
                return -1;
            }
            usleep(retry_delay_ms * 1000);
        }
    }

    // step 3: Stream input over shm_segment until all of the file has been received by the server 
    int packets_sent = 0;
    int total_packets = (file_size % ctx->segment_size) == 0 ? file_size / ctx->segment_size : (file_size / ctx->segment_size) + 1;  
    size_t bytes_read = 0;
    printf("Sending %d packets to the server\n", total_packets);
    while(packets_sent < total_packets) {
        printf("Client %d: Writing %zu bytes to input segment %d\n", ctx->client_pid, ctx->segment_size, data_seg_id);
        // do I need to grab the segment lock here?
        // change to fread(ctx->data_segments[data_seg_id].addr + packets_sent...)
        printf("Reading Segment %d from File...\n", packets_sent);
        bytes_read += fread(ctx->data_segments[data_seg_id].addr, 1, ctx->segment_size, fp); // fread will automatically update the file pointer by ctx->segment_size
        if(client_request_sent_data(ctx, file_size) == -1) {
            fprintf(stderr, "Failed to send input data\n");
            goto cleanup;
        }
        packets_sent++;
    }
    fclose(fp);

    // step 4: Client -> "compress my data"
    //         Server -> "compressed to segment"
    size_t compressed_size;
    if (client_request_compress(ctx, file_size, &compressed_size) != 0) {
        fprintf(stderr, "Compression failed\n");
        goto cleanup;
    }

    // step 5: read out buffer 
    int packets_received = 0;
    FILE *out_fp = fopen(output_file, "wb");
    if (!out_fp) {
        printf("Failed to open outfile\n");
        goto cleanup;
    }

    size_t bytes_written = 0;
    size_t bytes_remaining;
    size_t bytes_to_write;
    while(bytes_written < compressed_size) {
        // ensure we dont write garbage if compressed_size is not same as segment_size
        bytes_remaining = compressed_size - bytes_written;
        bytes_to_write = (bytes_remaining < ctx->segment_size) ? bytes_remaining : ctx->segment_size;
        
        printf("Client %d: Reading %zu bytes from output segment %d (packet %d)\n",ctx->client_pid, bytes_to_write, data_seg_id, packets_received);
        printf("Address of data segment: %p\n", ctx->data_segments[data_seg_id].addr);
        fwrite(ctx->data_segments[data_seg_id].addr, 1, bytes_to_write, out_fp);
        bytes_written += bytes_to_write;
        if (bytes_written < compressed_size) {
            if(client_request_rec_data(ctx, file_size) == -1) {
                fprintf(stderr, "Failed to send data request\n");
                goto cleanup;
            }
        }
    }
    fclose(out_fp);
    printf("Client %d: Wrote compressed file %s\n", ctx->client_pid, output_file);
    result = 0;

cleanup: //is this all for cleanup?
    //step 7: Release segments back to server
    if (data_seg_id != -1) {
        client_release_segment(ctx, data_seg_id);
    }
    pthread_mutex_unlock(&ctx->compress_lock);
    printf("=== Client %d: Compression complete ===\n\n", ctx->client_pid);
    return result;
}

int tinyfile_compress_async(client_context_t *ctx, char *output_file, char *input_file, compress_callback_t callback, void *user_data) {
    pthread_mutex_lock(&ctx->async_jobs_lock);
    if (ctx->n_async_jobs >= ctx->max_async_jobs) {
        pthread_mutex_unlock(&ctx->async_jobs_lock);
        fprintf(stderr, "Too many async jobs in flight\n");
        return -1;
    }
    pthread_mutex_unlock(&ctx->async_jobs_lock);
        async_job_t *job = malloc(sizeof(async_job_t));
    if (!job) {
        perror("Failed to allocate async job");
        return -1;
    }
    
    job->ctx = ctx;
    job->input_file = strdup(input_file);
    job->output_file = strdup(output_file);
    job->callback = callback;
    job->user_data = user_data;
    job->status = -1;
    job->job_id = ctx->next_request_id++;  
    pthread_mutex_lock(&ctx->async_jobs_lock);
    ctx->async_jobs[ctx->n_async_jobs++] = job;
    pthread_mutex_unlock(&ctx->async_jobs_lock);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    if (pthread_create(&job->thread, &attr, async_compress_worker, job) != 0) {
        perror("Failed to create async worker thread");
        pthread_attr_destroy(&attr);
        pthread_mutex_lock(&ctx->async_jobs_lock);
        ctx->n_async_jobs--;
        pthread_mutex_unlock(&ctx->async_jobs_lock);
        
        free(job->input_file);
        free(job->output_file);
        free(job);
        return -1;
    }
    
    pthread_attr_destroy(&attr);
    
    printf("Client %d: Started async compression job %d\n", 
           ctx->client_pid, job->job_id);
    
    return job->job_id;
}


void tinyfile_wait_all_async(client_context_t *ctx) {
    printf("Client %d: Waiting for all async jobs to complete...\n", ctx->client_pid);
    while (1) {
        pthread_mutex_lock(&ctx->async_jobs_lock);
        int jobs_remaining = ctx->n_async_jobs;
        pthread_mutex_unlock(&ctx->async_jobs_lock);
        if (jobs_remaining == 0) {
            break;
        } 
        printf("Client %d: %d async jobs still running...\n", 
               ctx->client_pid, jobs_remaining);
        usleep(100);  // Sleep 100us
    }
    
    printf("Client %d: All async jobs complete\n", ctx->client_pid);
}


void cleanup_client_context(client_context_t *ctx) {
    if (!ctx) return;
    
    printf("Client %d: Cleaning up...\n", ctx->client_pid);

    if (ctx->async_jobs) {
        tinyfile_wait_all_async(ctx);
        free(ctx->async_jobs);
        pthread_mutex_destroy(&ctx->async_jobs_lock);
    }
    pthread_mutex_destroy(&ctx->compress_lock);
    
    // munmap and close
    if (ctx->data_segments) {
        for (int i = 0; i < ctx->n_segments; i++) {
            if (ctx->data_segments[i].addr != MAP_FAILED && 
                ctx->data_segments[i].addr != NULL) {
                munmap(ctx->data_segments[i].addr, ctx->data_segments[i].size);
            }
        }
        free(ctx->data_segments);
    }
    
    // repsonse queue garbage
    if (ctx->response_segment.addr != MAP_FAILED && 
        ctx->response_segment.addr != NULL) {
        munmap(ctx->response_segment.addr, ctx->response_segment.size);
    }
    if (ctx->response_segment.fd != -1) {
        close(ctx->response_segment.fd);
        shm_unlink(ctx->response_segment.name);
    }
    
    free(ctx);
    printf("Client cleanup complete\n");
}