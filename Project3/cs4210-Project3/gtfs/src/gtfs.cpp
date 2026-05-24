#include "gtfs.hpp"

#define VERBOSE_PRINT(verbose, str...) do { \
    if (verbose) cout << "VERBOSE: "<< __FILE__ << ":" << __LINE__ << " " << __func__ << "(): " << str; \
} while(0)

int do_verbose;

// Lock Functions
int acquire_fs_lock(gtfs_t* gtfs) {
    if (!gtfs) return -1;
    struct flock lock;
    lock.l_type = F_WRLCK; // exclusive write lock (can't do both read/write)
    lock.l_whence = SEEK_SET; // start at beginning of file
    lock.l_start = 0;
    lock.l_len = 0; // extend full length of the file


    if (fcntl(gtfs->lock_fd, F_SETLKW, &lock) == -1) {
        perror("Failed to acquire lock");
        return -1;
    }
    return 0;
}

int release_fs_lock(gtfs_t* gtfs) {
    if (!gtfs) return -1;
    struct flock lock;
    lock.l_type = F_UNLCK; // unlock said file
    lock.l_whence = SEEK_SET; // start at beginning of file
    lock.l_start = 0;
    lock.l_len = 0; // extend full length of the file


    if (fcntl(gtfs->lock_fd, F_SETLK, &lock) == -1) {
        perror("Failed to release lock");
        return -1;
    }
    return 0;
}

int load_metadata(gtfs_t* gtfs) {
    if (!gtfs) return -1;

    FILE* meta_file = fopen(gtfs->metadata_file_path.c_str(), "r");
    if (!meta_file) {
        VERBOSE_PRINT(do_verbose, "No existing metadata file found (fresh init)\n");
        return 0;
    }

    int num_files = 0;
    if (fscanf(meta_file, "%d\n", &num_files) != 1) {
        VERBOSE_PRINT(do_verbose, "Failed to scan file for num_files in load_metadata()\n");
        fclose(meta_file);
        return -1;
    }
    VERBOSE_PRINT(do_verbose, "Loading metadata for " << num_files << " files\n");

    for (int i = 0; i < num_files; i++)
    {
        char filename_buf[MAX_FILENAME_LEN+1];
        int length;
        int is_open_int;
        int open_count;
        int owner_pid_int;

        if (fscanf(meta_file, "%s %d %d %d %d\n", filename_buf, &length, &is_open_int, &open_count, &owner_pid_int) != 5) { // filename, length, is_open, open_count
            VERBOSE_PRINT(do_verbose, "Failed to scan file for metadata in load_metadata()\n");
            fclose(meta_file);
            return -1;
        }

        file_metadata_t meta;
        meta.filename = string(filename_buf);
        meta.length = length;
        meta.is_open = (is_open_int != 0);
        meta.open_count = open_count;
        meta.owner_pid = owner_pid_int;

        gtfs->file_table[meta.filename] = meta;
        VERBOSE_PRINT(do_verbose, "Loaded metadata: " << meta.filename << " (length: " << length << ")\n");
    }

    fclose(meta_file);
    return 0;
}

int save_metadata(gtfs_t* gtfs) {
    if (!gtfs) return -1;
    FILE* meta_file = fopen(gtfs->metadata_file_path.c_str(), "w");
    if (!meta_file) {
        perror("Failed to save metadata");
        return -1;
    }

    VERBOSE_PRINT(do_verbose, "Saving metadata for " << gtfs->file_table.size() << " files:\n");
    for (auto& entry : gtfs->file_table) {
        VERBOSE_PRINT(do_verbose, "  - " << entry.first << "\n");
    }

    fprintf(meta_file, "%zu\n", gtfs->file_table.size());


    for (auto& entry : gtfs->file_table) { // filename, length, is_open, open_count
        fprintf(meta_file, "%s %d %d %d %d\n", entry.second.filename.c_str(), entry.second.length, entry.second.is_open, entry.second.open_count, entry.second.owner_pid);
    }

    fclose(meta_file);

    sync();

    return 0;

}

