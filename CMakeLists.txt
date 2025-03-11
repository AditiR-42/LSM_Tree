
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