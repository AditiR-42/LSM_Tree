// lsm_tree.hh
#ifndef LSM_TREE_HH
#define LSM_TREE_HH

#include <vector>
#include <string>
#include <fstream>
#include <utility>
#include <cmath> 
#include "bloom_filter.hh" 

// --- Constants ---
const int MEMTABLE_CAPACITY = 50;
const int INITIAL_LEVEL_CAPACITY = 10; // Capacity logic is less strict with tiering/runs
const int SIZE_RATIO = 5; // Number of runs allowed in a level before merging
const int MAX_LEVELS = 10;
const std::string SST_FILE_PREFIX = "run_"; // Using "run_" as in the .cpp
const std::string SST_FILE_SUFFIX = ".txt";
const int BLOCK_SIZE = 1024; // Define block size in bytes for fence pointers

// Constants for Bloom Filter sizing within LSM Tree
// Estimated number of elements for a flush from memtable
const int BLOOM_FILTER_ESTIMATED_N_FLUSH = MEMTABLE_CAPACITY;
// Estimated number of elements for a merged run (can be larger than flush)
// A simple heuristic: assume unique keys are roughly the average of input run sizes.
// But a safer bet is an upper bound. Sum of input sizes is an upper bound.
// Let's use a simple multiple of MEMTABLE_CAPACITY as a guess.
const int BLOOM_FILTER_ESTIMATED_N_MERGE = SIZE_RATIO * MEMTABLE_CAPACITY; // Or potentially larger


// Simple Key-Value struct
struct key_value {
    int key;
    int value;
    bool tombstone;

    // Use default arguments for flexibility (allows key_value(), key_value(k), key_value(k,v), etc.)
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
    // Fence pointers: vector of pairs (key, byte_offset)
    std::vector<std::pair<int, long long>> fence_pointers;
    BloomFilter filter; // Add Bloom Filter member

    // Constructor to initialize members
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

    // Store SSTableInfo objects instead of just filenames
    std::vector<SSTableInfo> sstable_runs_;

    level(int capacity, int curr_level);
    ~level(); // Destructor

    // Helper to get number of runs
    size_t get_run_count() const { return sstable_runs_.size(); }

    // Add a new SSTable run (takes SSTableInfo which now includes filter)
    void add_run(SSTableInfo&& info); // Use move semantics

    // Search for a key within all runs of this level (now using Bloom filters)
    bool find_key(int key, int& value, bool& is_tombstone);

    // Get list of filenames for deletion/merge
    std::vector<std::string> get_run_filenames() const;
    // Clear all runs from this level (used after merge)
    void clear_runs();
};

// --- Memtable Class ---
class memtable {
public:
    std::vector<key_value> memtable_;
    int capacity_ = MEMTABLE_CAPACITY;
    int curr_size_ = 0;

    memtable();

    // Insert key-value pair
    // Returns true if successful, false otherwise (e.g., if memtable needs flushing)
    bool insert(key_value kv_pair);

    // Check if memtable is full
    bool is_full() const { return curr_size_ >= capacity_; }

    // Get current data (sorted) and clear memtable
    std::vector<key_value> flush();

    // Find key in memtable
    bool find_key(int key, int& value, bool& is_tombstone);
};

// --- LSM Tree Class ---
class lsm_tree {
private:
    memtable* memtable_ptr_;
    std::vector<level*> levels_; // levels_[0] unused, levels_[1] is Level 1, etc.
    long long next_run_id_ = 0; // Simple way to generate unique run IDs

    // Helper to generate unique SSTable filenames
    std::string generate_sstable_filename(int level_num);

    // Helper to write sorted data to an SSTable file, returning SSTableInfo (now including filter)
    SSTableInfo write_sstable(const std::vector<key_value>& data, const std::string& filename, size_t estimated_n_for_filter);

    // Helper function for the k-way merge, returning SSTableInfo (now including filter)
    SSTableInfo merge_runs(int target_level_num, const std::vector<SSTableInfo>& runs_to_merge_info, size_t estimated_n_for_filter);

    // Helper function to rebuild fence pointers AND Bloom Filter for an existing file
    SSTableInfo rebuild_run_info(const std::string& filename);


    // Function to check and trigger merges starting from a level
    void check_and_trigger_merge(int level_num);

    // Helper to delete SSTable files (takes filenames)
    void delete_sst_files(const std::vector<std::string>& filenames);


public:
    lsm_tree();
    ~lsm_tree(); // Important destructor to clean up levels and memtable

    // Public Interface
    bool insert(key_value kv_pair);
    int get(int key, bool called_from_range = false);
    void range(int start, int end);
    void delete_key(int key);
    void printStats();
    void cleanup_files(); // Explicit cleanup function
};

#endif // LSM_TREE_HH