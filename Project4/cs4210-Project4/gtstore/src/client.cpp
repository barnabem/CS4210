#include "gtstore.hpp"



void GTStoreClient::init(int id) {

	DEBUG_PRINT("Inside GTStoreClient::init() for client " << id << "\n");
	client_id = id;

    // manager_ip_addr = "127.0.0.1";
    // manager_port = 50051;

	if (!refresh_node_registry()) {
        cerr << "Failed to get initial node registry from manager\n";
    }
}

int GTStoreClient::compute_node_for_key(const string&key) {
	if (num_nodes <= 0) {
        cerr << "Error: No storage nodes available\n";
        return -1;
    }
	// int node_id;
	size_t hashed_num;
	hashed_num = std::hash<std::string>{}(key);
	vector<int> active_node_ids;
	for (const auto& pair : node_map) {
		if (pair.second.is_alive) {
			active_node_ids.push_back(pair.first);
		}
	}
	if (active_node_ids.empty()) {
		cerr << "Error: No active nodes\n";
		return -1;
	}
	sort(active_node_ids.begin(), active_node_ids.end());
	int index = hashed_num % active_node_ids.size();
	int node_id = active_node_ids[index];


	// node_id = static_cast<int>(hashed_num % num_nodes);

	// DEBUG
	DEBUG_PRINT("Key '" << key << "' -> primary node " << node_id << " (hash=" << hashed_num << ", active_nodes=" << active_node_ids.size() << ")\n");

	return node_id;
}

vector<int> GTStoreClient::compute_replica_nodes_for_key(const string& key) {
	vector<int> nodes;

	if (num_nodes <= 0) {
		cerr << "Error: No storage nodes available\n";
		return nodes;
	}

	int primary_node = compute_node_for_key(key);
	if (primary_node < 0) {
		return nodes;
	}

	vector<int> all_active_node_ids;
	for (const auto& pair : node_map) {
		if (pair.second.is_alive) {
			all_active_node_ids.push_back(pair.first);
		}
	}

	if (all_active_node_ids.empty()) {
		cerr << "Error: No active storage nodes available\n";
		return nodes;
	}
	sort(all_active_node_ids.begin(), all_active_node_ids.end());

	auto primary_it = find(all_active_node_ids.begin(), all_active_node_ids.end(), primary_node);
	if (primary_it == all_active_node_ids.end()) {
		cerr << "Error: Primary node " << primary_node << " not found among active nodes\n";
		return nodes;
	}
	int primary_index = distance(all_active_node_ids.begin(), primary_it);
	nodes.push_back(primary_node);
	int num_replicas = min(replication_factor, (int)all_active_node_ids.size());
	for (int i = 1; i < num_replicas; i++) {
		int next_index = (primary_index + i) % all_active_node_ids.size();
		nodes.push_back(all_active_node_ids[next_index]);
	}

#if DEBUG == 1
	DEBUG_PRINT("Replicas for key '" << key << "': ");
	for (int nid : nodes) {
		DEBUG_PRINT(nid << " ");
	}
	DEBUG_PRINT("(replication_factor=" << replication_factor << ")\n");
#endif

	return nodes;

    // Placeholder implementation for replicas: return only the primary node for now.
    // Keep the existing commented plan above intact — replication logic will be implemented later.
    // vector<int> nodes;
    // int primary_node = compute_node_for_key(key);
    // if (primary_node >= 0) {
    //     nodes.push_back(primary_node);
    // }
    // return nodes;
}

bool GTStoreClient::refresh_node_registry() {
	DEBUG_PRINT("Refreshing node registry from manager...\n");

	try {
		string manager_address = manager_ip_addr + ":" + to_string(manager_port);
        auto channel = grpc::CreateChannel(manager_address, grpc::InsecureChannelCredentials());
        auto stub = ManagerService::NewStub(channel);

        GetNodesRequest request;
        GetNodesResponse response;
        ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT);
        context.set_deadline(deadline);
        
        Status status = stub->GetNodes(&context, request, &response);
        
        if (status.ok()) {
            node_map.clear();
            num_nodes = 0;
			replication_factor = response.replication_factor();  // ← This line was added
            
            for (int i = 0; i < response.nodes_size(); i++) {
                const auto& node = response.nodes(i);
                if (node.is_alive()) {
                    NodeInfo info(node.node_id(), node.ip_address(), node.port());
                    node_map[node.node_id()] = info;
                    num_nodes++;
                }
            }
            
            DEBUG_PRINT("Successfully refreshed registry. Active nodes: " << num_nodes << "\n");
            return true;
        } else {
            cerr << "Failed to get nodes from manager: " << status.error_message() << "\n";
            return false;
        }
	} catch (const exception& e) {
        cerr << "Exception during refresh_node_registry: " << e.what() << "\n";
        return false;
    }
}

