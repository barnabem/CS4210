#include "gtstore.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>


class ManagerServiceImpl final : public ManagerService::Service {
    private:
        GTStoreManager* manager;
        std::map<int, std::chrono::steady_clock::time_point> last_heartbeat;
        std::mutex heartbeat_mutex;
    public:
        explicit ManagerServiceImpl(GTStoreManager* mgr) : manager(mgr) {}

        Status RegisterNode(ServerContext* context, const RegisterNodeRequest* request, RegisterNodeResponse* response) override {
            DEBUG_PRINT("Received RegisterNode request from " << request->ip_address() << ":" << request->port() << "\n");

            try {
                int node_id = manager->register_storage_node(request->ip_address(), request->port());

                response->set_node_id(node_id);
                response->set_success(true);
                response->set_message("Registration success");



                {
                    std::lock_guard<std::mutex> lock(heartbeat_mutex);
                    last_heartbeat[node_id] = std::chrono::steady_clock::now();
                }
                
                DEBUG_PRINT("Succesfully registered node " << node_id << "\n");
                return Status::OK;
            } catch (const exception& e) {
                cerr << "error during node registration " << e.what() << "\n";
                response->set_success(false);
                response->set_message("Registration failed");
                return Status(grpc::StatusCode::INTERNAL, "Registration failed");
            }
        }

        Status Heartbeat(ServerContext* context, const HeartbeatRequest* request, HeartbeatResponse* response) {
            int node_id = request->node_id();

            {
                std::lock_guard<std::mutex> lock(heartbeat_mutex);
                last_heartbeat[node_id] = std::chrono::steady_clock::now();
            }

            response->set_ack(true);
            return Status::OK;
        }

        Status GetNodes(ServerContext* context, const GetNodesRequest* request, GetNodesResponse* response) {
            DEBUG_PRINT("Recevied get nodes request\n");

            try {
                auto nodes = manager->get_all_nodes();
                int rep_factor = manager->get_replication_factor();

                for (const auto& pair : nodes) {
                    const NodeInfo& node = pair.second;
                    NodeInfoMsg* node_msg = response->add_nodes();
                    node_msg->set_node_id(node.node_id);
                    node_msg->set_ip_address(node.ip_addr);
                    node_msg->set_port(node.port);
                    node_msg->set_is_alive(node.is_alive);
                }
                response->set_replication_factor(rep_factor);
                DEBUG_PRINT("Returning " << response->nodes_size() << " nodes to client\n");
                return Status::OK;

            } catch (const exception& e) {
                cerr << "Error during get nodes acquiring " << e.what() << "\n";
                return Status(grpc::StatusCode::INTERNAL, "Failed to get nodes in manager");
            }
        }

        void check_heartbeats_periodically() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_FREQ));

                auto now = std::chrono::steady_clock::now();
                vector<int> failed_nodes;

                {
                    std::lock_guard<std::mutex> lock(heartbeat_mutex);

                    for (const auto& pair : last_heartbeat) {
                        int node_id = pair.first;
                        auto last_time = pair.second;

                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count();
                        if (elapsed > HEARTBEAT_DEAD) { 
                            failed_nodes.push_back(node_id); 
                        }
                    }
                }

                for (int node_id : failed_nodes) {
                    cerr << "Node " << node_id << " missed heartbeat, marking as failed\n";
                    manager->handle_node_failure(node_id);

                    std::lock_guard<std::mutex> lock(heartbeat_mutex);
                    last_heartbeat.erase(node_id);
                }
                DEBUG_PRINT("[Heartbeat Check] Active storage nodes: " << manager->get_active_node_count() << "\n");
            }
        }

};


void GTStoreManager::init() {
	
	DEBUG_PRINT("Inside GTStoreManager::init()\n");

	next_node_id = 0;
    // replication_factor = 3; // Default, can be set via command line
    // ip_addr = "127.0.0.1";
    // port = 50051; // Default manager port

	cout << "Manager initialized with:\n";
    cout << "  Address: " << ip_addr << ":" << port << "\n";
    cout << "  Replication factor: " << replication_factor << "\n";

    string server_address = ip_addr + ":" + to_string(port);
    ManagerServiceImpl service(this);
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    unique_ptr<Server> server(builder.BuildAndStart());

    cout << "Manager server listening on " << server_address << "\n";

    std::thread heartbeat_thread([&service]() {
        service.check_heartbeats_periodically();
    });
    heartbeat_thread.detach();
    
    server->Wait();

}

