#ifndef TINYFILE_H
#define TINYFILE_H

#include <pthread.h>
#include <stddef.h>

#define REQUEST_QUEUE_CAPACITY  32
#define RESPONSE_QUEUE_CAPACITY 10

/////////////////////////////////// ENUMS ///////////////////////////////////
typedef enum {
    REQ_ALLOC_SEGMENT,     
    REQ_SENT_DATA,
    REQ_COMPRESS,           
    REQ_REC_DATA,
    REQ_FREE_SEGMENT        
} request_type_t;

typedef enum {
    STATUS_PENDING,
    STATUS_PROCESSING,
    STATUS_COMPLETED,
    STATUS_ERROR
} request_status_t;


////////////////////////////// DATA STRUCTURES //////////////////////////////
typedef struct {
    int request_id; 
    int client_pid;
    request_type_t req_type;
    request_status_t status;
    size_t required_size;           // Input: size needed
    int allocated_segment_id;       // Output: which segment was allocated
    size_t input_size;
    size_t output_size;
    size_t input_offset;
    size_t output_offset;

    char error_msg[256];
} request_t;

typedef struct {
    request_t requests[REQUEST_QUEUE_CAPACITY];
    int capacity;
    int head;
    int tail;
    int count;
    
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    
    // Statistics
    unsigned long total_requests;
    unsigned long completed_requests;
} request_queue_t;

/*
 * Shared memory segment information
 * Can add more content as we go on to this struct
 */
typedef struct {
    void *addr;         // address returned by mmap() to move shared memory object into process address space
    int fd;             // file descriptor for segment (returned by shm_open)
    size_t size;        // size of segment in bytes
    char name[64];      // segment name (necessary for POSIX identification)
    int is_free;
    pthread_mutex_t lock;
} shm_segment_t;

typedef struct {
    int n_segments;
    size_t segment_size;
    shm_segment_t *segments;
    request_queue_t *req_queue;
    shm_segment_t control_segment;
    int running;
    pthread_t *worker_threads;
    int n_workers;
} tinyfile_service_t;

typedef struct {
    int request_id;
    int client_pid;
    request_status_t status;
    size_t output_size;
    size_t output_offset;
    char error_msg[256];
} response_t;

typedef struct {
    response_t responses[RESPONSE_QUEUE_CAPACITY];
    int capacity;
    int head;
    int tail;
    int count;
    
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} response_queue_t;

// Control segment layout in shared memory
typedef struct {
    int n_data_segments;
    size_t data_segment_size;
    int magic_number;
    request_queue_t req_queue;
} control_segment_header_t;

typedef struct {
    int client_pid;     // setting to 0 will work (unless scheduler requests tinyfile service then it breaks)
    int total_packets;  // file_size/seg_size + (file_size % seg_size != 0) 
    int segment_id;
    int packets_received; // how many input packets we've been sent so far (incremented upon REQ_SENT_DATA)
    void* input_buf; // write data at input_buf[(packets_received * tinyfile_service->sms_size)]
    int packets_sent;
    int total_output_packets;
    size_t output_size;
    void* output_buf; // read data at output_buf[(packets_sent * tinyfile_service->sms_size)]
} compress_job_t;

///////////////////////////////////////////// FUNCTIONS /////////////////////////////////////////////
int create_shm_segment(shm_segment_t *segment, const char *name, size_t size);
int create_data_segments(int n_segments, size_t segment_size, shm_segment_t **segments);
int create_control_segment(shm_segment_t *segment, int queue_capacity, int n_data_segs, size_t data_seg_size, request_queue_t **queue_ptr_out);

int find_free_segment(tinyfile_service_t *service);
void release_segment(tinyfile_service_t *service, int segment_id);
void cleanup_shm_segment(shm_segment_t *segment); // for both application and server
void destroy_shm_segment(shm_segment_t *segment); // ONLY CALLED BY SERVER to shm_unlink()

extern int enqueue_request(request_queue_t *queue, request_t *req);
extern int dequeue_request(request_queue_t *queue, request_t *req);

void process_request(request_t *req);
int compress_data(const void *input, size_t input_size, void **output, size_t *output_size);

int start_worker_threads(int n_workers);
void stop_worker_threads(void);

void cleanup_service(void);

#endif // TINYFILE_H