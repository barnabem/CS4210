#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "tinyfile_lib.h"
#include "snappy.h"

#define MAX_FILES 100

double get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1e6) + (ts.tv_nsec / 1e3);
}

void construct_output_path(const char *input_file, char *output_file, size_t max_len) {
    const char *basename = strrchr(input_file, '/');
    basename = basename ? basename + 1 : input_file;
    snprintf(output_file, max_len, "../bin/output/%s.snappy", basename); // ad .snappy at edn
    // printf("Output file: %s\n", output_file);
}

void print_usage(const char *prog_name) { // some descriptors
    fprintf(stderr, "Usage: %s --file <filepath> --state <SYNC|ASYNC>\n", prog_name);
    fprintf(stderr, "   OR: %s --files <filelist> --state <SYNC|ASYNC>\n", prog_name);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --file <filepath>        Compress a single file\n");
    fprintf(stderr, "  --files filelist.txt     Compress a batch of files\n");
    fprintf(stderr, "  --state <mode>           SYNC for synchronous, ASYNC for asynchronous\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --file bin/input/Tiny.txt --state SYNC\n", prog_name);
}

void compression_complete_callback(int status, const char *output_file, void *user_data) {
    double *start_time = (double*)user_data;
    double end_time = get_time_us();
    double cst = (end_time - *start_time) / 1000000.0;
    
    if (status == 0) {
        printf("\nASYNC Compression successful!\n");
        printf("Output file: %s\n", output_file);
        printf("Client-side Service Time (CST): %.6f seconds\n", cst);
    } else {
        fprintf(stderr, "\nASYNC Compression FAILED!\n");
    }
    
    free(start_time);  // Clean up user data
}

int compress_file_async(client_context_t *ctx, const char *input_file) { // --state ASYNC for files
    char output_file[512];
    construct_output_path(input_file, output_file, sizeof(output_file));
    
    printf("\n=== Starting ASYNC compression: %s ===\n", input_file);
    
    double *start_time = malloc(sizeof(double));
    *start_time = get_time_us();
    
    int job_id = tinyfile_compress_async(ctx, output_file, (char*)input_file, compression_complete_callback, start_time);
    
    if (job_id < 0) {
        fprintf(stderr, "Failed to start async compression\n");
        free(start_time);
        return -1;
    }
    
    printf("Async job %d started, main thraed continue...\n", job_id);
    return 0;
}

int compress_file_sync(client_context_t *ctx, const char *input_file) { // --state SYNC for files
    char output_file[512];
    construct_output_path(input_file, output_file, sizeof(output_file));
    
    printf("\n=== Compressing: %s ===\n", input_file);
    
    double start_time = get_time_us();
    
    int result = tinyfile_compress_sync(ctx, output_file, (char*)input_file);
    
    double end_time = get_time_us();
    double cst = (end_time - start_time) / 1000000.0; // convert to seconds
    
    if (result == 0) {
        printf("SYNC Compression successful!\n");
        printf("Output file: %s\n", output_file);
        printf("CST: %.6f seconds\n", cst);
    } else {
        fprintf(stderr, "SYNC Compression FAILED!\n");
    }
    
    return result;
}

int read_file_list(const char *list_file, char ***files_out, int *count_out) { // for reading all files needed for compression
    FILE *fp = fopen(list_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open file list: %s\n", list_file);
        return -1;
    }
    char **files = malloc(MAX_FILES * sizeof(char*));
    if (!files) {
        fclose(fp);
        return -1;
    }
    int count = 0;
    char line[512];
    while (fgets(line,sizeof(line), fp) &&count < MAX_FILES) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] =='\0' ||  line[0] == '#') {
            continue;
        }
        files[count] = strdup(line);
        if (!files[count]) {
            for (int i = 0; i < count; i++) {
                free(files[i]);
            }
            free(files);
            fclose(fp);
            return -1;
        }
        count++;
    }
    fclose(fp);
    *files_out = files;
    *count_out = count;
    printf("Loaded %d files from %s\n", count, list_file);
    return 0;
}

void free_file_list(char **files, int count) {
    for (int i = 0; i < count; i++) {
        free(files[i]);
    }
    free(files);
}

