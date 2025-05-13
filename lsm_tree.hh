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
#include <future>   
#include <tuple>    
#include <optional> 
#include <map>
#include <limits>

#include "bloom_filter.hh"

// --- Constants ---
const int MEMTABLE_CAPACITY = 100;
const int INITIAL_LEVEL_CAPACITY = 10; 
const int SIZE_RATIO = 5; 
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
     bool operator==(const key_value& other) const {
        return key == other.key && value == other.value && tombstone == other.tombstone;
    }
};

// Struct to hold SSTable filename, its fence pointers, and its Bloom Filter
struct SSTableInfo {
    std::string filename;
    std::vector<std::pair<int, long long>> fence_pointers;
    BloomFilter filter; 
    int min_key = std::numeric_limits<int>::max(); 
    int max_key = std::numeric_limits<int>::min(); 

    SSTableInfo() = default;

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
    int capacity_; 
    int curr_level_;
    level* next_ = nullptr;

    std::vector<SSTableInfo> sstable_runs_;
    mutable std::mutex level_mutex_; // Use mutable because find_key is const but needs to lock

    level(int capacity, int curr_level);
    ~level(); // Destructor

    size_t get_run_count() const { return sstable_runs_.size(); }

    void add_run(SSTableInfo&& info); 

    // Search for a key within all runs of this level (using Bloom filters)
    // Returns true if found, updates value and is_tombstone. Assumes lock held by caller.
    bool find_key(int key, int& value, bool& is_tombstone) const;

    // Helper specifically for parallel search to return optional<tuple<...>>
    // Acquires and releases its own lock internally.
    std::optional<std::tuple<int, int, bool, int>> find_key_parallel(int key) const;

    std::vector<std::string> get_run_filenames() const;
    void clear_runs();
};

// --- Memtable Class ---
class memtable {
public:
    std::map<int, key_value> memtable_;
    size_t capacity_ = 0;   
    size_t cur_size_ = 0;  
    std::mutex memtable_mutex_;

    memtable(size_t capacity);
    memtable();

    void insert(key_value kv_pair, bool& trigger_flush);

    bool is_full() const {
        return memtable_.size() >= static_cast<decltype(memtable_)::size_type>(capacity_);
    }

    std::vector<key_value> flush();

    bool find_key(int key, int& value, bool& is_tombstone);
};

// --- LSM Tree Class ---
class lsm_tree {
private:
    memtable* memtable_ptr_;
    std::vector<level*> levels_; 
    long long next_run_id_ = 0; 
    std::mutex id_mutex_;
    mutable std::mutex cout_mutex_; // Use mutable because printing from const methods needs lock
    std::mutex file_delete_mutex_;
    std::vector<std::future<void>> background_tasks_;

    std::thread flusher_thread_; // Dedicated thread for flushing memtable
    std::mutex flush_mutex_;            
    std::condition_variable flush_request_cv_; // Condition variable to wake flusher
    bool flush_needed_ = false; 
    std::atomic<bool> shutdown_requested_ = false;

    void flushThreadLoop();

    std::string generate_sstable_filename(int level_num);

    SSTableInfo write_sstable(const std::vector<key_value>& data, const std::string& filename, size_t estimated_n_for_filter);

    SSTableInfo merge_runs(int target_level_num, const std::vector<SSTableInfo>& runs_to_merge_info, size_t estimated_n_for_filter);

    SSTableInfo rebuild_run_info(const std::string& filename);

    void check_and_trigger_merge(int level_num);

    void delete_sst_files(const std::vector<std::string>& filenames);

    void load_file(const std::string& fileName); // private helper

    std::optional<key_value> getValueForKey(int key) const;


public:
    lsm_tree();
    ~lsm_tree();

    // Public Interface (modified to accept ostream for output where needed)
    bool insert(key_value kv_pair);

    int get(int key, std::ostream& os); 

    void range(int start, int end, std::ostream& os);

    void delete_key(int key);

    void printStats(std::ostream& os) const;

    void cleanup_files();

    void load(const std::string& fileName);
};

#endif // LSM_TREE_HH