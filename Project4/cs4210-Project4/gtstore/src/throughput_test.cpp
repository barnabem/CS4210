#include "gtstore.hpp"
#include <chrono>
#include <random>

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <replication_factor>\n";
        return 1;
    }
    
    int rep_factor = atoi(argv[1]);
    const int TOTAL_OPS = 200000;
    const int READ_RATIO = 50; //half write and read
    


    cout << "=== Throughput Test ===" << endl;
    cout << "Replication Factor: " << rep_factor << endl;
    cout << "Total Operations: " << TOTAL_OPS << endl;
    cout << endl;
    
    GTStoreClient client;
    client.manager_ip_addr = "127.0.0.1"; // std address for all tests
    client.manager_port = 50051;
    client.init(1);
    std::random_device rd;// we use rand number ot decide read or write (mnore accurate)
    std::mt19937 gen(rd()); 
    std::uniform_int_distribution<> op_dist(1, 100);
    std::uniform_int_distribution<> key_dist(1, 10000);
    
    //pre-populate some keys for reads (warm cache, and measuring actual data read not miss lookups)
    cout << "Pre-populating 1000 keys..." << endl;
    for (int i = 1; i <= 1000; i++) {
        string key = "key" + to_string(i);
        val_t value;
        value.push_back("value" + to_string(i));
        client.put(key, value);
        
        if (i % 100 == 0) {
            cout << "  Populated " << i << "/1000 keys" << endl;
        }
    }
    
    cout << "Starting throughput test..." << endl;
    
    int read_ops = 0;
    int write_ops = 0;
    int failed_ops = 0;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < TOTAL_OPS; i++) {
        int op_type = op_dist(gen);
        string key = "key" + to_string(key_dist(gen) % 1000 + 1);
        
        if (op_type <= READ_RATIO) {
            val_t result = client.get(key);
            if (!result.empty()) {
                read_ops++;
            } else {
                failed_ops++;
            }
        } else {
            val_t value;
            value.push_back("updated_value_" + to_string(i));
            if (client.put(key, value)) {
                write_ops++;
            } else {
                failed_ops++;
            }
        }
        
        if ((i + 1) % 20000 == 0) {
            cout << "  Progress: " << (i + 1) << "/" << TOTAL_OPS << " ops" << endl;
        }
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;
    double throughput = TOTAL_OPS / seconds;
    
    cout << endl;
    cout << "=== Results ===" << endl;
    cout << "Total time: " << seconds << " seconds" << endl;
    cout << "Read operations: " << read_ops << endl;
    cout << "Write operations: " << write_ops << endl;
    cout << "Failed operations: " << failed_ops << endl;
    cout << "Throughput: " << throughput << " ops/sec" << endl;
    cout << endl;

    cout << "CSV Output (for graphing):" << endl;
    cout << rep_factor << "," << throughput << endl;
    
    client.finalize();
    return 0;
}