int main(int argc, char *argv[]) {
    char *file_path = NULL;
    char *files_path = NULL;
    char *state = NULL;
    int is_sync = 1; // default -> SYNC
    
    // parse command line args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file_path = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) {
            files_path = argv[++i];
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state = argv[++i];
            if (strcmp(state, "ASYNC") == 0) {
                is_sync = 0;
            } else if (strcmp(state, "SYNC") != 0) {
                fprintf(stderr, "Error: Invalid state '%s'. Must be SYNC or ASYNC.\n", state);
                print_usage(argv[0]);
                return 1;
            }
        }
    }
    if (file_path == NULL && files_path == NULL) {
        fprintf(stderr, "Error: Missing required arguments.\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (file_path != NULL && files_path != NULL) {
        fprintf(stderr, "Error: Cannot specify both --file and --files.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    printf("=================================================\n");
    printf("TinyFile Sample Application\n");
    printf("=================================================\n");
    printf("Mode: %s\n", is_sync ? "SYNCHRONOUS" : "ASYNCHRONOUS");
    printf("PID: %d\n", getpid());
    printf("=================================================\n");

    client_context_t *ctx = NULL;
    if (tinyfile_init(&ctx) != 0) {
        fprintf(stderr, "Failed to initialize TinyFile client.\n");
        fprintf(stderr, "Is the TinyFile service running?\n");
        return 1;
    }

    int result = 0;

    if (file_path != NULL) { //single file compress
        if (is_sync) { //SYNC
            result = compress_file_sync(ctx, file_path);
        } else { //ASYNC
            result = compress_file_async(ctx, file_path);
        
            // main thread simulating fakework
            printf("\n>>> Main thread doing other work while compression runs...\n");
            for (int i = 0; i < 5; i++) {
                printf(">>> Main thread working... (%d/5)\n", i+1);
                sleep(1);
            }
            printf(">>> Main thread done with other work\n\n");
        }

        
    } else if (files_path != NULL) { // multi-file compress
        char **files = NULL;
        int file_count = 0;
        
        if (read_file_list(files_path, &files, &file_count) != 0) {
            fprintf(stderr, "Failed to read file list\n");
            cleanup_client_context(ctx);
            return 1;
        }
        
        printf("\n=== Processing %d files ===\n", file_count);
        
        if (is_sync) {
            // SYNC mode: compress files sequentially
            double total_start = get_time_us();
            
            for (int i = 0; i < file_count; i++) {
                printf("\n[%d/%d] ", i+1, file_count);
                int file_result = compress_file_sync(ctx, files[i]);
                if (file_result != 0) {
                    result = -1;
                    fprintf(stderr, "Failed to compress: %s\n", files[i]);
                }
            }
            
            double total_end = get_time_us();
            double total_time = (total_end - total_start) / 1000000.0;
            
            printf("\n=== SYNC Summary ===\n");
            printf("Total files: %d\n", file_count);
            printf("Total time: %.6f seconds\n", total_time);
            printf("Average per file: %.6f seconds\n", total_time / file_count);
            
        } else {
            // ASYNC: start all compressions, let them run in parallel
            double total_start = get_time_us();
            
            int jobs_started = 0;
            for (int i = 0; i < file_count; i++) {
                printf("\n[%d/%d] ", i+1, file_count);
                int file_result = compress_file_async(ctx, files[i]);
                if (file_result >= 0) {
                    jobs_started++;
                } else {
                    result = -1;
                    fprintf(stderr, "Failed to start compression for: %s\n", files[i]);
                }
            }
            
            
            // Simulate other work while compressions happen in parallel
            // for (int i = 0; i < 5; i++) {
            //     printf(">>> Main thread working... (%d/5)\n", i+1);
            //     sleep(1);
            // }
            
            //wait for all async jobs to complete
            tinyfile_wait_all_async(ctx);
            
            double total_end = get_time_us();
            double total_time = (total_end - total_start) / 1000000.0;
            
            printf("\n=== ASYNC Summary ===\n");
            printf("Total files: %d\n", file_count);
            printf("Total time: %.6f seconds\n", total_time);
            printf("Average per file: %.6f seconds\n", total_time / file_count);
        }
        
        free_file_list(files, file_count);
    }
    
    cleanup_client_context(ctx);
    
    printf("\nApplication complete.\n");
    return result;
}