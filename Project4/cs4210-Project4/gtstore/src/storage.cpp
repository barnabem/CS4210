#include "gtstore.hpp"
#include <thread>




class StorageServiceImpl final : public StorageService::Service {
	private:
		GTStoreStorage* storage;
	public:
		explicit StorageServiceImpl(GTStoreStorage* storage) : storage(storage) {}
		
		Status Put(ServerContext* context, const PutRequest* request, PutResponse* response) override {
			const string& key = request->key();

			val_t value;
			for (int i = 0; i < request->values_size(); i++) {
            	value.push_back(request->values(i));
        	}

			DEBUG_PRINT("Storage node " << storage->node_id << " received PUT for key: " << key << "\n");

			bool success = storage->handle_put(key, value);
			response->set_success(success);
			response->set_node_id(storage->node_id);

			
			if (success) {
				response->set_message("PUT successful");
				return Status::OK;
			} else {
				response->set_message("PUT failed");
				return Status(grpc::StatusCode::INTERNAL, "Failed to store key-value pair");
			}
		}

		Status Get(ServerContext* context, const GetRequest* request, GetResponse* response) override {
			const string& key = request->key();
			bool found = false;
			val_t value = storage->handle_get(key, found);

			response->set_key(key);
        	response->set_found(found);
        	response->set_node_id(storage->node_id);

			if (found) {
				// Add all values to the repeated field
				for (const auto& v : value) {
					response->add_values(v);
				}
			}

			return Status::OK;
		}

		Status GetAllData(ServerContext* context, const GetAllDataRequest* request, GetAllDataResponse* response) override {
			DEBUG_PRINT("Storage node " << storage->node_id << " received GetAllData request\n");
			auto all_data = storage->get_all_data();
			for (const auto& pair : all_data) {
				auto* data_entry = response->add_entries();
				data_entry->set_key(pair.first);
				for (const auto& v : pair.second) {
					data_entry->add_values(v);
				}
			}
			DEBUG_PRINT("Returning " << all_data.size() << " entries\n");
			return Status::OK;
		}

		Status ReplicateData(ServerContext* context, const ReplicateDataRequest* request, ReplicateDataResponse* response) override {
			DEBUG_PRINT("Storage node " << storage->node_id << " received ReplicateData request\n");
			map<string, val_t> data_to_replicate;
			for (int i = 0; i < request->entries_size(); i++) {
				const auto& entry = request->entries(i);
				val_t values;
				for (int j = 0; j < entry.values_size(); j++) {
					values.push_back(entry.values(j));
				}
				data_to_replicate[entry.key()] = values;
			}
			bool success = storage->replicate_data(data_to_replicate);
			response->set_success(success);
			response->set_node_id(storage->node_id);
			
			if (success) {
				DEBUG_PRINT("Successfully replicated " << data_to_replicate.size() << " entries\n");
			} else {
				cerr << "Failed to replicate data\n";
			}
			return Status::OK;
		}
};


void GTStoreStorage::init() {
	DEBUG_PRINT("Inside GTStoreStorage::init()\n");

	// Register with the manager
    if (register_with_manager()) {
        cout << "Storage node initialized successfully\n";
    } else {
        cerr << "Failed to register with manager\n";
        exit(1);
    }

	
	std::thread heartbeat_thread(&GTStoreStorage::send_heartbeat, this);
    heartbeat_thread.detach();
    
    string server_address = ip_addr + ":" + to_string(port);
    StorageServiceImpl service(this);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    unique_ptr<Server> server(builder.BuildAndStart());
    DEBUG_PRINT("Storage server listening on " << server_address << "\n");

    server->Wait();
}

