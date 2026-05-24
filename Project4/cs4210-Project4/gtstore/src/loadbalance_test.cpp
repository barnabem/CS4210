#include "gtstore.hpp"
#include <map>

int main(int argc, char** argv) {
    const int TOTAL_INSERTS = 100000;
    
    cout << "=== Load Balance Test ===" << endl;
    cout << "Total Inserts: " << TOTAL_INSERTS << endl;
    cout << "Nodes: 7" << endl;
    cout << "Replication Factor: 1" << endl;
    cout << endl;
    
    GTStoreClient client;
    client.manager_ip_addr = "127.0.0.1";
    client.manager_port = 50051;
    client.init(1);
    map<int, int> node_key_count;
    
    cout << "Inserting " << TOTAL_INSERTS << " keys..." << endl;
    for (int i = 0; i < TOTAL_INSERTS; i++) {
        string key = "loadtest_key_" + to_string(i);
        val_t value;
        value.push_back("value_" + to_string(i));
        int node_id = client.compute_node_for_key(key);
        
        if (client.put(key, value)) {
            node_key_count[node_id]++;
        }
        
        if ((i + 1) % 10000 == 0) {
            cout << "  Progress: " << (i + 1) << "/" << TOTAL_INSERTS << " keys" << endl;
        }
    }
    
    cout << endl;
    cout << "=== Load Distribution Results ===" << endl;
    cout << "Node ID, Key Count, Percentage" << endl;
    
    int total_keys = 0;
    for (const auto& pair : node_key_count) {
        total_keys += pair.second;
    }
    for (const auto& pair : node_key_count) {
        double percentage = (pair.second * 100.0) / total_keys;
        cout << pair.first << "," << pair.second << "," << percentage << "%" << endl;
    }
    
    cout << endl;
    cout << "Total: " << total_keys << " keys distributed across " << node_key_count.size() << " nodes" << endl;
    
    cout << endl;
    cout << "CSV Output (Node ID, Key Count):" << endl;
    for (const auto& pair : node_key_count) {
        cout << pair.first << "," << pair.second << endl;
    }
    
    client.finalize();
    return 0;
}