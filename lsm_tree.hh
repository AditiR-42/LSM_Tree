// lsm_tree.hh
#ifndef LSM_TREE_HH
#define LSM_TREE_HH

#include <vector>
#include <string>
#include <fstream>
#include <utility>
#include <cmath>
#include <ostream>
#include <mutex>

#include "bloom_filter.hh"

// --- Constants ---
const int MEMTABLE_CAPACITY = 50;
const int INITIAL_LEVEL_CAPACITY = 10; // Capacity logic is less strict with tiering/runs
const int SIZE_RATIO = 5; // Number of runs allowed in a level before merging
const int MAX_LEVELS = 10;
const std::string SST_FILE_PREFIX = "run_";
const std::string SST_FILE_SUFFIX = ".txt";
const int BLOCK_SIZE = 1024; // Define block size in bytes for fence pointers

// Constants for Bloom Filter sizing within LSM Tree
const int BLOOM_FILTER_ESTIMATED_N_FLUSH = MEMTABLE_CAPACITY;
const int BLOOM_FILTER_ESTIMATED_N_MERGE = SIZE_RATIO * MEMTABLE_CAPACITY;

// Simple Key-Value struct
struct key_value {
    int key;
    int value;
    bool tombstone;

    key_value(int k = 0, int v = 0, bool t = false)
        : key(k), value(v), tombstone(t) {}

    // Overload < operator for sorting
    bool operator<(const key_value& other) const {
        return key < other.key;
    }
};

// Struct to hold SSTable filename, its fence pointers, and its Bloom Filter
struct SSTableInfo {
    std::string filename;
    std::vector<std::pair<int, long long>> fence_pointers;
    BloomFilter filter;

    SSTableInfo(std::string fn = "", std::vector<std::pair<int, long long>> fp = {}, BloomFilter bf = BloomFilter())
        : filename(std::move(fn)), fence_pointers(std::move(fp)), filter(std::move(bf)) {}

    // Enable move semantics for efficient transfer
    SSTableInfo(const SSTableInfo& other) = default;
    SSTableInfo& operator=(const SSTableInfo& other) = default;
    SSTableInfo(SSTableInfo&& other) = default;
    SSTableInfo& operator=(SSTableInfo&& other) = default;
};

class level;
class memtable;

// --- Level Class ---
class level {
public:
    int capacity_; // Max number of elements (approximate, less strict with tiering)
    int curr_level_;
    level* next_ = nullptr; // Pointer to the next level

    std::vector<SSTableInfo> sstable_runs_;
    mutable std::mutex level_mutex_;

    level(int capacity, int curr_level);
    ~level(); // Destructor

    size_t get_run_count() const { return sstable_runs_.size(); }

    void add_run(SSTableInfo&& info); // Use move semantics

    // Search for a key within all runs of this level (now using Bloom filters)
    bool find_key(int key, int& value, bool& is_tombstone) const;

    std::vector<std::string> get_run_filenames() const;
    void clear_runs();
};

// --- Memtable Class ---
class memtable {
public:
    std::vector<key_value> memtable_;
    int capacity_ = MEMTABLE_CAPACITY;
    int curr_size_ = 0;
    std::mutex memtable_mutex_;

    memtable();

    bool insert(key_value kv_pair);

    bool is_full() const { return curr_size_ >= capacity_; }

    std::vector<key_value> flush();

    bool find_key(int key, int& value, bool& is_tombstone);
};

// --- LSM Tree Class ---
class lsm_tree {
private:
    memtable* memtable_ptr_;
    std::vector<level*> levels_; // levels_[0] unused, levels_[1] is Level 1, etc.
    long long next_run_id_ = 0; // Simple way to generate unique run IDs
    std::mutex id_mutex_; 
    mutable std::mutex cout_mutex_;
    std::mutex file_delete_mutex_;

    // Helper to generate unique SSTable filenames
    std::string generate_sstable_filename(int level_num);

    // Helper to write sorted data to an SSTable file, returning SSTableInfo
    SSTableInfo write_sstable(const std::vector<key_value>& data, const std::string& filename, size_t estimated_n_for_filter);

    // Helper function for the k-way merge, returning SSTableInfo
    SSTableInfo merge_runs(int target_level_num, const std::vector<SSTableInfo>& runs_to_merge_info, size_t estimated_n_for_filter);

    // Helper function to rebuild fence pointers AND Bloom Filter for an existing file
    SSTableInfo rebuild_run_info(const std::string& filename);

    // Function to check and trigger merges starting from a level
    void check_and_trigger_merge(int level_num);

    // Helper to delete SSTable files (takes filenames)
    void delete_sst_files(const std::vector<std::string>& filenames);

    // Note: This now assumes the generator directory exists relative to the server's working directory.
    void load_file(const std::string& fileName);


public:
    lsm_tree();
    ~lsm_tree();

    // Public Interface (modified to accept ostream for output where needed)
    bool insert(key_value kv_pair);
    // get now returns int and writes to ostream if found and not tombstone
    int get(int key, std::ostream& os, bool called_from_range = false);
    // range now writes to ostream
    void range(int start, int end, std::ostream& os);
    void delete_key(int key);
    // printStats now writes to ostream
    void printStats(std::ostream& os) const;

    // Explicit cleanup function
    void cleanup_files();

    // Public wrapper for load_file to be accessible by server command handler
    void load(const std::string& fileName);
};

#endif // LSM_TREE_HH