int GTStoreManager::compute_primary_node(const string& key) {
	size_t hash_value = std::hash<std::string>{}(key);
    
    vector<int> active_node_ids;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex);
        for (const auto& pair : storage_nodes) {
            if (pair.second.is_alive) {
                active_node_ids.push_back(pair.first);
            }
        }
    }
	if (active_node_ids.empty()) {
        cerr << "No active nodes found\n";
        return -1;
    }
	sort(active_node_ids.begin(), active_node_ids.end());

	int index = hash_value % active_node_ids.size();
    int primary_node = active_node_ids[index];
	DEBUG_PRINT("Key '" << key << "' -> primary node " << primary_node << " (hash=" << hash_value << ", active_nodes=" << active_node_ids.size() << ")\n");
	return primary_node;
}

vector<int> GTStoreManager::compute_replica_nodes(const string& key) {
	vector<int> replica_nodes;
	int primary = compute_primary_node(key);
    if (primary < 0) {
        return replica_nodes;
    }
    vector<int> active_node_ids;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex);
        for (const auto& pair : storage_nodes) {
            if (pair.second.is_alive) {
                active_node_ids.push_back(pair.first);
            }
        }
    }
	if (active_node_ids.empty()) {
        cerr << "No active nodes available for replicas\n";
        return replica_nodes;
    }

	sort(active_node_ids.begin(), active_node_ids.end());
	auto primary_it = find(active_node_ids.begin(), active_node_ids.end(), primary);
    if (primary_it == active_node_ids.end()) {
        cerr << "Primary node not found in active nodes\n";
        return replica_nodes;
    }
    int primary_index = distance(active_node_ids.begin(), primary_it);
    replica_nodes.push_back(primary);



	
	int num_replicas = min(replication_factor, (int)active_node_ids.size());
    for (int i = 1; i < num_replicas; i++) {
        int next_index = (primary_index + i) % active_node_ids.size();
        replica_nodes.push_back(active_node_ids[next_index]);
    }

#if DEBUG == 1
	DEBUG_PRINT("Replicas for key '" << key << "': ");
    for (int node : replica_nodes) {
        DEBUG_PRINT(node << " ");
    }
    DEBUG_PRINT("\n");
#endif

    return replica_nodes;

}


int GTStoreManager::register_storage_node(const string& address, int node_port) {
	std::lock_guard<std::mutex> lock(nodes_mutex);
	int node_id = next_node_id++;
	NodeInfo node_info(node_id, address, node_port);
	storage_nodes[node_id] = node_info;

	DEBUG_PRINT("Registered new storage node: ID=" << node_id << ", Address=" << address << ":" << node_port << "\n");
	return node_id;
}

