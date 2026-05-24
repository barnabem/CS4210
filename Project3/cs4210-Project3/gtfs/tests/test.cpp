#include "../src/gtfs.hpp"

// Assumes files are located within the current directory
string directory;
int verbose;

// **Test 1**: Testing that data written by one process is then successfully read by another process.
void writer() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    string filename = "test1.txt";
    file_t *fl = gtfs_open_file(gtfs, filename, 100);

    string str = "Hi, I'm the writer.\n";
    write_t *wrt = gtfs_write_file(gtfs, fl, 10, str.length(), str.c_str());
    gtfs_sync_write_file(wrt);

    gtfs_close_file(gtfs, fl);
}

void reader() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    string filename = "test1.txt";
    file_t *fl = gtfs_open_file(gtfs, filename, 100);

    string str = "Hi, I'm the writer.\n";
    char *data = gtfs_read_file(gtfs, fl, 10, str.length());
    if (data != NULL) {
        str.compare(string(data)) == 0 ? cout << PASS : cout << FAIL;
    } else {
        cout << FAIL;
    }
    gtfs_close_file(gtfs, fl);
}

void test_write_read() {
    int pid;
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(-1);
    }
    if (pid == 0) {
        writer();
        exit(0);
    }
    waitpid(pid, NULL, 0);
    reader();
}

// **Test 2**: Testing that aborting a write returns the file to its original contents.

void test_abort_write() {

    gtfs_t *gtfs = gtfs_init(directory, verbose);
    string filename = "test2.txt";
    file_t *fl = gtfs_open_file(gtfs, filename, 100);

    string str = "Testing string.\n";
    write_t *wrt1 = gtfs_write_file(gtfs, fl, 0, str.length(), str.c_str());
    gtfs_sync_write_file(wrt1);

    write_t *wrt2 = gtfs_write_file(gtfs, fl, 20, str.length(), str.c_str());
    gtfs_abort_write_file(wrt2);

    char *data1 = gtfs_read_file(gtfs, fl, 0, str.length());
    printf("data1 = '%s'\n", data1);  // Print data1
    if (data1 != NULL) {
        // First write was synced so reading should be successfull
        if (str.compare(string(data1)) != 0) {
            cout << FAIL;
        }
        // Second write was aborted and there was no string written in that offset
        char *data2 = gtfs_read_file(gtfs, fl, 20, str.length());
        printf("data2 = '%s'\n", data2);  // Print data2
        if (data2 == NULL) {
            cout << FAIL;
        } else if (string(data2).compare("") == 0) {
            cout << PASS;
        } else {
            cout << FAIL;
        }
    } else {
        cout << FAIL;
    }
    gtfs_close_file(gtfs, fl);
}

// **Test 3**: Testing that the logs are truncated.

void test_truncate_log() {

    gtfs_t *gtfs = gtfs_init(directory, verbose);
    string filename = "test3.txt";
    file_t *fl = gtfs_open_file(gtfs, filename, 100);

    string str = "Testing string.\n";
    write_t *wrt1 = gtfs_write_file(gtfs, fl, 0, str.length(), str.c_str());
    gtfs_sync_write_file(wrt1);

    write_t *wrt2 = gtfs_write_file(gtfs, fl, 20, str.length(), str.c_str());
    gtfs_sync_write_file(wrt2);

    cout << "Before GTFS cleanup\n";
    system("ls -l .");
    // system("ls -l ../bin");

    gtfs_clean(gtfs);

    cout << "After GTFS cleanup\n";
    system("ls -l .");
    // system("ls -l ../bin");

    cout << "If log is truncated: " << PASS << "If exactly same output:" << FAIL;

    gtfs_close_file(gtfs, fl);

}

