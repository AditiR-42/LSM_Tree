// lsm_tree.hh
#ifndef LSM_TREE_HH
#define LSM_TREE_HH

#include <vector>
#include <string>
#include <fstream>
#include <limits> // Required for numeric_limits
#include <utility> // Required for std::pair

// --- Constants ---
const int MEMTABLE_CAPACITY = 50;               
const int INITIAL_LEVEL_CAPACITY = 10;          
const int SIZE_RATIO = 5;                       
const int MAX_LEVELS = 10;                     
// const std::string SST_FILE_PREFIX = "level_"; // No longer strictly used in naming
const std::string SST_FILE_SUFFIX = ".sst"; // Changed suffix for binary format
const size_t BLOCK_SIZE_BYTES = 4096; // 4KB block size

// --- Data Structures ---

// Simple Key-Value struct (still POD)
struct key_value {
    int key;
    int value;
    bool tombstone; // Stored as char 0 or 1 in binary

    // Use default arguments for flexibility
    key_value(int k = 0, int v = 0, bool t = false)
        : key(k), value(v), tombstone(t) {}

    // Overload < operator for sorting (used by std::sort and std::map/set)
    bool operator<(const key_value& other) const {
        return key < other.key;
    }
};

// Struct to hold SSTable file metadata (filename and block index)
// min_key and max_key are derived from the index/file content
struct SstMetadata {
    std::string filename;
    // Block index: vector of {last_key_in_block, offset_of_block} pairs
    std::vector<std::pair<int, long long>> block_index_;

    // Derived min/max keys for quick initial range check
    int min_key = std::numeric_limits<int>::max();
    int max_key = std::numeric_limits<int>::min();

    SstMetadata(std::string fname = "") : filename(fname) {}

    // Helper to check if the metadata is valid (implies a non-empty file/index)
    bool is_valid() const {
        return !filename.empty() && !block_index_.empty();
    }
};


// Forward declarations
class level;
class memtable;

// --- Level Class ---
class level {
public:
    int capacity_; // Max number of *elements* (approximate, less strict with tiering)
    int curr_level_;
    level* next_ = nullptr; // Pointer to the next level

    // Store metadata about SSTables including the block index
    std::vector<SstMetadata> sstable_metadata_;

    level(int capacity, int curr_level);
    ~level();

    // Helper to get number of runs
    size_t get_run_count() const { return sstable_metadata_.size(); }

    // Add a new SSTable file metadata to this level
    void add_run(const SstMetadata& metadata);

    // Search for a key within all runs of this level, using block index and min/max to prune
    bool find_key(int key, int& value, bool& is_tombstone);

    // Get a list of all SSTable metadata in this level
    const std::vector<SstMetadata>& get_metadata() const { return sstable_metadata_; }
};

// --- Memtable Class ---
class memtable {
public:
    std::vector<key_value> memtable_; // Data stored directly in vector
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

    // Find key in memtable (linear scan)
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

    // Helper to write sorted data to a BINARY SSTable file with block index
    // Pass data by const ref, return SstMetadata (includes filename and block index)
    SstMetadata write_sstable(const std::vector<key_value>& data, const std::string& filename);

    // Helper function for the k-way merge on BINARY SSTables, returns metadata for the merged file
    SstMetadata merge_runs(int target_level_num, const std::vector<SstMetadata>& runs_to_merge_metadata);

    // Function to check and trigger merges starting from a level
    void check_and_trigger_merge(int level_num);

    // Helper to delete SSTable files given their metadata
    void delete_sst_files(const std::vector<SstMetadata>& files_metadata);

    // Helper to read metadata (including block index) from an existing BINARY SSTable file
    SstMetadata read_sst_metadata(const std::string& filename);


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