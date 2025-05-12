# Makefile

CC = g++
CFLAGS = -Wall -Wextra -pedantic -std=c++11 -O3

# Server requires threading support
SERVER_LDFLAGS = -pthread

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
CLIENT_OBJECTS = $(CLIENT_OBJECT)
TEST_OBJECTS = $(TEST_OBJECT) $(LSM_TREE_OBJECT) $(BLOOM_FILTER_OBJECT)

# List of ALL object files that might be created
ALL_OBJECTS = $(SERVER_OBJECT) $(CLIENT_OBJECT) $(TEST_OBJECT) $(LSM_TREE_OBJECT) $(BLOOM_FILTER_OBJECT)

# Default target builds server and client (and test if you keep it)
all: $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET)

# --- Linking Rules ---

# Link the server application
$(SERVER_TARGET): $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) $(SERVER_OBJECTS) $(SERVER_LDFLAGS) -o $(SERVER_TARGET)

# Link the client application
$(CLIENT_TARGET): $(CLIENT_OBJECTS)
	$(CC) $(CFLAGS) $(CLIENT_OBJECTS) $(CLIENT_LDFLAGS) -o $(CLIENT_TARGET)

# Link the test application
$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(TEST_OBJECTS) $(SERVER_LDFLAGS) -o $(TEST_TARGET) # Assuming test might use threading

# --- Compilation Rules (.o files) ---

# Compile lsm_tree.cpp (depends on lsm_tree.hh and bloom_filter.hh)
$(LSM_TREE_OBJECT): lsm_tree.cpp lsm_tree.hh bloom_filter.hh
	$(CC) $(CFLAGS) -c lsm_tree.cpp -o $(LSM_TREE_OBJECT)

# Compile main.cpp (the server logic)
# Depends on main.cpp and the main header it includes (lsm_tree.hh)
# Networking headers are included within main.cpp, no need to list here.
$(SERVER_OBJECT): main.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c main.cpp -o $(SERVER_OBJECT)

# Compile client.cpp (the client logic)
# Depends only on client.cpp. It doesn't include lsm_tree.hh or bloom_filter.hh
$(CLIENT_OBJECT): client.cpp
	$(CC) $(CFLAGS) -c client.cpp -o $(CLIENT_OBJECT)

# Compile test.cpp
$(TEST_OBJECT): test.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c test.cpp -o $(TEST_OBJECT)

# Compile bloom_filter.cpp
$(BLOOM_FILTER_OBJECT): bloom_filter.cpp bloom_filter.hh
	$(CC) $(CFLAGS) -c bloom_filter.cpp -o $(BLOOM_FILTER_OBJECT)

# --- Clean Rule ---

# Clean up executables, object files, and generated SSTable files
clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET) $(ALL_OBJECTS)

# Phony targets
.PHONY: all clean