void GTStoreManager::trigger_data_redistribution(int failed_node_id) {
    cout << "Triggering data redistribution due to failure of node " << failed_node_id << "\n";
    
    vector<int> active_node_ids;
	map<int, NodeInfo> active_nodes;
	{
		std::lock_guard<std::mutex> lock(nodes_mutex);
		for (const auto& pair : storage_nodes) {
			if (pair.second.is_alive && pair.first != failed_node_id) {
				active_node_ids.push_back(pair.first);
				active_nodes[pair.first] = pair.second;
			}
		}
	}

    if (active_node_ids.empty()) {
        cerr << "No active nodes available for data redistribution\n";
        return;
    }
    for(const auto& pair : active_nodes) {
        int source_node_id = pair.first;
		const NodeInfo& source_node = pair.second;

        try {
            string source_address = source_node.ip_addr + ":" + to_string(source_node.port);
			auto channel = grpc::CreateChannel(source_address, grpc::InsecureChannelCredentials());
			auto stub = StorageService::NewStub(channel);
			
			// Get all data from this node
			GetAllDataRequest req;
			GetAllDataResponse resp;
			ClientContext context;
			auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT);
			context.set_deadline(deadline);
            Status status = stub->GetAllData(&context, req, &resp);

            if (!status.ok()) {
                cerr << "Failed to get data from node " << source_node_id << ": " << status.error_message() << "\n";
                continue;
            }
            DEBUG_PRINT("Node " << source_node_id << " has " << resp.entries_size() << " keys\n");

            for (int i = 0; i < resp.entries_size(); i++) {
                const auto& entry = resp.entries(i);
                const string& key = entry.key();

                vector<int> new_replicas = compute_replica_nodes(key);

                for (int target_node_id : new_replicas) { // get replicas and redistribute to all other nodes
                    if (target_node_id == source_node_id || target_node_id == failed_node_id){
                        continue;
                    }
                    auto target_it = active_nodes.find(target_node_id);
                    if (target_it == active_nodes.end()) 
                        continue;

                    const NodeInfo& target_node = target_it->second;
                    string target_address = target_node.ip_addr + ":" + to_string(target_node.port);
                    auto target_channel = grpc::CreateChannel(target_address, grpc::InsecureChannelCredentials());
                    auto target_stub = StorageService::NewStub(target_channel);

                    PutRequest put_req;
                    put_req.set_key(key);
                    for (int j = 0; j < entry.values_size(); j++) {
                        put_req.add_values(entry.values(j));
                    }
                    PutResponse put_resp;
                    ClientContext put_context;
                    auto put_deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT);
                    put_context.set_deadline(put_deadline);

                    Status put_status = target_stub->Put(&put_context, put_req, &put_resp);
                    if (put_status.ok() || put_resp.success()) { 
                        DEBUG_PRINT("  Replicated key '" << key << "' from node " << source_node_id << " to node " << target_node_id << "\n");
                    }
                
                }
            }
        } catch (const exception& e) {
			cerr << "Exception during redistribution from node " << source_node_id << ": " << e.what() << "\n";
		}

    }
    DEBUG_PRINT("=== Data Redistribution Complete ===\n\n");
}

void GTStoreManager::handle_node_failure(int node_id) {
	std::lock_guard<std::mutex> lock(nodes_mutex);
    
    auto it = storage_nodes.find(node_id);
    if (it == storage_nodes.end()) {
        cerr << "Cannot handle failure: Node " << node_id << " not found\n";
        return;
    }
	if (!it->second.is_alive) { return; }
	
	it->second.is_alive = false;
	DEBUG_PRINT("Node " << node_id << " marked as FAILED\n");
    
    int active_count = 0;
    for (const auto& pair : storage_nodes) {
        if (pair.second.is_alive) {
            active_count++;
        }
    }
	DEBUG_PRINT("Active nodes remaining: " << active_count << "\n");

    if (replication_factor > 1 && active_count > 0) {
		// Release lock before redistribution to avoid deadlock
		nodes_mutex.unlock();
		trigger_data_redistribution(node_id);
		nodes_mutex.lock();
	}
	// Note: Data redistribution happens automatically since compute_primary_node()
    // only considers alive nodes. Clients will refresh their node registry
    // and requests will be routed to surviving nodes.

}

int GTStoreManager::get_active_node_count() {
    std::lock_guard<std::mutex> lock(nodes_mutex);
    
    int count = 0;
    for (const auto& pair : storage_nodes) {
        if (pair.second.is_alive) {
            count++;
        }
    }
    return count;
}

int GTStoreManager::get_replication_factor() {
	return replication_factor;
}

map<int, NodeInfo> GTStoreManager::get_all_nodes() {
    std::lock_guard<std::mutex> lock(nodes_mutex);
    return storage_nodes;  // Returns a COPY
}

void check_heartbeats(GTStoreManager* manager) {
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_FREQ)); // check every 5 seconds
		DEBUG_PRINT("[Heartbeat Check] Active storage nodes: " << manager->get_active_node_count() << "\n");
	}
}



int main(int argc, char **argv) {

	GTStoreManager manager;
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <manager_ip> <manager_port> <replication_factor>\n";
		cerr << "Example: " << argv[0] << " 127.0.0.1 50051 3\n";
        return 1;
    }

    manager.ip_addr = string(argv[1]);
    manager.port = atoi(argv[2]);

    if (argc >= 4) {
        manager.replication_factor = atoi(argv[3]);
    } else {
		manager.replication_factor = 1; // Default replication factor
	}
    
	manager.init();
    return 0;
	
}
