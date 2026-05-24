CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -I. -I../lib/snappy-c -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDFLAGS = -pthread -lrt

# dirs
SRC_DIR = .
LIB_DIR = ../lib
OBJ_DIR = ../obj
BIN_DIR = ../bin
SNAPPY_DIR = $(LIB_DIR)/snappy-c

# create obj dirs
OBJ_SNAPPY_DIR = $(OBJ_DIR)/snappy
OBJ_SRC_DIR = $(OBJ_DIR)/src

# snappy (only snappy.c)
SNAPPY_SRCS = $(SNAPPY_DIR)/snappy.c
SNAPPY_OBJS = $(OBJ_SNAPPY_DIR)/snappy.o

# tinyfile shared code
TINYFILE_COMMON_SRC = $(SRC_DIR)/tinyfile_common.c
TINYFILE_COMMON_OBJ = $(OBJ_SRC_DIR)/tinyfile_common.o

# tinyfile service
TINYFILE_SRC = $(SRC_DIR)/tinyfile.c
TINYFILE_OBJ = $(OBJ_SRC_DIR)/tinyfile.o
TINYFILE_SERVER = $(BIN_DIR)/tinyfile

# client lib
TINYFILE_LIB_SRC = $(SRC_DIR)/tinyfile_lib.c
TINYFILE_LIB_OBJ = $(OBJ_SRC_DIR)/tinyfile_lib.o
TINYFILE_LIB = $(LIB_DIR)/libtinyfile.a

# sample app
SAMPLE_APP_SRC = $(SRC_DIR)/sample_app.c
SAMPLE_APP_OBJ = $(OBJ_SRC_DIR)/sample_app.o
SAMPLE_APP = $(BIN_DIR)/sample_app

# default cmds
all: dirs $(TINYFILE_SERVER) $(TINYFILE_LIB) $(SAMPLE_APP)

# make all needed dirs
dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(BIN_DIR)/output
	@mkdir -p $(BIN_DIR)/input
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(OBJ_SNAPPY_DIR)
	@mkdir -p $(OBJ_SRC_DIR)
	@mkdir -p $(LIB_DIR)

# objects
$(OBJ_SNAPPY_DIR)/snappy.o: $(SNAPPY_DIR)/snappy.c $(SNAPPY_DIR)/snappy.h
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "compiled snappy -> $@"

$(OBJ_SRC_DIR)/tinyfile_common.o: $(SRC_DIR)/tinyfile_common.c $(SRC_DIR)/tinyfile.h
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "compiled common -> $@"

$(OBJ_SRC_DIR)/tinyfile.o: $(SRC_DIR)/tinyfile.c $(SRC_DIR)/tinyfile.h
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "compiled tinyfile -> $@"

$(OBJ_SRC_DIR)/tinyfile_lib.o: $(SRC_DIR)/tinyfile_lib.c $(SRC_DIR)/tinyfile_lib.h $(SRC_DIR)/tinyfile.h
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "compiled lib -> $@"

$(OBJ_SRC_DIR)/sample_app.o: $(SRC_DIR)/sample_app.c $(SRC_DIR)/tinyfile_lib.h
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "compiled sample app -> $@"

# execs
$(TINYFILE_SERVER): $(TINYFILE_OBJ) $(TINYFILE_COMMON_OBJ) $(SNAPPY_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo "built server: $(TINYFILE_SERVER)"

$(TINYFILE_LIB): $(TINYFILE_LIB_OBJ) $(TINYFILE_COMMON_OBJ)
	ar rcs $@ $^
	@echo "built lib: $(TINYFILE_LIB)"

$(SAMPLE_APP): $(SAMPLE_APP_OBJ) $(TINYFILE_LIB) $(SNAPPY_OBJS)
	$(CC) $(SAMPLE_APP_OBJ) $(TINYFILE_LIB) $(SNAPPY_OBJS) -o $@ $(LDFLAGS)
	@echo "built sample: $(SAMPLE_APP)"

# single targets
tinyfile: dirs $(TINYFILE_SERVER)
libtinyfile: dirs $(TINYFILE_LIB)
sample_app: dirs $(SAMPLE_APP)

# clean
clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TINYFILE_SERVER)
	rm -f $(SAMPLE_APP)
	rm -f $(TINYFILE_LIB)
	rm -rf $(BIN_DIR)/output
	@echo "cleaned build files"
	rm -f /dev/shm/tinyfile_*
	@echo "cleaned shm"

# help msg
help:
	@echo ""
	@echo "TinyFile Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all        - build all"
	@echo "  tinyfile   - just the server"
	@echo "  libtinyfile- just the lib"
	@echo "  sample_app - just the sample app"
	@echo "  clean      - rm build files + output"
	@echo ""
	@echo "Usage:"
	@echo "  make              # build everything"
	@echo "  make clean        # clean it all"
	@echo "  ../bin/tinyfile --n_sms 5 --sms_size 4096 -f"
	@echo "  ../bin/sample_app --file ../bin/input/Tiny.txt --state SYNC"
	@echo ""

# debug:
# 	@echo "SNAPPY_SRCS: $(SNAPPY_SRCS)"
# 	@echo "SNAPPY_OBJS: $(SNAPPY_OBJS)"
# 	@echo "TINYFILE_COMMON_OBJ: $(TINYFILE_COMMON_OBJ)"
# 	@echo "TINYFILE_OBJ: $(TINYFILE_OBJ)"
# 	@echo "TINYFILE_LIB_OBJ: $(TINYFILE_LIB_OBJ)"
# 	@echo "SAMPLE_APP_OBJ: $(SAMPLE_APP_OBJ)"

.PHONY: all dirs clean help tinyfile libtinyfile sample_app