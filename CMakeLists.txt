
```cmake
cmake_minimum_required(VERSION 3.15)
project(LSM_Tree)

set(CMAKE_CXX_STANDARD 17)  # Or a higher standard if desired
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(include)  #include is now the parent directory for all include folders
include_directories(include/core) #explicit include paths
include_directories(include/lsm)
include_directories(include/server)

# Source files for the server
set(SERVER_SOURCES
    src/server/main.cpp
    src/server/server.cpp
    src/core/keyvalue.h
    src/lsm/memtable.cpp
    src/lsm/sstable.cpp
    src/lsm/lsm_tree.cpp
    src/lsm/tiering_merge_policy.cpp
    src/core/bloom_filter.cpp
)

# Source files for the client
set(CLIENT_SOURCES
    src/client/client_main.cpp
    src/client/client.cpp
    src/core/keyvalue.h
)

# Add executable for the server
add_executable(LSM_Tree_server ${SERVER_SOURCES})

# Add executable for the client
add_executable(LSM_Tree_client ${CLIENT_SOURCES})

# Test execution setup
enable_testing()

# Add test executables for different modules
add_executable(bloom_filter_test test/core/bloom_filter_test.cpp src/core/bloom_filter.cpp)
add_test(NAME bloom_filter_test COMMAND bloom_filter_test)

add_executable(memtable_test test/lsm/memtable_test.cpp src/lsm/memtable.cpp)
add_test(NAME memtable_test COMMAND memtable_test)

add_executable(sstable_test test/lsm/sstable_test.cpp src/lsm/sstable.cpp src/lsm/lsm_tree.cpp src/core/bloom_filter.cpp)
add_test(NAME sstable_test COMMAND sstable_test)

add_executable(lsm_tree_test test/lsm/lsm_tree_test.cpp src/lsm/lsm_tree.cpp src/lsm/sstable.cpp src/lsm/memtable.cpp src/core/bloom_filter.cpp)
add_test(NAME lsm_tree_test COMMAND lsm_tree_test)

add_executable(server_test test/server/server_test.cpp src/server/server.cpp src/lsm/lsm_tree.cpp src/lsm/sstable.cpp src/lsm/memtable.cpp src/core/bloom_filter.cpp)
add_test(NAME server_test COMMAND server_test)