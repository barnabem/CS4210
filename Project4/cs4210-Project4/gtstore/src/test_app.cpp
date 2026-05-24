#include "gtstore.hpp"
#include <getopt.h>
#include <sstream>

void print_usage(const char* program_name) {
    cerr << "Usage: " << program_name << " [OPTIONS]\n";
    cerr << "Options:\n";
    cerr << "  --put <key>          Set a key (requires --val)\n";
    cerr << "  --get <key>          Get a key\n";
    cerr << "  --val <value>        Value to set (for --put)\n";
    cerr << "  --manager <addr>     Manager address (default: 127.0.0.1:50051)\n";
    cerr << "  --interactive        Enter interactive mode\n";
    cerr << "  --help               Show this help message\n";
}

void print_interactive_help() {
    cout << "\nAvailable commands:\n";
    cout << "  put <key> <value>   - Store a key-value pair\n";
    cout << "  get <key>           - Retrieve a value\n";
    cout << "  help                - Show this help message\n";
    cout << "  quit / exit         - Exit interactive mode\n";
    cout << endl;
}

void interactive_mode(GTStoreClient& client) {
    cout << "\n=== GTStore Interactive Client ===" << endl;
    cout << "Type 'help' for available commands" << endl;
    print_interactive_help();
    
    string line;
    while (true) {
        cout << "gtstore> ";
        if (!getline(cin, line)) {
            break;
        }
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        



        if (line.empty()) {
            continue;
        }
        istringstream iss(line);
        string command;
        iss >> command;
        

        if (command == "quit" || command == "exit") {
            cout << "Exiting interactive mode..." << endl;
            break;
        } else if (command == "help") {
            print_interactive_help();
        } else if (command == "put") {
            string key, value;
            iss >> key >> value;
            
            if (key.empty() || value.empty()) {
                cerr << "Usage: put <key> <value>" << endl;
                continue;
            }
            
            val_t val_vector;
            val_vector.push_back(value);
            
            cout << "Putting key='" << key << "', value='" << value << "'..." << endl;
            bool success = client.put(key, val_vector);
            
            if (success) {
                int node_id = client.compute_node_for_key(key);
                cout << "OK, server" << node_id << endl;
            } else {
                cerr << "PUT failed" << endl;
            }
        } else if (command == "get") {
            string key;
            iss >> key;
            if (key.empty()) {
                cerr << "Usage: get <key>" << endl;
                continue;
            }
            
            cout << "Getting key='" << key << "'..." << endl;
            val_t result = client.get(key);
            if (!result.empty()) {
                int node_id = client.compute_node_for_key(key);
                cout << key << ", " << result[0];
                for (size_t i = 1; i < result.size(); i++) {
                    cout << " " << result[i];
                }
                cout << ", server" << node_id << endl;
            } else {
                cerr << "Key not found or operation failed" << endl;
            }
        } else {
            cerr << "Unknown command: '" << command << "'. Type 'help' for available commands." << endl;
        }
    }
}

int main(int argc, char** argv) {
    string operation = "";
    string key = "";
    string value = "";
    string manager_addr = "127.0.0.1:50051";
    bool interactive = false;
    static struct option long_options[] = {
        {"put", required_argument, 0, 'p'},
        {"get", required_argument, 0, 'g'},
        {"val", required_argument, 0, 'v'},
        {"manager", required_argument, 0, 'm'},
        {"interactive", no_argument, 0, 'i'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "p:g:v:m:ih", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                operation = "put";
                key = optarg;
                break;
            case 'g':
                operation = "get";
                key = optarg;
                break;
            case 'v':
                value = optarg;
                break;
            case 'm':
                manager_addr = optarg;
                break;
            case 'i':
                interactive = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    size_t colon_pos = manager_addr.find(':');
    string manager_ip = manager_addr.substr(0, colon_pos);
    int manager_port = 50051;
    if (colon_pos != string::npos) {
        manager_port = stoi(manager_addr.substr(colon_pos + 1));
    }
    GTStoreClient client;
    client.manager_ip_addr = manager_ip;
    client.manager_port = manager_port;
    
    client.init(1);

    if (interactive) {
        interactive_mode(client);
        client.finalize();
        return 0;
    }
    
    if (operation.empty() || key.empty()) {
        cerr << "Error: Must specify either --put or --get with a key, or use --interactive\n";
        print_usage(argv[0]);
        client.finalize();
        return 1;
    }
    
    if (operation == "put" && value.empty()) {
        cerr << "Error: --put requires --val\n";
        print_usage(argv[0]);
        client.finalize();
        return 1;
    }
    
    if (operation == "put") {
        val_t val_vector;
        val_vector.push_back(value);
        
        bool success = client.put(key, val_vector);
        if (success) {
            int node_id = client.compute_node_for_key(key);
            cout << "OK, server" << node_id << "\n";
        } else {
            cerr << "PUT failed\n";
            client.finalize();
            return 1;
        }
    } else if (operation == "get") {
        val_t result = client.get(key);
        if (!result.empty()) {
            int node_id = client.compute_node_for_key(key);
            cout << key << ", " << result[0];
            for (size_t i = 1; i < result.size(); i++) {
                cout << " " << result[i];
            }
            cout << ", server" << node_id << "\n";
        } else {
            cerr << "Key not found\n";
            client.finalize();
            return 1;
        }
    }
    
    client.finalize();
    return 0;
}