void test_initialization() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    
    if (gtfs == NULL) {
        cout << "Initialization failed" << FAIL;
        return;
    }
    if (gtfs->dirname != directory) {
        cout << "Directory path not set correctly" << FAIL;
        return;
    }
    struct stat st;
    string lock_file = directory + "/gtfs_lock";
    string log_file = directory + "/gtfs_log";
    string meta_file = directory + "/gtfs_metadata";
    
    if (stat(lock_file.c_str(), &st) != 0) {
        cout << "Lock file not created" << FAIL;
        return;
    }
    
    if (stat(log_file.c_str(), &st) != 0) {
        cout << "Log file not created" << FAIL;
        return;
    }
    
    cout << PASS;
}

// TODO: Implement any additional tests

void test_crash_recovery() {
    int pid = fork();
    string str1 = "Before crash\n";
    string str2 = "CRASHING\n";
    if (pid == 0) {
        gtfs_t *gtfs = gtfs_init(directory, verbose);
        file_t *fl = gtfs_open_file(gtfs, "test4.txt", 200);
        
        write_t *wrt1 = gtfs_write_file(gtfs, fl, 0, str1.length(), str1.c_str());
        gtfs_sync_write_file(wrt1);
        
        write_t *wrt2 = gtfs_write_file(gtfs, fl, 50, str2.length(), str2.c_str());
        // gtfs_close_file(gtfs, fl);

        abort();
    }
    waitpid(pid, NULL, 0);
    
    // Recovery in parent process
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    file_t *fl = gtfs_open_file(gtfs, "test4.txt", 200);
    
    char *data1 = gtfs_read_file(gtfs, fl, 0, str1.length());
    // printf("data1 = '%s'\n", data1);  // Print data1
    char *data2 = gtfs_read_file(gtfs, fl, 50, str2.length());
    // printf("data2 = '%s'\n", data2);  // Print data1
    
    
    // First write should be recovered, second should not exist
    if (string(data1).compare(str1) == 0 && string(data2).compare("") == 0) {
        cout << PASS;
    } else {
        cout << FAIL;
    }
    gtfs_close_file(gtfs, fl);
}

void test_multiple_writes_same_location() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    file_t *fl = gtfs_open_file(gtfs, "test5.txt", 100);
    
    string str1 = "First write";
    write_t *wrt1 = gtfs_write_file(gtfs, fl, 0, str1.length(), str1.c_str());
    gtfs_sync_write_file(wrt1);
    
    string str2 = "Second write";
    write_t *wrt2 = gtfs_write_file(gtfs, fl, 0, str2.length(), str2.c_str());
    gtfs_sync_write_file(wrt2);
    
    char *data = gtfs_read_file(gtfs, fl, 0, str2.length());
    if (string(data) == "Second write") {
        cout << PASS;
    } else {
        cout << FAIL;
    }
    gtfs_close_file(gtfs, fl);
}

void test_concurrent_access() {
    int pid = fork();
    if (pid == 0) {
        gtfs_t *gtfs = gtfs_init(directory, verbose);
        file_t *fl = gtfs_open_file(gtfs, "test6.txt", 100);
        sleep(5); // hold file open
        gtfs_close_file(gtfs, fl);
        exit(0);
    }
    
    sleep(3);
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    file_t *fl = gtfs_open_file(gtfs, "test6.txt", 100);
    
    // the child should be holding the file lock so the parent shouldnt be able to open it
    if (fl == NULL) {
        cout << PASS;
    } else {
        cout << FAIL;
        gtfs_close_file(gtfs, fl);
    }
    waitpid(pid, NULL, 0);
}

void test_write_boundary() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    file_t *fl = gtfs_open_file(gtfs, "test7.txt", 100);
 
    string str = "This has too many characters";
    write_t *wrt = gtfs_write_file(gtfs, fl, 99, str.length(), str.c_str());
    
    if (wrt == NULL) {
        cout << PASS;  
    } else {
        cout << FAIL;
        gtfs_abort_write_file(wrt);
    }
    
    gtfs_close_file(gtfs, fl);
}

