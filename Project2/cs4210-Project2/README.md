# TinyFile Service and Library
## Project 2: Inter-process Communication Services
* Barnabe M., Padraig L.


### Dependencies

* snappy-c library: must be cloned into the ./lib/ folder before use

### Building the Project

The project uses a Makefile to compile all components. From the `src/` directory, use the following commands:

**Build everything:**
```bash
make
```

**Build specific components:**
```bash
make tinyfile      # Build only the TinyFile service
make libtinyfile   # Build only the client library
make sample_app    # Build only the sample application
```

**Clean build artifacts:**
```bash
make clean         # Remove all build files and shared memory segments
```

**View help:**
```bash
make help          # Display available targets and usage examples
```

All executables will be placed in the `../bin/` directory after compilation. Output files and libraries will be placed in in the `./obj/` directory.
### TinyFile service

The service must be started before calls can be made to it through the TinyFile library. It can be initialized with configurable parameters that define the number and size of shared memory segments available for concurrent client operations.

* Parameters:
  * `--n_sms`: # shared memory segments
  * `--sms_size`: the size of shared memory segments, in the unit of bytes.
  * `-f` : run the service in the forground
* e.g. running TinyFile service in blocking mode: `./tinyfile --n_sms 5 --sms_size 32`

### TinyFile Library API

The TinyFile library provides a simple interface for clients to communicate with the TinyFile compression service through shared memory. The API supports both synchronous (blocking) and asynchronous (non-blocking) compression operations.

#### Core Functions

##### `int tinyfile_init(client_context_t **ctx_out)`
Initializes the client context and establishes connection to the TinyFile service.

- Creates a client-specific response queue for receiving service responses
- Maps the service control segment to access shared memory configuration
- **Parameters:**
  - `ctx_out`: Pointer to store the initialized client context
- **Returns:** `0` on success, `-1` on failure
- **Usage:** Must be called before any compression operations

---

##### `int tinyfile_compress_sync(client_context_t *ctx, char *output_file, char *input_file)`
Performs synchronous (blocking) file compression.

- Blocks the calling thread until compression is complete
- **Parameters:**
  - `ctx`: Client context from `tinyfile_init()`
  - `output_file`: Path where compressed output will be written
  - `input_file`: Path to file to be compressed
- **Returns:** `0` on success, `-1` on failure
- **Usage:** Suitable for sequential processing or when result is immediately needed

---

##### `int tinyfile_compress_async(client_context_t *ctx, char *output_file, char *input_file, compress_callback_t callback, void *user_data)`
Performs asynchronous (non-blocking) file compression.

- Returns immediately and executes compression in a background thread
- **Parameters:**
  - `ctx`: Client context from `tinyfile_init()`
  - `output_file`: Path where compressed output will be written
  - `input_file`: Path to file to be compressed
  - `callback`: Function to call upon completion (can be `NULL`)
  - `user_data`: User-defined data passed to callback (can be `NULL`)
- **Returns:** Job ID on success, `-1` on failure
- **Usage:** Suitable for concurrent processing or when caller has other work to perform

---

##### `void tinyfile_wait_all_async(client_context_t *ctx)`
Blocks until all pending asynchronous compression jobs complete.

- **Parameters:**
  - `ctx`: Client context with active async jobs
- **Usage:** Call before cleanup to ensure all operations finish

---

##### `void cleanup_client_context(client_context_t *ctx)`
Releases all resources associated with the client context.

- Waits for any pending async jobs to complete
- Unmaps shared memory segments and closes file descriptors
- **Parameters:**
  - `ctx`: Client context to clean up
- **Usage:** Must be called when done with the TinyFile service

---


#### A sample Application to use the service using TinyFile Library

* The number and size of the shared memory segments should be as configurable runtime parameters.
* The path to indicate the file(s) to be compressed
* Parameters:
  * `--state`: SYNC | ASYNC
  * `--file`: specify the file path to be compressed
  * `--files`: specify the file containing the list of files to compressed. 
* For example, if you were to run a sample application:
  * for single file request: `./sample_app --file ./aos_is_fun.txt --state SYNC`
  * for multi-file request: `./sample_app --files ./filelist.txt --state SYNC`
* The files put in the filelist file should be listed in a column with the relative path from where you run the sample app. The following is an example for running the sample app from within bin/ directory:
```
$ cat filelist.txt
input/Large.txt
input/Huge.jpg

```
---
