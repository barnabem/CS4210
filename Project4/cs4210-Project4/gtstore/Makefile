CFLAGS  =
LFLAGS  =
CC      = g++ -std=c++11 -Wall
RM      = /bin/rm -rf
#LDFLAGS = -lgrpc++ -lgrpc -lgpr -lprotobuf -lpthread -labsl_synchronization -labsl_base
LDFLAGS = $(shell pkg-config --libs grpc++ grpc protobuf) -lpthread
BIN_DIR	= ./bin

PROTO_PATH = ./proto
PROTO_FILE = $(PROTO_PATH)/gtstore.proto
PROTO_SRC = $(BIN_DIR)/gtstore.pb.cc $(BIN_DIR)/gtstore.grpc.pb.cc
PROTO_OBJ = $(PROTO_SRC:.cc=.o)
GRPC_CPP_PLUGIN = $(shell which grpc_cpp_plugin)

TESTS 		= bin/manager bin/storage bin/client bin/throughput_test bin/loadbalance_test
CLIENT_SRC	= ./src/test_app.cpp ./src/client.cpp

all: $(TESTS)

proto: bin/gtstore.pb.cc 

bin/gtstore.pb.cc: $(PROTO_FILE)
	mkdir -p bin
	protoc --proto_path=./proto --cpp_out=./bin --grpc_out=./bin \
	       --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN) gtstore.proto

bin/%.o: bin/%.cc
	$(CC) -c $< -o $@

bin/manager: ./src/manager.cpp $(PROTO_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

bin/storage: ./src/storage.cpp $(PROTO_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

bin/client: ./src/test_app.cpp ./src/client.cpp $(PROTO_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

bin/throughput_test: ./src/throughput_test.cpp ./src/client.cpp $(PROTO_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

bin/loadbalance_test: ./src/loadbalance_test.cpp ./src/client.cpp $(PROTO_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	$(RM) bin/*
.PHONY: all clean