void test_empty_read() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    file_t *fl = gtfs_open_file(gtfs, "test8.txt", 100);
    
    char *data = gtfs_read_file(gtfs, fl, 50, 10);
    if (data != NULL && string(data) == "") {
        cout << PASS;
    } else {
        cout << FAIL;
    }
    
    gtfs_close_file(gtfs, fl);
}


void test_file_shrinking() {
    gtfs_t *gtfs = gtfs_init(directory, verbose);
    string str = "I'm a Ramblin' Wreck from Georgia Tech and a hell of an engineer\n"
             "A helluva', helluva helluva helluva helluva engineer\n"
             "Like all the jolly good fellows I drink my whiskey clear\n"
             "I'm a Ramblin' Wreck from Georgia Tech and a hell of an engineer\n"
             "Oh, if I had a daughter sir, I'd dress her in white and gold\n"
             "And put her on the campus, sir, to cheer on the brave and bold\n"
             "And if I had a son, sir, I'd tell you what he'd do\n"
             "He would yell \"To Hell With georgia\" like his daddy used to do\n"
             "Oh, I wish I had a barrel of rum and sugar three thousand pounds\n"
             "A college bell to put it in and a clapper to stir it around\n"
             "I'd drink to all the good fellows who come from far and near\n"
             "I'm a ramblin' gamblin' hell of an engineer! Hey!\n";
    
    file_t *fl1 = gtfs_open_file(gtfs, "test9.txt", (str.length()+10));
    write_t *wrt = gtfs_write_file(gtfs, fl1, 0, str.length(), str.c_str());
    gtfs_sync_write_file(wrt);    
    gtfs_close_file(gtfs, fl1);
    
    file_t *fl2 = gtfs_open_file(gtfs, "test9.txt", (str.length()-25));
    if (fl2 == NULL) {
        cout << PASS;
    } else {
        cout << FAIL;
        gtfs_close_file(gtfs, fl2);
    }
}

int main(int argc, char **argv) {
    if (argc < 2)
        printf("Usage: ./test verbose_flag\n");
    else
        verbose = strtol(argv[1], NULL, 10);

    // Get current directory path
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        directory = string(cwd);
        // directory = "/home/bmarty/cs4210/cs4210-Project3/gtfs/bin";
    } else {
        cout << "[cwd] Something went wrong.\n";
    }

    // Call existing tests
    // cout << "================== Test 0 ==================\n";
    // cout << "Testing initialization and basic setup.\n";
    // test_initialization();  

    cout << "================== Test 1 ==================\n";
    cout << "Testing that data written by one process is then successfully read by another process.\n";
    test_write_read();

    cout << "================== Test 2 ==================\n";
    cout << "Testing that aborting a write returns the file to its original contents.\n";
    test_abort_write();

    cout << "================== Test 3 ==================\n";
    cout << "Testing that the logs are truncated.\n";
    test_truncate_log();

    // TODO: Call any additional tests
    /**
            Test Ideas:
        =================
        
            Creative Tests:
                1. Crash Recovery
                    - Child process dies from runtime error while writing to a file
            2. 
    */
    
    cout << "================== Test 4 ==================\n";
    cout << "Testing that writes will be recovered from a crash.\n";
    test_crash_recovery();

    cout << "================== Test 5 ==================\n";
    cout << "Testing writing multiple things to the same location\n";
    test_multiple_writes_same_location();

    cout << "================== Test 6 ==================\n";
    cout << "Testing concurrent accesses to the same file\n";
    test_concurrent_access();

    cout << "================== Test 7 ==================\n";
    cout << "Testing that you can't write more than the file size\n";
    test_write_boundary();
    
    cout << "================== Test 8 ==================\n";
    cout << "Testing that an empty file is empty\n";
    test_empty_read();

    cout << "================== Test 9 ==================\n";
    cout << "Testing that reopening a larger file with a smaller length isn't allowed\n";
    test_file_shrinking();

    
}   