bool GTStoreClient::send_put_to_node(int node_id, const string& key, const val_t& value) {
	auto it = node_map.find(node_id);
    if (it == node_map.end()) {
        cerr << "Node " << node_id << " not found in registry\n";
        return false;
    }

	const NodeInfo& node = it->second;
    if (!node.is_alive) {
        cerr << "Node " << node_id << " is marked as dead\n";
        return false;
    }

	try {
		string node_address = node.ip_addr + ":" + to_string(node.port);
		auto channel = grpc::CreateChannel(node_address, grpc::InsecureChannelCredentials());
		auto stub = StorageService::NewStub(channel);

		PutRequest request;
		request.set_key(key);
		for (const auto& val : value) {
			request.add_values(val);
		}

		PutResponse response;
		ClientContext context;
		auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT); // set timeout
		context.set_deadline(deadline);

		Status status = stub->Put(&context, request, &response);
		if (status.ok()) {
			if (response.success()) {
				return true;
			} else {
				cerr << "Put operation failed on node " << node_id << "\n";
				return false;
			}
		} else {
			cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << "\n";
			if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
                DEBUG_PRINT("Node appears to be down, refreshing registry...\n");
                refresh_node_registry();
            }
			return false;
		}
	} catch (const exception& e) {
		cerr << "Exception during put to node " << node_id << ": " << e.what() << "\n";
		return false;
	}
}

val_t GTStoreClient::send_get_to_node(int node_id, const string& key, bool& success) {
	success = false;

	auto it = node_map.find(node_id);
	if (it == node_map.end()) {
		cerr << "Node " << node_id << " not found in registry\n";
		return val_t();
	}

	const NodeInfo& node = it->second;
	if (!node.is_alive) {
		cerr << "Node " << node_id << " is marked as dead\n";
		return val_t();
	}

	try {
		string node_address = node.ip_addr + ":" + to_string(node.port);
		auto channel = grpc::CreateChannel(node_address, grpc::InsecureChannelCredentials());
		auto stub = StorageService::NewStub(channel);

		GetRequest request;
		request.set_key(key);

		GetResponse resp;
		ClientContext context;
		auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT); // set timeout
		context.set_deadline(deadline);

		Status status = stub->Get(&context, request, &resp);
		if (status.ok()) {
			if (resp.found()) {
				val_t vals;
				for (int i = 0; i < resp.values_size(); i++) {
					vals.push_back(resp.values(i));
				}
				success = true;
				return vals;
			} else {
				cerr << "Key not found on node " << resp.node_id() << "\n";
				return val_t();
			}
		} else {
			cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << "\n";
			if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
                DEBUG_PRINT("Node appears to be down, refreshing registry...\n");
                refresh_node_registry();
            }
			return val_t();
		}
	} catch (const exception& e) {
		cerr << "Exception during get from node " << node_id << ": " << e.what() << "\n";
		return val_t();
	}
}

val_t GTStoreClient::get(string key) {

	DEBUG_PRINT("Inside GTStoreClient::get() for client: " << client_id << " key: " << key << "\n");

	// int node_id = compute_node_for_key(key);
	vector<int> replica_nodes = compute_replica_nodes_for_key(key);
    // if (node_id < 0) {
	if (replica_nodes.empty()) {
        cerr << "Failed to compute node for key\n";
        return val_t();
    }

	for (int node_id : replica_nodes) {
		bool success = false;
		val_t value = send_get_to_node(node_id, key, success);
		
		if (success) {
			return value;
		}
		
		DEBUG_PRINT("Failed to get from node " << node_id << ", trying next replica...\n");
	}

	cerr << "GET operation failed for key '" << key << "' on all replicas\n";
	return val_t();
}

// 	bool success = false;
// 	val_t value = send_get_to_node(node_id, key, success);

// 	if (!success) {
// 		cerr << "GET operation failed for key '" << key << "' on node " << node_id << "\n";
// 		return val_t();
// 	}

// 	// Get the value!
// 	return value;
// }

bool GTStoreClient::put(string key, val_t value) {

	string print_value = "";
	for (uint i = 0; i < value.size(); i++) {
		print_value += value[i] + " ";
		if (i < value.size() - 1) print_value += ", ";
	}
	DEBUG_PRINT("Inside GTStoreClient::put() for client: " << client_id << " key: " << key << " value: " << print_value << "\n");

	// int node_id = compute_node_for_key(key);
	vector<int> replica_nodes = compute_replica_nodes_for_key(key);
	// if (node_id < 0) {
	if (replica_nodes.empty()) {
		cerr << "Failed to compute node for key\n";
		return false;
	}
	DEBUG_PRINT("Writing to " << replica_nodes.size() << " replica node(s)...\n");
	int successful_writes = 0;
	for (int node_id : replica_nodes) {
		if (send_put_to_node(node_id, key, value)) {
			successful_writes++;
		}
	}
	int required_writes = (replication_factor + 1) / 2; // TODO: Majority writes good enough or no?
	if (successful_writes >= required_writes) {
		DEBUG_PRINT("PUT successful: wrote to " << successful_writes << "/" << replica_nodes.size() << " nodes\n");
		return true;
	} else {
		cerr << "PUT failed: only " << successful_writes << "/" << replica_nodes.size() << " writes succeeded\n";
		return false;
	}
}
	
// 	bool success = send_put_to_node(node_id, key, value);
// 	if (!success) {
// 		cerr << "PUT operation failed for key '" << key << "' on node " << node_id << "\n";
// 	}
// 	return success;
// }

void GTStoreClient::finalize() {
	DEBUG_PRINT("Inside GTStoreClient::finalize() for client " << client_id << "\n");
	node_map.clear();
	num_nodes = 0;
}
