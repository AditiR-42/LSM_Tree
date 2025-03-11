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

## Phase 2: Concurrency