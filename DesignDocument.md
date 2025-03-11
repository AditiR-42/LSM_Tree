# Design Document

## Phase 1: LSM Tree Core Components

1. Key-Value Data Structure Definition
- Can be as simple as a key-value pair struct

2. Memtable Implementation
- Choose in-memory data structure for memtable
- Ex. skip list, heap, map
- Include put(key, value), get(key), and delete(key) (tombstone) operations
- Implement method to flush memtable to disk as a sorted string table

3. Sorted String Table Implementation
- Define on-disk table format (magic number, metadata (version number, bloom filter parameters, number of entires, min/max key), bloom filter, fence pointers, data blocks, checksums)
- Implement functions to write and read SSTables to/from disk (serialize in binary)
- Implement fence pointers at each level to allow page/block access
- Implement bloom filters at each level with optimized bits per entry to quickly determine if key is not present at that level
- Implement LSM tree 
    - Start with single level
    - Define size ratios betwen levels (ex. L1 = L0 * 10)
    - Create data directory
    - Implement method to load SSTable into LSM tree structure
    - Create manifest structure to store current snapshot of levels of SSTables

4. Merge Policy and Data Persistence
- Implement tiering merge policy (write-optimized): when the number of SSTables in Level 0 exceeds a threshold, merge them into a larger SSTable and move it to Level 1
- Implement tombstone records for deleted keys
- Implement shutdown procedures: flush memtable ot disk, close open file handles
- Implement startup procedures: scan data directory, load all SSTables into LSM tree structure, rebuild bloom filters and fence pointers

5. Server and Client Implementation
- Implement server that listens for client connections using sockets
- Implement CS265 DSL parsing:
    - ```PUT <key> <value>```
    - ```GET <key>```
    - ```DELETE <key>```
- Implement client that can connect to server and send CS265 DSL commands and display results from server

## Phase 2: Concurrency

## Phase 3: Optimizations