bool GTStoreStorage::register_with_manager() {
	cout << "Registering with manager at " << manager_ip_addr << ":" << manager_port << "\n";
	try {
		string manager_address = manager_ip_addr + ":" + to_string(manager_port);
		auto channel = grpc::CreateChannel(manager_address, grpc::InsecureChannelCredentials());
		auto stub = ManagerService::NewStub(channel);

		RegisterNodeRequest request;
		request.set_ip_address(ip_addr);
		request.set_port(port);
		
		RegisterNodeResponse response;
		ClientContext context;
		auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT); // set timeout
		context.set_deadline(deadline);

		Status status = stub->RegisterNode(&context, request, &response);

		if (status.ok()) {
			if (response.success()) {
				
				node_id = response.node_id();
				DEBUG_PRINT("Storage Node Registration Successful" << std::endl);
				DEBUG_PRINT("node_id= "<< node_id << std::endl);
				return true;

			} else {
				cerr << "NODE REGISTRATION REJECTED: " << response.message() << std::endl;
				return false;
			}
		} else {
			cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << "\n";
			return false;
		}
	} catch (const exception& e) {
		cerr << "Exception during registration: " << e.what() << "\n";
        return false;
	}
}

void GTStoreStorage::send_heartbeat() {
	
	string manager_address = manager_ip_addr + ":" + to_string(manager_port);
	auto channel = grpc::CreateChannel(manager_address, grpc::InsecureChannelCredentials());
	auto stub = ManagerService::NewStub(channel);
	try {
		while(true) {
			std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_FREQ)); //send heartbeats on a set frequency
			
			HeartbeatRequest request;
			request.set_node_id(node_id);

			HeartbeatResponse response;
			ClientContext context;
			auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(TIMEOUT); // set timeout
			context.set_deadline(deadline);

			Status status = stub->Heartbeat(&context, request, &response);
			if(status.ok() && response.ack()) {
				continue; // can add more debug info for success if needed
			} else {
				cerr << "Heartbeat on node: " << node_id << " failed: ";
				if(!status.ok()) {
					cerr << status.error_message() << "\n";;
				} else {
					cerr << "Acknowledgement failed." << std::endl;
				}
			}
		}
	} catch (const exception& e) {
		cerr << "Exception during heartbeat: " << e.what() << "\n";
        return;
	}
}

bool GTStoreStorage::handle_put(const string& key, const val_t& value) {
	data_store[key] = value;
	DEBUG_PRINT("Stored key '" << key << "' with " << value.size() << " value(s)\n");
	return true;
}

val_t GTStoreStorage::handle_get(const string& key, bool& found) {
	auto it = data_store.find(key);
    if (it != data_store.end()) {
        found = true;
		DEBUG_PRINT("Found key '" << key << "' with " << it->second.size() << " value(s)\n");
        return it->second;
    }
    found = false;
	DEBUG_PRINT("Key '" << key << "' not found\n");
    return val_t();
}

unordered_map<string, val_t> GTStoreStorage::get_all_data() {
	std::lock_guard<std::mutex> lock(data_mutex);
	return data_store; // Returns a copy
}

bool GTStoreStorage::replicate_data(const map<string, val_t>& data) {
	std::lock_guard<std::mutex> lock(data_mutex);
	try {
		for (const auto& pair : data) {
			data_store[pair.first] = pair.second;
		}
		DEBUG_PRINT("Replicated " << data.size() << " entries\n");
		return true;
	} catch(const exception& e) {
		cerr << "Exception during data replication: " << e.what() << "\n";
		return false;
	}
	
	
}

int main(int argc, char **argv) {

	GTStoreStorage storage;
	
	if (argc < 5) {
		cerr << "Usage: " << argv[0] << " <storage_ip> <storage_port> <manager_ip> <manager_port>\n";
		cerr << "Example: " << argv[0] << " 127.0.0.1 50052 127.0.0.1 50051\n";
        return 1;
	}

	storage.ip_addr = string(argv[1]);
	storage.port = atoi(argv[2]);
	storage.manager_ip_addr = string(argv[3]);
	storage.manager_port = atoi(argv[4]);

	DEBUG_PRINT("Starting storage node on port " << storage.port << "\n");
	DEBUG_PRINT("Manager Address: " << storage.manager_ip_addr << ":" << storage.manager_port << "\n");
	storage.init();
	return 0;
	
	
}
