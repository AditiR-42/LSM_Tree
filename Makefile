# Makefile

CC = g++
CFLAGS = -Wall -Wextra -pedantic -std=c++17 -O3

# Linker flags for executables that require threading
PTHREAD_LDFLAGS = -pthread

# Executable names
SERVER_TARGET = server
CLIENT_TARGET = client
TEST_TARGET = test

# Object files
BLOOM_FILTER_OBJECT = bloom_filter.o
LSM_TREE_OBJECT = lsm_tree.o
SERVER_OBJECT = main.o       # The server logic is in main.cpp
CLIENT_OBJECT = client.o
TEST_OBJECT = test.o

# List of object files for each executable
SERVER_OBJECTS = $(SERVER_OBJECT) $(LSM_TREE_OBJECT) $(BLOOM_FILTER_OBJECT)
# Assuming the client does not directly link against LSM tree or Bloom Filter
CLIENT_OBJECTS = $(CLIENT_OBJECT)
# Test requires LSM tree and Bloom Filter
TEST_OBJECTS = $(TEST_OBJECT) $(LSM_TREE_OBJECT) $(BLOOM_FILTER_OBJECT)

# List of ALL object files that might be created
ALL_OBJECTS = $(SERVER_OBJECT) $(CLIENT_OBJECT) $(TEST_OBJECT) $(LSM_TREE_OBJECT) $(BLOOM_FILTER_OBJECT)

# Default target builds server, client, and test
all: $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET)

# --- Linking Rules ---

# Link the server application - requires threading
$(SERVER_TARGET): $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) $(SERVER_OBJECTS) $(PTHREAD_LDFLAGS) -o $(SERVER_TARGET)

# Link the client application - assuming it might need threading support too
$(CLIENT_TARGET): $(CLIENT_OBJECTS)
	$(CC) $(CFLAGS) $(CLIENT_OBJECTS) $(PTHREAD_LDFLAGS) -o $(CLIENT_TARGET)

# Link the test application - requires threading
$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(TEST_OBJECTS) $(PTHREAD_LDFLAGS) -o $(TEST_TARGET)

# --- Compilation Rules (.o files) ---

# Compile lsm_tree.cpp (depends on lsm_tree.hh and bloom_filter.hh)
$(LSM_TREE_OBJECT): lsm_tree.cpp lsm_tree.hh bloom_filter.hh
	$(CC) $(CFLAGS) -c $< -o $@

# Compile main.cpp (the server logic) - depends on lsm_tree.hh
$(SERVER_OBJECT): main.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c $< -o $@

# Compile client.cpp (the client logic) - depends only on client.cpp
$(CLIENT_OBJECT): client.cpp
	$(CC) $(CFLAGS) -c $< -o $@

# Compile test.cpp - depends on lsm_tree.hh
$(TEST_OBJECT): test.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c $< -o $@

# Compile bloom_filter.cpp
$(BLOOM_FILTER_OBJECT): bloom_filter.cpp bloom_filter.hh
	$(CC) $(CFLAGS) -c $< -o $@

# --- Clean Rule ---

# Clean up executables, object files, and generated SSTable files/directories
clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET) $(ALL_OBJECTS)
	# Remove the data directory created by the LSM tree
	-rm -rf $(DATA_DIR)

# Phony targets
.PHONY: all clean