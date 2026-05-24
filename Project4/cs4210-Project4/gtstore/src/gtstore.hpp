#ifndef GTSTORE
#define GTSTORE

#include <string>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <map>
#include <unordered_map>
#include <set>

// gRPC and Protocol Buffer includes
#include <grpcpp/grpcpp.h>
#include "../bin/gtstore.grpc.pb.h"
#include "../bin/gtstore.pb.h"
using grpc::Channel;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using namespace gtstore;

#define MAX_KEY_BYTE_PER_REQUEST 20
#define MAX_VALUE_BYTE_PER_REQUEST 1000

#define TIMEOUT 5 // 5 seconds forall timeouts
#define HEARTBEAT_FREQ 5 // 5 seconds as well
#define HEARTBEAT_DEAD 15 // 15 seconds if heartbeat dies

#define DEBUG 0
#if DEBUG == 1
    #define DEBUG_PRINT(expr) do { std::cout << expr; } while (0)
#else
    #define DEBUG_PRINT(expr) do {} while (0)
#endif

using namespace std;

typedef vector<string> val_t;

struct NodeInfo { // we use a structure to keep track of more than just node_id (ip, port, and whether the node is alive)
	int node_id;
	string ip_addr;
	int port;
	bool is_alive;
	// add more if necessary

	NodeInfo() : node_id(-1), ip_addr(""), port(0), is_alive(false) {}
	NodeInfo(int id, string addr, int p) : node_id(id), ip_addr(addr), port(p), is_alive(true) {}
};

class GTStoreClient {
	private:
		int client_id;
		map<int, NodeInfo> node_map; // maps a node id to the node information
		int num_nodes;
		int replication_factor;

		

		bool send_put_to_node(int node_id, const string& key, const val_t& value); // get and put
        val_t send_get_to_node(int node_id, const string& key, bool& success);

		
		
		val_t value;
	public:
		string manager_ip_addr;
		int manager_port;

		GTStoreClient() : client_id(-1), num_nodes(0), replication_factor(1), manager_ip_addr("127.0.0.1"), manager_port(50051) {}

		int compute_node_for_key(const string& key); // hash functino for key, value pair
		vector<int> compute_replica_nodes_for_key(const string& key);
		bool refresh_node_registry();// in case client has stale data

		map<int, NodeInfo> get_node_map() {
			return node_map;
		}
		int get_replication_factor() {
			return replication_factor;
		}

		void init(int id);
		void finalize();
		val_t get(string key);
		bool put(string key, val_t value);
};

class GTStoreManager {
	private:
		map<int, NodeInfo> storage_nodes;
		int next_node_id; // counter to assign unique node id to new node

		std::mutex nodes_mutex;

		

		int compute_primary_node(const string& key); // figure out which node this key should go to
		vector<int> compute_replica_nodes(const string& key); // compute which nodes should store replicas
		
		void trigger_data_redistribution(int failed_node_id);
		

		
	public:
		string ip_addr;
		int port;
		int replication_factor; // some factor K for replication (need to understand this better)

		GTStoreManager() : next_node_id(0), ip_addr("127.0.0.1"), port(50051), replication_factor(1) {}

		void handle_node_failure(int node_id);
		map<int, NodeInfo> get_all_nodes();
		int register_storage_node(const string& address, int port); // new node handler
		int get_active_node_count();
		int get_replication_factor();
		void init();
};

class GTStoreStorage {
	private:
		unordered_map<string, val_t> data_store;// this actually is where the data lives in memroy
		std::mutex data_mutex;
		

		bool register_with_manager(); // init with manager


		void send_heartbeat(); // to check whether node died
	public:
		string manager_ip_addr;
        int manager_port;
		string ip_addr;
    	int port;
		int node_id;

		GTStoreStorage() : manager_ip_addr("127.0.0.1"), manager_port(50051), ip_addr("127.0.0.1"), port(50051), node_id(-1) {}

		bool handle_put(const string& key, const val_t& value); // get and put
		val_t handle_get(const string& key, bool& found);
		unordered_map<string, val_t> get_all_data();
		bool replicate_data(const map<string, val_t>& data);

		void init();
};

#endif