int replay_log(gtfs_t* gtfs) {
    if (!gtfs) return -1;
    VERBOSE_PRINT(do_verbose, "Starting log replay for crash recovery\n");

    struct stat log_stat;
    if (fstat(gtfs->log_fd, &log_stat) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to stat log file\n");
        return -1;
    }
    if (log_stat.st_size == 0) {
        VERBOSE_PRINT(do_verbose, "Log file is empty, nothing to replay\n");
        return 0;  // This is fine, just means fresh start
    }
    VERBOSE_PRINT(do_verbose, "Log file size: " << log_stat.st_size << " bytes\n");
    // map log data for effcient reading
    char* log_data = (char*)mmap(NULL, log_stat.st_size, PROT_READ, MAP_PRIVATE, gtfs->log_fd, 0);
    if (log_data == MAP_FAILED) { // if mapping failed
        VERBOSE_PRINT(do_verbose, "Failed to mmap log file\n");
        return -1;
    }
    vector<log_entry_t*> entries_to_replay;
    size_t current_pos = 0;
    int entries_found = 0;
    int entries_skipped = 0;
    
    while (current_pos < log_stat.st_size) { // iteration through each entry
        log_entry_t* entry = parse_log_entry_from_buffer(log_data, log_stat.st_size, &current_pos);
        
        if (!entry) {
            VERBOSE_PRINT(do_verbose, "Failed to parse entry at position " << current_pos << ", stopping replay\n");
            break;
        }
        
        entries_found++;
        
        
        
        if (entry->is_committed && entry->type == LOG_TYPE_REDO) { // Only replay entries that are marked as committed
            entries_to_replay.push_back(entry); 
            VERBOSE_PRINT(do_verbose, "Will replay: " << entry->filename << " offset=" << entry->offset << " length=" << entry->length << "\n");
        } else { // Uncommitted entries represent operations that were never finalized, delete those
            entries_skipped++;
            delete entry;
        }
    }
    
    VERBOSE_PRINT(do_verbose, "Found " << entries_found << " log entries, " << "will replay " << entries_to_replay.size() << ", skipped " << entries_skipped << "\n");
    
    // Apply each committed redo entry to its corresponding file
    
    int entries_applied = 0;
    for (log_entry_t* entry : entries_to_replay) {
        // This is where we actually restore the ststem state
        if (apply_log_entry_to_file(gtfs, entry) == 0) {
            entries_applied++;
        } else {
            VERBOSE_PRINT(do_verbose, "Warning: failed to apply log entry for " << entry->filename << "\n");
            // TODO: failing an apply to log entry good or bad?
        }
        delete entry;
    }
    
    VERBOSE_PRINT(do_verbose, "Successfully applied " << entries_applied << " out of " << entries_to_replay.size() << " log entries\n");
    
    // clean up and ensure changes are persisted
    munmap(log_data, log_stat.st_size);
    sync();
    
    VERBOSE_PRINT(do_verbose, "Log replay completed successfully\n");
    return 0;
}

// checksum for sanity check
uint32_t compute_simple_checksum(const char* data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum = (checksum << 1) ^ data[i];
    }
    return checksum;
}


