#ifndef GTFS
#define GTFS

#include <string>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

using namespace std;

#define PASS "\033[32;1m PASS \033[0m\n"
#define FAIL "\033[31;1m FAIL \033[0m\n"


#define MAGIC_GTFS 0x484B4E47 // "HKNG" in hex

// GTFileSystem basic data structures 

#define MAX_FILENAME_LEN 255
#define MAX_NUM_FILES_PER_DIR 1024

extern int do_verbose;

typedef struct write write_t; // to fix circular depedency in gtfs_t

typedef enum {
    LOG_TYPE_UNDO,
    LOG_TYPE_REDO
} log_type_t;

// Extra structs to not have to open files but still get metadata
typedef struct file_metadata {
    string filename;
    int length; // length of file
    bool is_open; // whether file is currently open by any process
    int open_count; // track how many times file has been opened (for safety)
    int owner_pid;
} file_metadata_t;

/**
 * Log entry format on disk:
 * [MAGIC_NUMBER:4 bytes][type:4 bytes][txn_id:8 bytes][is_committed:4 bytes]
 * [filename_length:4 bytes][filename:variable][offset:4 bytes][length:4 bytes]
 * [data:variable bytes][CHECKSUM:4 bytes]
 * 
 * use this to be able to parse through log file properly, this is our standard
 * MAGIC NUMBER
 */
typedef struct log_entry {
    log_type_t type;
    string filename;
    int offset;
    int length;
    char* data; // actual data to write
    unsigned long txn_id;
    bool is_committed;
    
    // Some cpp constructor stuff for log_entry()
    log_entry() : data(NULL), txn_id(0), is_committed(false) {}
    ~log_entry() { if (data) free(data); }
} log_entry_t;

typedef struct gtfs { // entire file system instance
    string dirname; // directory path where all files and metadata stored

    // NEW
    string path_to_log; // path to log where we persist committed writes
    int log_fd; // file descriptore for log
    string metadata_file_path; // path_to_metadata file that stores information about all files
    
    // In-memory map of all files managed by this file system
    // Key: filename, Value: metadata about that file
    map<string, file_metadata_t> file_table;

    // lock file stuff needed
    int lock_fd;
    string lock_file_path;

    map<string, vector<write_t*>> pending_writes;
    
    unsigned long next_txn_id; // random next id for TX
} gtfs_t;

typedef struct file {
    string filename; // The name of the file (up to 255 characters based on MAX_FILENAME_LEN)
    int file_length; // The current length of the file in bytes

    // NEW
    char* mapped_data_seg; // ptr to memory-mapped region of file
    int fd; // file descriptor for this file (its ID)
    gtfs_t* parent_gtfs; // poitner back to parent fs
    /**
     * Possibly a flag indicating whether the file is currently open
     */
} file_t;

struct write { // a pending write operation and contains
    string filename; // Which file is being written to
    int offset; // Starting position in the file (in bytes)
    int length; // How many bytes are being written
    char *data; // Pointer to the actual data being written

    // NEW
    char* original_data; // original data that was at that offset (for undo/abort operations)
    file_t* parent_file; // pointer to the file this write belongs to
    bool is_synced; // whether operation has been pushed to disk yet

    unsigned long txn_id;
    log_entry_t* undo_entry; // undo log entry
    log_entry_t* redo_entry; //redo log entry (created on sync)
};

// GTFileSystem basic API calls

gtfs_t* gtfs_init(string directory, int verbose_flag);
int gtfs_clean(gtfs_t *gtfs);

file_t* gtfs_open_file(gtfs_t* gtfs, string filename, int file_length);
int gtfs_close_file(gtfs_t* gtfs, file_t* fl);
int gtfs_remove_file(gtfs_t* gtfs, file_t* fl);

char* gtfs_read_file(gtfs_t* gtfs, file_t* fl, int offset, int length);
write_t* gtfs_write_file(gtfs_t* gtfs, file_t* fl, int offset, int length, const char* data);
int gtfs_sync_write_file(write_t* write_id);
int gtfs_abort_write_file(write_t* write_id);

// BONUS: Implement below API calls to get bonus credits

int gtfs_clean_n_bytes(gtfs_t *gtfs, int bytes);
int gtfs_sync_write_file_n_bytes(write_t* write_id, int bytes);

// Add here any additional data structures or API calls

// Lock functions
int acquire_fs_lock(gtfs_t* gtfs);
int release_fs_lock(gtfs_t* gtfs);

// LOG Stuff
int load_metadata(gtfs_t* gtfs); // lookup metadata of a file
int save_metadata(gtfs_t* gtfs); // change the metadata of a file
int replay_log(gtfs_t* gtfs); // crash recovery of the system (gtfs_init)
uint32_t compute_simple_checksum(const char* data, size_t length);
int apply_log_entry_to_file(gtfs_t* gtfs, log_entry_t* entry);
log_entry_t* parse_log_entry_from_buffer(char* buffer, size_t buffer_size, size_t* current_pos);
int write_log_entry_to_disk(gtfs_t* gtfs, log_entry_t* entry);

#endif
