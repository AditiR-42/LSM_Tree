// lsm_tree.hh
#ifndef LSM_TREE_HH
#define LSM_TREE_HH

#include <vector>
#include <string>
#include <fstream>

// --- Constants ---
const int MEMTABLE_CAPACITY = 50;               // Example capacity
const int INITIAL_LEVEL_CAPACITY = 10;          // Initial capacity of Level 1
const int SIZE_RATIO = 5;                       // Tiering Threshold: Merge when a level has this many runs
const int MAX_LEVELS = 10;                      // Maximum number of levels
const std::string SST_FILE_PREFIX = "level_";
const std::string SST_FILE_SUFFIX = ".txt";

// --- Data Structures ---

// Simple Key-Value struct (ensure it's POD or handle serialization carefully)
struct key_value {
    int key;
    int value;
    bool tombstone;

    // Use default arguments for flexibility (allows key_value(), key_value(k), key_value(k,v), etc.)
    key_value(int k = 0, int v = 0, bool t = false)
        : key(k), value(v), tombstone(t) {}
    // ---------------------------

    // Overload < operator for sorting
    bool operator<(const key_value& other) const {
        return key < other.key;
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

    // Store filenames of SSTables instead of data
    std::vector<std::string> sstable_files_;

    level(int capacity, int curr_level);
    ~level(); // Destructor to potentially clean up files if needed

    // Helper to get number of runs
    size_t get_run_count() const { return sstable_files_.size(); }

    // Add a new SSTable file (run) to this level
    void add_run(const std::string& filename);

    // Function to perform the merge (now internal logic) - might be better in lsm_tree
    // bool merge(std::vector<key_value>& data_to_merge); // Old merge - remove or repurpose

    // Search for a key within all runs of this level
    bool find_key(int key, int& value, bool& is_tombstone);
};

// --- Memtable Class ---
class memtable {
public:
    std::vector<key_value> memtable_;
    int capacity_ = MEMTABLE_CAPACITY;
    int curr_size_ = 0;
    // level* level1ptr_ = nullptr; // Level 1 ptr is managed by lsm_tree now

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

    // Helper to write sorted data to an SSTable file
    bool write_sstable(const std::vector<key_value>& data, const std::string& filename);

    // Helper function for the k-way merge
    std::string merge_runs(int level_num, const std::vector<std::string>& runs_to_merge);

    // Function to check and trigger merges starting from a level
    void check_and_trigger_merge(int level_num);

    // Helper to delete SSTable files
    void delete_sst_files(const std::vector<std::string>& filenames);

public:
    lsm_tree();
    ~lsm_tree(); // Important destructor to clean up levels and memtable

    // Public Interface
    bool insert(key_value kv_pair);
    int get(int key, bool called_from_range = false);
    void range(int start, int end);
    void delete_key(int key);
    void printStats(); // Needs significant update
    void cleanup_files(); // Explicit cleanup function
};


#endif // LSM_TREE_HH