int apply_log_entry_to_file(gtfs_t* gtfs, log_entry_t* entry) {
    if (!gtfs || !entry) return -1;
    string filepath = gtfs->dirname + "/" + entry->filename;
    
    // Open the file, creating it if necessary
    // The file might not exist if it was created but never properly closed
    int fd = open(filepath.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to open file " << filepath << " for replay\n");
        return -1;
    }
    
    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        close(fd);
        return -1;
    }
    
    int required_size = entry->offset + entry->length;
    if (file_stat.st_size < required_size) {
        if (ftruncate(fd, required_size) < 0) {
            VERBOSE_PRINT(do_verbose, "Failed to extend file to " << required_size << " bytes\n");
            close(fd);
            return -1;
        }
    }
    if (lseek(fd, entry->offset, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    
    ssize_t written = write(fd, entry->data, entry->length);
    if (written != entry->length) {
        VERBOSE_PRINT(do_verbose, "Failed to write complete data during replay\n");
        close(fd);
        return -1;
    }
    
    // SYNC
    fsync(fd);
    close(fd);
    
    return 0;
}

log_entry_t* parse_log_entry_from_buffer(char* buffer, size_t buffer_size, size_t* current_pos) {
    // This standard spacing is described in the .hpp file for log entries
    const size_t MIN_ENTRY_SIZE = 4 + 4 + 8 + 4 + 4 + 4 + 4 + 4; 
    if (*current_pos + MIN_ENTRY_SIZE > buffer_size) {
        return NULL;
    }
    size_t pos = *current_pos;
    
    // Read and verify magic number
    // This is our first line of defense against corrupted logs
    // TODO: NOT SURE IF WE NEED BUT CAN BE USEFULE
    uint32_t magic;
    memcpy(&magic, buffer + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    
    const uint32_t LOG_MAGIC = MAGIC_GTFS;  
    if (magic != LOG_MAGIC) {
        VERBOSE_PRINT(do_verbose, "Invalid magic number at position " << *current_pos << "\n");
        return NULL;
    }
    
    //POPULATE ALL FIELDS HERE
    uint32_t type_raw;
    memcpy(&type_raw, buffer + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    log_type_t type = (log_type_t)type_raw;

    unsigned long txn_id;
    memcpy(&txn_id, buffer + pos, sizeof(unsigned long));
    pos += sizeof(unsigned long);

    uint32_t is_committed;
    memcpy(&is_committed, buffer + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    uint32_t filename_len;
    memcpy(&filename_len, buffer + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    if (filename_len > MAX_FILENAME_LEN || pos + filename_len > buffer_size) {
        return NULL;  // Sanity check failed
    }
    
    char filename_buf[MAX_FILENAME_LEN + 1];
    memcpy(filename_buf, buffer + pos, filename_len);
    filename_buf[filename_len] = '\0';
    pos += filename_len;

    int32_t offset, length;
    memcpy(&offset, buffer + pos, sizeof(int32_t));
    pos += sizeof(int32_t);
    memcpy(&length, buffer + pos, sizeof(int32_t));
    pos += sizeof(int32_t);
    
    if (pos + length + sizeof(uint32_t) > buffer_size) {
        return NULL;
    }
    
    // Data payload
    char* data = (char*)malloc(length);
    if (!data) {
        return NULL;
    }
    memcpy(data, buffer + pos, length);
    pos += length;
    
    // now here comes the checksum (necessary to check for corrupted data)
    uint32_t stored_checksum;
    memcpy(&stored_checksum, buffer + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    
    uint32_t computed_checksum = compute_simple_checksum(buffer + *current_pos, pos - *current_pos - sizeof(uint32_t));
    
    if (stored_checksum != computed_checksum) {
        VERBOSE_PRINT(do_verbose, "Checksum mismatch for entry at position " << *current_pos << "\n");
        free(data);
        return NULL;
    }
 
    log_entry_t* entry = new log_entry_t();
    entry->type = type;
    entry->txn_id = txn_id;
    entry->is_committed = (is_committed != 0);
    entry->filename = string(filename_buf);
    entry->offset = offset;
    entry->length = length;
    entry->data = data;
    
    *current_pos = pos;
    return entry;
}
int write_log_entry_to_disk(gtfs_t* gtfs, log_entry_t* entry) {
    if (!gtfs || !entry) {
        return -1;
    }
    
    const uint32_t LOG_MAGIC = MAGIC_GTFS;  // "HKNG" in hex
    
    size_t total_size = sizeof(uint32_t) +  // magic
                       sizeof(uint32_t) +   // type
                       sizeof(unsigned long) + // txn_id
                       sizeof(uint32_t) +   // is_committed
                       sizeof(uint32_t) +   // filename_len
                       entry->filename.length() + // filename
                       sizeof(int32_t) +    // offset
                       sizeof(int32_t) +    // length
                       entry->length +      // data
                       sizeof(uint32_t);    // checksum
    
    char* buffer = (char*)malloc(total_size);
    if (!buffer) {
        return -1;
    }
    
    size_t pos = 0;
    
    // Write to shared memory now
    memcpy(buffer + pos, &LOG_MAGIC, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    uint32_t type_val = (uint32_t)entry->type;
    memcpy(buffer + pos, &type_val, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    memcpy(buffer + pos, &entry->txn_id, sizeof(unsigned long));
    pos += sizeof(unsigned long);

    uint32_t committed_val = entry->is_committed ? 1 : 0;
    memcpy(buffer + pos, &committed_val, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    uint32_t filename_len = entry->filename.length();
    memcpy(buffer + pos, &filename_len, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    memcpy(buffer + pos, entry->filename.c_str(), filename_len);
    pos += filename_len;

    memcpy(buffer + pos, &entry->offset, sizeof(int32_t));
    pos += sizeof(int32_t);
    memcpy(buffer + pos, &entry->length, sizeof(int32_t));
    pos += sizeof(int32_t);

    memcpy(buffer + pos, entry->data, entry->length);
    pos += entry->length;

    uint32_t checksum = compute_simple_checksum(buffer, pos);
    memcpy(buffer + pos, &checksum, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    ssize_t written = write(gtfs->log_fd, buffer, total_size);
    free(buffer);
    if (written != (ssize_t)total_size) {
        VERBOSE_PRINT(do_verbose, "Failed to write complete log entry\n");
        return -1;
    }
    
    // SYNC
    fsync(gtfs->log_fd);
    return 0;
}

gtfs_t* gtfs_init(string directory, int verbose_flag) {
    do_verbose = verbose_flag;
    VERBOSE_PRINT(do_verbose, "Initializing GTFileSystem inside directory " << directory << "\n");
    //TODO: Add any additional initializations and checks, and complete the functionality

    gtfs_t *gtfs = new gtfs_t();
    if (!gtfs) {
        VERBOSE_PRINT(do_verbose, "Failed to allocate gtfs structure\n");
        return NULL;
    }

    gtfs->dirname = directory;

    // Metdata, Log, and Lock
    gtfs->path_to_log = directory + "/gtfs_log";
    gtfs->lock_file_path = directory + "/gtfs_lock";
    gtfs->metadata_file_path = directory + "/gtfs_metadata";
    gtfs->next_txn_id = 1;

    gtfs->lock_fd = open(gtfs->lock_file_path.c_str(), O_RDWR | O_CREAT, 0666);
    if (gtfs->lock_fd < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to create lock file\n");
        delete gtfs;
        return NULL;
    }

    if (acquire_fs_lock(gtfs) < 0) { // acqurie lock before modification
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock in gtfs_init()\n");
        close(gtfs->lock_fd);
        delete gtfs;
        return NULL;
    }

    if (load_metadata(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to load metadata\n");
        release_fs_lock(gtfs);
        close(gtfs->lock_fd);
        delete gtfs;
        return NULL;
    }

    // Need to perform crash recovery here
    gtfs->log_fd = open(gtfs->path_to_log.c_str(), O_RDWR | O_CREAT, 0666);
    if (gtfs->log_fd < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to open/create log file\n");
        release_fs_lock(gtfs);
        close(gtfs->lock_fd);
        delete gtfs;
        return NULL;
    }
    if (replay_log(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to replay log during recovery\n");
        close(gtfs->log_fd);
        release_fs_lock(gtfs);
        close(gtfs->lock_fd);
        delete gtfs;
        return NULL;
    }

    gtfs->log_fd = open(gtfs->path_to_log.c_str(), O_RDWR | O_CREAT | O_APPEND, 0666);
    if (gtfs->log_fd < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to open log file\n");
        release_fs_lock(gtfs);
        close(gtfs->lock_fd);
        delete gtfs;
        return NULL;
    }

    release_fs_lock(gtfs);

    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns non NULL.
    return gtfs;
}

int gtfs_clean(gtfs_t *gtfs) {
    int ret = -1;
    if (gtfs) {
        VERBOSE_PRINT(do_verbose, "Cleaning up GTFileSystem inside directory " << gtfs->dirname << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem does not exist\n");
        return ret;
    }

    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock for cleaning\n");
        return -1;
    }

    // for (auto* write : gtfs->pending_writes) {
    //     gtfs_abort_write_file(write);
    // }
    gtfs->pending_writes.clear();
    
    struct stat log_stat;
    if (fstat(gtfs->log_fd, &log_stat) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to stat log file\n");
        release_fs_lock(gtfs);
        return -1;
    }
    if (log_stat.st_size == 0) {
        VERBOSE_PRINT(do_verbose, "Log file is empty, nothing to clean\n");
        release_fs_lock(gtfs);
        return 0;
    }

    VERBOSE_PRINT(do_verbose, "Log file size before clean: " << log_stat.st_size << " bytes\n");
    close(gtfs->log_fd); // close log to reread all of it from start

    gtfs->log_fd = open(gtfs->path_to_log.c_str(), O_RDWR);
    if (gtfs->log_fd < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to reopen log file for cleaning\n");
        release_fs_lock(gtfs);
        return -1;
    }

    char* log_data = (char*)mmap(NULL, log_stat.st_size, PROT_READ, MAP_PRIVATE, gtfs->log_fd, 0);
    if (log_data == MAP_FAILED) { // map log file
        VERBOSE_PRINT(do_verbose, "Failed to mmap log file for cleaning\n");
        close(gtfs->log_fd);
        release_fs_lock(gtfs);
        return -1;
    }

    vector<log_entry_t*> entries_to_apply;
    size_t current_pos = 0;
    int total_entries = 0;

    while (current_pos < log_stat.st_size) {
        log_entry_t* entry = parse_log_entry_from_buffer(log_data, log_stat.st_size, &current_pos);
        
        if (!entry) {
            VERBOSE_PRINT(do_verbose, "Stopping parse at position " << current_pos << "\n");
            break;
        }
        
        total_entries++;
        
        if (entry->is_committed && entry->type == LOG_TYPE_REDO) { // apply committed redo entries
            entries_to_apply.push_back(entry);
            VERBOSE_PRINT(do_verbose, "Will apply: " << entry->filename << " offset=" << entry->offset << " length=" << entry->length << "\n");
        } else {
            delete entry;  // delete uncommitted or undo entries
        }
    }
    VERBOSE_PRINT(do_verbose, "Found " << total_entries << " total entries, " << entries_to_apply.size() << " to apply\n");

    int entries_applied = 0;
    for (log_entry_t* entry : entries_to_apply) {
        if (apply_log_entry_to_file(gtfs, entry) == 0) {
            entries_applied++;
        } else {
            VERBOSE_PRINT(do_verbose, "Warning: failed to apply entry for " << entry->filename << "\n");
        }
        delete entry;
    }
    VERBOSE_PRINT(do_verbose, "Successfully applied " << entries_applied << " out of " << entries_to_apply.size() << " entries\n");

    munmap(log_data, log_stat.st_size); // unmap once done

    if (ftruncate(gtfs->log_fd, 0) < 0) { // truncate log
        VERBOSE_PRINT(do_verbose, "Failed to truncate log file\n");
        close(gtfs->log_fd);
        release_fs_lock(gtfs);
        return -1;
    }

    // CRUCIAL STEP, force sync on disk
    fsync(gtfs->log_fd);
    sync();

    close(gtfs->log_fd);
    gtfs->log_fd = open(gtfs->path_to_log.c_str(), O_RDWR | O_CREAT | O_APPEND, 0666);
    if (gtfs->log_fd < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to reopen log file in append mode\n");
        release_fs_lock(gtfs);
        return -1;
    }
    
    VERBOSE_PRINT(do_verbose, "Log cleaned successfully - truncated from " << log_stat.st_size << " bytes to 0\n");
    
    release_fs_lock(gtfs);
    
    ret = 0;
    VERBOSE_PRINT(do_verbose, "Success\n");
    return ret;
}

file_t* gtfs_open_file(gtfs_t* gtfs, string filename, int file_length) {
    file_t *fl;
    if (gtfs) {
        VERBOSE_PRINT(do_verbose, "Opening file " << filename << " inside directory " << gtfs->dirname << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem does not exist\n");
        return NULL;
    }
    // Safety checks of inputs
    if (filename.length() > MAX_FILENAME_LEN) {
        VERBOSE_PRINT(do_verbose, "Filename too long (max " << MAX_FILENAME_LEN << " chars)\n");
        return NULL;
    }
    if (file_length <= 0) {
        VERBOSE_PRINT(do_verbose, "Invalid file length: " << file_length << "\n");
        return NULL;
    }

    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock\n");
        return NULL;
    }

    // Have we reached max amount of files within the directory?
    if (gtfs->file_table.size() >= MAX_NUM_FILES_PER_DIR) {
        VERBOSE_PRINT(do_verbose, "Maximum number of files reached\n");
        release_fs_lock(gtfs);
        return NULL;
    }

    bool file_exists = (gtfs->file_table.find(filename) != gtfs->file_table.end());
    // VERBOSE_PRINT(do_verbose, "File exists? " << file_exists <<"\n");
    
    // VERBOSE_PRINT(do_verbose, "file_table.find(filename)=" << gtfs->file_table.find(filename)->first
    //           << ", file_table.end()=" << gtfs->file_table.end()->first
    //           << ", file_exists=" << file_exists << endl);
    // VERBOSE_PRINT(do_verbose, "Found filename? " << (gtfs->file_table.find(filename)) << "\n");

    if (file_exists) { 
        // A file with the specific filename under the specified directory can only be opened by one process at a time
        
        if (gtfs->file_table[filename].is_open) {
            pid_t owner = gtfs->file_table[filename].owner_pid;
            if (kill(owner, 0) == 0) {
                VERBOSE_PRINT(do_verbose, "File is already open by other process\n");
                release_fs_lock(gtfs);
                return NULL;
            } else {
                VERBOSE_PRINT(do_verbose, "Clearing orphaned open flag from dead process\n");
                gtfs->file_table[filename].is_open = false;
                gtfs->file_table[filename].owner_pid = 0;
            }
        }

        // If the specified file_length is smaller than the existing file length, then this operation should not be permitted
        if (file_length < gtfs->file_table[filename].length) {
            VERBOSE_PRINT(do_verbose, "Cannot shrink file (would lose data)\n");
            release_fs_lock(gtfs);
            return NULL;
        }
    } else { // we creating the file for first time
        file_metadata_t meta;
        meta.filename = filename;
        meta.length = file_length;
        meta.is_open = false;
        meta.open_count = 0;
        meta.owner_pid = 0;
        gtfs->file_table[filename] = meta;
    }
    //constrcut fiel path
    string filepath = gtfs->dirname + "/" + filename;

    // Open/Create file
    int fd = open(filepath.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) { // faield to open/create file
        VERBOSE_PRINT(do_verbose, "Failed to open/create file\n");
        if (!file_exists) { // erase entry if it didn't exist
            gtfs->file_table.erase(filename);
        }
        release_fs_lock(gtfs);
        return NULL;
    }

    //stats time (for file size and modifying if necessary)
    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        // classic cleanup procedure
        VERBOSE_PRINT(do_verbose, "Failed to stat file\n");
        close(fd);
        if (!file_exists) {
            gtfs->file_table.erase(filename);
        }
        release_fs_lock(gtfs);
        return NULL;
    }
    // now modify/extend that file if needed
    if (file_stat.st_size < file_length) {
        if (ftruncate(fd, file_length) < 0) {
            // classic cleanup procedure
            VERBOSE_PRINT(do_verbose, "Failed to extend file to " << file_length << " bytes\n");
            close(fd);
            if (!file_exists) {
                gtfs->file_table.erase(filename);
            }
            release_fs_lock(gtfs);
            return NULL;
        }
        gtfs->file_table[filename].length = file_length;
    }

    // time to map
    char* mapped_data = (char*)mmap(NULL, file_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_data == MAP_FAILED) { // if mapped failed than handle accordingly
        // same stuff again
        VERBOSE_PRINT(do_verbose, "Failed to mmap file\n");
        close(fd);
        if (!file_exists) {
            gtfs->file_table.erase(filename);
        }
        release_fs_lock(gtfs);
        return NULL;
    }

    // create the handle
    fl = new file_t();
    fl->filename = filename;
    fl->file_length = file_length;
    fl->mapped_data_seg = mapped_data;
    fl->fd = fd;
    fl->parent_gtfs = gtfs;

    gtfs->file_table[filename].is_open = true;
    gtfs->file_table[filename].owner_pid = (int)getpid();
    gtfs->file_table[filename].open_count++;
    // persist the metadata (this is the huge bottleneck, but I don't think it will matter)
    if (save_metadata(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Warning: failed to save metadata\n");
        // TODO: don't really know what to do if metadata doesn't persist
    }

    release_fs_lock(gtfs);

    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns non NULL.
    return fl;
}

int gtfs_close_file(gtfs_t* gtfs, file_t* fl) {
    int ret = -1;
    if (gtfs and fl) {
        VERBOSE_PRINT(do_verbose, "Closing file " << fl->filename << " inside directory " << gtfs->dirname << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem or file does not exist\n");
        return ret;
    }

    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock\n");
        return -1;
    }
    if (gtfs->file_table.find(fl->filename) == gtfs->file_table.end()) {
        VERBOSE_PRINT(do_verbose, "File not found in file table\n");
        release_fs_lock(gtfs);
        return -1;
    }

    if (fl->mapped_data_seg) {
        // if (msync(fl->mapped_data_seg, fl->file_length, MS_SYNC) < 0) { // persist to disk
        //     VERBOSE_PRINT(do_verbose, "Warning: msync failed during close\n");
        //     // TODO: Don't really know what to do if this fails
        // }
        if (munmap(fl->mapped_data_seg, fl->file_length) < 0) {
            VERBOSE_PRINT(do_verbose, "Warning: munmap failed\n");
            // TODO: Not good either, can't really do much about it though
        }
        fl->mapped_data_seg = NULL;

    }

    if (fl->fd >= 0) { // again flush file descriptor to ensure persistence to disk
        // fsync(fl->fd);
        close(fl->fd);
        fl->fd = -1;
    }
    gtfs->file_table[fl->filename].is_open = false;
    gtfs->file_table[fl->filename].owner_pid = 0;


    if (save_metadata(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Warning: failed to save metadata after close\n");
        // TODO: again dont really know what to do if we fail to persist the metadata
    }
    release_fs_lock(gtfs);
    delete fl;
    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns 0.
    ret = 0;
    return ret;
}

int gtfs_remove_file(gtfs_t* gtfs, file_t* fl) {
    int ret = -1;
    if (gtfs and fl) {
        VERBOSE_PRINT(do_verbose, "Removing file " << fl->filename << " inside directory " << gtfs->dirname << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem or file does not exist\n");
        return ret;
    }
    // We actually don't care about ermongi log entries since they will be handled in gtfs_clean or replay log
    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock\n");
        return -1;
    }
    if (gtfs->file_table.find(fl->filename) == gtfs->file_table.end()) { // check if file exists in our dir
        VERBOSE_PRINT(do_verbose, "File not found in file table\n");
        release_fs_lock(gtfs);
        return -1;
    }
    if (gtfs->file_table[fl->filename].is_open) { // check if open
        VERBOSE_PRINT(do_verbose, "Cannot remove file that is currently open\n");
        release_fs_lock(gtfs);
        return -1;
    }

    string filepath = gtfs->dirname + "/" + fl->filename;
    if (unlink(filepath.c_str()) < 0) { // actual deleting of file
        VERBOSE_PRINT(do_verbose, "Failed to delete file from disk\n");
        release_fs_lock(gtfs);
        return -1;
    }

    gtfs->file_table.erase(fl->filename); // erase metadata and update
    if (save_metadata(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Warning: failed to save metadata after removal\n");
    }

    release_fs_lock(gtfs);
    delete fl;
    
    ret = 0;
    VERBOSE_PRINT(do_verbose, "Success\n"); //On success return 0
    return ret;
}

char* gtfs_read_file(gtfs_t* gtfs, file_t* fl, int offset, int length) {
    char* ret_data = NULL;
    if (gtfs and fl) {
        VERBOSE_PRINT(do_verbose, "Reading " << length << " bytes starting from offset " << offset << " inside file " << fl->filename << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem or file does not exist\n");
        return NULL;
    }
    // Limtis check
    if (offset < 0 || length < 0) {
        VERBOSE_PRINT(do_verbose, "Invalid offset or length\n");
        return NULL;
    }
    if (offset + length > fl->file_length) {
        VERBOSE_PRINT(do_verbose, "Read exceeds file bounds\n");
        return NULL;
    }

    if (!fl->mapped_data_seg) { // check if mapped data segment is there
        VERBOSE_PRINT(do_verbose, "File not memory-mapped\n");
        return NULL;
    }
    ret_data = (char*)malloc(length + 1);  // +1 for null terminator
    if (!ret_data) {
        VERBOSE_PRINT(do_verbose, "Failed to allocate memory for read\n");
        return NULL;
    }
    memcpy(ret_data, fl->mapped_data_seg + offset, length); // read file
    ret_data[length] = '\0';  // Null-terminate for safety (text files)


    if (acquire_fs_lock(gtfs) < 0) {
        free(ret_data);
        return NULL;
    }
    
    // che for pending wriets
    if (gtfs->pending_writes.find(fl->filename) != gtfs->pending_writes.end()) {
        vector<write_t*>& writes = gtfs->pending_writes[fl->filename];
        for (write_t* write : writes) {
            if (write->is_synced) {
                continue;
            }
            int write_start = write->offset;
            int write_end = write->offset + write->length;
            int read_start = offset;
            int read_end = offset + length;
            if (write_start < read_end && write_end > read_start) {
                int overlap_start = max(write_start, read_start);
                int overlap_end = min(write_end, read_end);
                int overlap_length = overlap_end - overlap_start;
                int src_offset = overlap_start - write_start;  // Offset within write->data
                int dst_offset = overlap_start - read_start;   // Offset within ret_data
                memcpy(ret_data + dst_offset, write->data + src_offset, overlap_length);
                
                VERBOSE_PRINT(do_verbose, "Applied pending write: offset=" << overlap_start << " length=" << overlap_length << "\n");
            }
        }
    }
    
    release_fs_lock(gtfs);

    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns pointer to data read.
    return ret_data; // caller must free the data after
}

write_t* gtfs_write_file(gtfs_t* gtfs, file_t* fl, int offset, int length, const char* data) {
    write_t *write_id = NULL;
    if (gtfs and fl) {
        VERBOSE_PRINT(do_verbose, "Writting " << length << " bytes starting from offset " << offset << " inside file " << fl->filename << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem or file does not exist\n");
        return NULL;
    }
    //Limits Check
    if (offset < 0 || length <= 0) {
        VERBOSE_PRINT(do_verbose, "Invalid offset or length\n");
        return NULL;
    }
    if (offset + length > fl->file_length) {
        VERBOSE_PRINT(do_verbose, "Write exceeds file bounds\n");
        return NULL;
    }
    if (!data) {
        VERBOSE_PRINT(do_verbose, "Data pointer is NULL\n");
        return NULL;
    }
    if (!fl->mapped_data_seg) {
        VERBOSE_PRINT(do_verbose, "File not memory-mapped\n");
        return NULL;
    }

    //lock
    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock\n");
        return NULL;
    }

    write_id = new write_t(); // here comes write_t
    if (!write_id) {
        VERBOSE_PRINT(do_verbose, "Failed to allocate write structure\n");
        release_fs_lock(gtfs);
        return NULL;
    }
    write_id->filename = fl->filename;
    write_id->offset = offset;
    write_id->length = length;
    write_id->parent_file = fl;
    write_id->is_synced = false;
    write_id->txn_id = gtfs->next_txn_id++;

    write_id->data = (char*)malloc(length); // get data for write_t
    if (!write_id->data) {
        VERBOSE_PRINT(do_verbose, "Failed to allocate data buffer\n");
        delete write_id;
        release_fs_lock(gtfs);
        return NULL;
    }
    memcpy(write_id->data, data, length);

    // we are gonna save original data as well
    write_id->original_data = (char*)malloc(length);
    if (!write_id->original_data) {
        VERBOSE_PRINT(do_verbose, "Failed to allocate original data buffer\n");
        free(write_id->data);
        delete write_id;
        release_fs_lock(gtfs);
        return NULL;
    }
    memcpy(write_id->original_data, fl->mapped_data_seg + offset, length);

    gtfs->pending_writes[fl->filename].push_back(write_id);

    /**
     * Need an undo log entry in case we abort the write
     * For this need to make a new log_entry_t with all the information we had above and the data
     */
    write_id->undo_entry = new log_entry_t();
    write_id->undo_entry->type = LOG_TYPE_UNDO;
    write_id->undo_entry->filename = fl->filename;
    write_id->undo_entry->offset = offset;
    write_id->undo_entry->length = length;
    write_id->undo_entry->txn_id = write_id->txn_id;
    write_id->undo_entry->is_committed = false;
    write_id->undo_entry->data = (char*)malloc(length);
    if (!write_id->undo_entry->data) { // if we fail to malloc again
        VERBOSE_PRINT(do_verbose, "Failed to allocate undo entry data\n");
        free(write_id->original_data);
        free(write_id->data);
        delete write_id->undo_entry;
        delete write_id;
        release_fs_lock(gtfs);
        return NULL;
    }

    if (write_log_entry_to_disk(gtfs, write_id->undo_entry) < 0) { // now put this log entry on disk
        VERBOSE_PRINT(do_verbose, "Failed to write undo log entry\n");
        free(write_id->undo_entry->data);
        free(write_id->original_data);
        free(write_id->data);
        delete write_id->undo_entry;
        delete write_id;
        release_fs_lock(gtfs);
        return NULL;
    }

    // now write to memory mapped region 
    // TODO: May be incorrect to sync to disk at this point.
    // memcpy(fl->mapped_data_seg + offset, data, length);
    // msync(fl->mapped_data_seg + offset, length, MS_ASYNC);

    write_id->redo_entry = NULL;
    release_fs_lock(gtfs); // TODO: potential race condition because of ASYNC
    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns non NULL.
    return write_id;
}

int gtfs_sync_write_file(write_t* write_id) {
    int ret = -1;
    if (write_id) {
        VERBOSE_PRINT(do_verbose, "Persisting write of " << write_id->length << " bytes starting from offset " << write_id->offset << " inside file " << write_id->filename << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "Write operation does not exist\n");
        return ret;
    }
    if (write_id->is_synced) {
        VERBOSE_PRINT(do_verbose, "Write already synced\n");
        return write_id->length;  // Already done
    }
    if (!write_id->parent_file || !write_id->parent_file->parent_gtfs) {
        VERBOSE_PRINT(do_verbose, "Invalid parent file or gtfs\n");
        return -1;
    }
    gtfs_t* gtfs = write_id->parent_file->parent_gtfs;
    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock\n");
        return -1;
    }

    // now REDO log entry in case we crash
    write_id->redo_entry = new log_entry_t();
    write_id->redo_entry->type = LOG_TYPE_REDO;
    write_id->redo_entry->filename = write_id->filename;
    write_id->redo_entry->offset = write_id->offset;
    write_id->redo_entry->length = write_id->length;
    write_id->redo_entry->txn_id = write_id->txn_id;
    write_id->redo_entry->is_committed = true; 
    write_id->redo_entry->data = (char*)malloc(write_id->length);
    if (!write_id->redo_entry->data) {
        VERBOSE_PRINT(do_verbose, "Failed to allocate redo entry data\n");
        delete write_id->redo_entry;
        write_id->redo_entry = NULL;
        release_fs_lock(gtfs);
        return -1;
    }
    memcpy(write_id->redo_entry->data, write_id->data, write_id->length);

    if (write_log_entry_to_disk(gtfs, write_id->redo_entry) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to write redo log entry\n");
        free(write_id->redo_entry->data);
        delete write_id->redo_entry;
        write_id->redo_entry = NULL;
        release_fs_lock(gtfs);
        return -1;
    }

    memcpy(write_id->parent_file->mapped_data_seg + write_id->offset, write_id->data, write_id->length);

    if (msync(write_id->parent_file->mapped_data_seg + write_id->offset, write_id->length, MS_SYNC) < 0) {
        VERBOSE_PRINT(do_verbose, "Warning: msync failed during sync_write\n");
        // TODO: is it ok if msync fails
    }
    fsync(write_id->parent_file->fd);
    // syncing point
    // Remove from pending writes
    auto& writes = gtfs->pending_writes[write_id->filename];
    writes.erase(std::remove(writes.begin(), writes.end(), write_id), writes.end());
    write_id->is_synced = true;
    release_fs_lock(gtfs);
    ret = write_id->length;

    // CLEANUP
    if (write_id->data) {
        free(write_id->data);
        write_id->data = NULL;
    }
    if (write_id->original_data) {
        free(write_id->original_data);
        write_id->original_data = NULL;
    }
    if (write_id->undo_entry) {
        delete write_id->undo_entry;
        write_id->undo_entry = NULL;
    }
    if (write_id->redo_entry) {
        delete write_id->redo_entry;
        write_id->redo_entry = NULL;
    }
    delete write_id;


    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns number of bytes written.
    return ret;
}

int gtfs_abort_write_file(write_t* write_id) {
    int ret = -1;
    if (write_id) {
        VERBOSE_PRINT(do_verbose, "Aborting write of " << write_id->length << " bytes starting from offset " << write_id->offset << " inside file " << write_id->filename << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "Write operation does not exist\n");
        return ret;
    }
    //TODO: Add any additional initializations and checks, and complete the functionality
    // error checking
    if(!write_id->parent_file || !write_id->parent_file->parent_gtfs) {
        VERBOSE_PRINT(do_verbose, "Invalid parent/gtfs\n");
        return ret;
    }

    if(!write_id->original_data) {
        VERBOSE_PRINT(do_verbose, "No original data to writeback\n");
        return ret;
    }

    if(write_id->is_synced) {
        VERBOSE_PRINT(do_verbose, "Write already synced, cannot abort\n");
        return ret;
    }


    gtfs_t* gtfs = write_id->parent_file->parent_gtfs;
    
    if (acquire_fs_lock(gtfs) < 0) {
        VERBOSE_PRINT(do_verbose, "Failed to acquire lock in abort_write\n");
        return ret;
    }

    // remove from pending writes
    auto& writes = gtfs->pending_writes[write_id->filename];
    writes.erase(std::remove(writes.begin(), writes.end(), write_id), writes.end());

    // copy the original data back
    memcpy(write_id->parent_file->mapped_data_seg + write_id->offset, write_id->original_data, write_id->length);

    if (msync(write_id->parent_file->mapped_data_seg + write_id->offset, write_id->length, MS_SYNC) < 0) {
        VERBOSE_PRINT(do_verbose, "Warning: msync failed during abort\n");
        // TODO: Also dont know what to do if this failed
    }

    fsync(write_id->parent_file->fd);

    //TODO: More log book-keeping stuff needed?

    // CLEANUP
    release_fs_lock(gtfs);
    ret = 0;
     if (write_id->data) {
        free(write_id->data);
        write_id->data = NULL;
    }
    
    if (write_id->original_data) {
        free(write_id->original_data);
        write_id->original_data = NULL;
    }
    
    if (write_id->undo_entry) {
        delete write_id->undo_entry;
        write_id->undo_entry = NULL;
    }
    
    if (write_id->redo_entry) {
        delete write_id->redo_entry;
        write_id->redo_entry = NULL;
    }
    
    delete write_id;
    VERBOSE_PRINT(do_verbose, "Success.\n"); //On success returns 0.
    return ret;
}

// BONUS: Implement below API calls to get bonus credits

int gtfs_clean_n_bytes(gtfs_t *gtfs, int bytes){
    int ret = -1;
    if (gtfs) {
        VERBOSE_PRINT(do_verbose, "Cleaning up [ " << bytes << " bytes ] GTFileSystem inside directory " << gtfs->dirname << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "GTFileSystem does not exist\n");
        return ret;
    }

    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns 0.
    return ret;
}

int gtfs_sync_write_file_n_bytes(write_t* write_id, int bytes){
    int ret = -1;
    if (write_id) {
        VERBOSE_PRINT(do_verbose, "Persisting [ " << bytes << " bytes ] write of " << write_id->length << " bytes starting from offset " << write_id->offset << " inside file " << write_id->filename << "\n");
    } else {
        VERBOSE_PRINT(do_verbose, "Write operation does not exist\n");
        return ret;
    }

    VERBOSE_PRINT(do_verbose, "Success\n"); //On success returns 0.
    return ret;
}

