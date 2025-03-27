#ifndef LSM_TREE_HH
#define LSM_TREE_HH

#include <iostream>
#include <algorithm>
#include <vector> 
#include <memory> 

#define INITIAL_LEVEL_CAPACITY  10
#define SIZE_RATIO              10
#define MEMTABLE_CAPACITY       10
#define MAX_LEVELS              10
#define NUM_ENTRIES_PER_PAGE    341 // 4096 divided by 12
#define KEY_VALUE_SIZE          12

struct key_value {
    int key;
    int value;
    bool tombstone;
};

class level {
public:
    int capacity_;
    int curr_size_ = 0;
    int curr_level_;
    std::vector<key_value> sstable_;
    level* prev_ = nullptr;
    level* next_ = nullptr;

    level(int capacity, int curr_level);
    bool merge(std::vector<key_value>& child_data, int num_elements_to_merge);
};

class memtable {
public:
    level* level1ptr_;
    int capacity_ = MEMTABLE_CAPACITY;
    int curr_size_ = 0;
    std::vector<key_value> memtable_;  // Use std::vector instead of a raw pointer

    memtable();  // Constructor declaration
    bool insert(key_value kv_pair);  // Insert declaration
};

class lsm_tree {
public:
    memtable* memtable_ptr_;
    std::vector<level*> levels_; // Using a vector to hold the levels

    lsm_tree();  // Constructor declaration
    bool insert(key_value kv_pair);
    int get(int key, bool called_from_range = false);
    void range(int start, int end);
    void delete_key(int key);
    void printStats();
};

#endif