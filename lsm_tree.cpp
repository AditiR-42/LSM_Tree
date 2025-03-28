#include "lsm_tree.hh"
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <fstream>
#include <cstdio> 
#include <queue> 
#include <limits> 
#include <map> 
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h> 
#include <dirent.h> 

using namespace std;

// --- Define data directory constant ---
const std::string DATA_DIR = "data";

// --- Helper Function (Example - needs error checking) ---
bool directory_exists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return false; // Cannot access
    }
    return (info.st_mode & S_IFDIR) != 0; // Check if it's a directory
}

bool create_directory(const std::string& path) {
    // Mode 0755 (rwxr-xr-x) - adjust if needed
    if (mkdir(path.c_str(), 0755) == 0) {
        // std::cout << "Created directory: " << path << std::endl;
        return true;
    } else {
        // Check if it already exists (EEXIST is okay)
        if (errno == EEXIST && directory_exists(path)) {
            return true; // Already exists, that's fine
        }
        std::cerr << "Error creating directory " << path << ": " << strerror(errno) << std::endl;
        return false;
    }
}

// --- Helper Struct for Merge ---
struct merge_entry {
    key_value kv;
    size_t stream_index; // Which input file this entry came from

    // Custom comparator for min-heap (priority queue) based on key
    bool operator>(const merge_entry& other) const {
        // If keys are equal, prefer the one from the lower stream index (arbitrary but consistent)
        if (kv.key == other.kv.key) {
            return stream_index > other.stream_index; // Or some other tie-breaker
        }
        return kv.key > other.kv.key;
    }
};


// --- Level Class Implementation ---
level::level(int capacity, int curr_level) : capacity_(capacity), curr_level_(curr_level) {
    // sstable_files_ is already default-initialized (empty vector)
}

level::~level() {
    // Optional: Decide if level destructor should delete its files.
    // It might be safer to do this explicitly in lsm_tree destructor or cleanup.
}

void level::add_run(const std::string& filename) {
    sstable_files_.push_back(filename);
}

// Search key in this level's SSTables (files)
bool level::find_key(int key, int& value, bool& is_tombstone) {
    // Search runs in reverse order (newest first)
    for (auto it = sstable_files_.rbegin(); it != sstable_files_.rend(); ++it) {
        const std::string& filename = *it;
        // Open in text mode (default)
        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Warning: Could not open SSTable TXT file for reading: " << filename << std::endl;
            continue; // Skip this file if it can't be opened
        }

        int current_key;
        int current_value;
        int tombstone_flag; // Read tombstone as 0 or 1

        // Read file line by line, parsing space-separated values
        while (infile >> current_key >> current_value >> tombstone_flag) {
            if (current_key == key) {
                value = current_value;
                is_tombstone = (tombstone_flag == 1); // Convert 0/1 back to bool
                infile.close();
                return true; // Key found
            }
            // Optimization: If we pass the key in a sorted file, it won't be found later in this file
            if (current_key > key) {
                 // Since the file is sorted, no need to read further in *this* file
                 break;
            }
        }

        // Check for read errors that didn't result in EOF
        if (!infile.eof() && infile.fail()) {
             std::cerr << "Warning: Read error or parsing issue in SSTable TXT file: " << filename << std::endl;
        }

        infile.close(); // Close the file stream
    }
    return false; // Key not found in any run of this level
}


// --- Memtable Class Implementation ---
memtable::memtable() {
    memtable_.reserve(MEMTABLE_CAPACITY);
}

bool memtable::insert(key_value kv_pair) {
    // Linear search for update (can optimize with map/skip list if memtable gets large)
    for (int i = 0; i < curr_size_; ++i) {
        if (memtable_[i].key == kv_pair.key) {
            memtable_[i].value = kv_pair.value;
            memtable_[i].tombstone = kv_pair.tombstone; // Update tombstone status too
            return true;
        }
    }

    // If key not found and memtable is full, signal to flush (caller handles flush)
    if (is_full()) {
       return false; // Indicate memtable is full
    }

    // Add new entry
    memtable_.push_back(kv_pair);
    ++curr_size_;
    return true;
}

std::vector<key_value> memtable::flush() {
    // Sort memtable before flushing
    std::sort(memtable_.begin(), memtable_.end());

    // Create a copy to return
    std::vector<key_value> data_to_flush = memtable_;

    // Clear the current memtable
    memtable_.clear();
    memtable_.reserve(MEMTABLE_CAPACITY); // Re-reserve capacity
    curr_size_ = 0;

    return data_to_flush;
}

bool memtable::find_key(int key, int& value, bool& is_tombstone) {
     // Search in reverse for newest value (although linear scan finds first)
     // Optimization: If updates are common, searching backwards might be slightly faster
     for (int i = curr_size_ - 1; i >= 0; --i) {
        if (memtable_[i].key == key) {
            value = memtable_[i].value;
            is_tombstone = memtable_[i].tombstone;
            return true;
        }
    }
    return false;
}


// --- LSM_Tree Class Implementation ---
lsm_tree::lsm_tree() : next_run_id_(0) { // Initialize next_run_id_
    memtable_ptr_ = new memtable();
    levels_.resize(MAX_LEVELS + 1, nullptr);

    // 1. Create root data directory
    if (!create_directory(DATA_DIR)) {
        // Handle critical error - cannot proceed without data directory
        throw std::runtime_error("Failed to create or access data directory: " + DATA_DIR);
    }

    long long max_run_id_found = -1;

    // 2. Create levels and load existing SSTables
    int current_capacity = INITIAL_LEVEL_CAPACITY; // Capacity logic might be less relevant now
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        levels_[i] = new level(current_capacity, i);
        if (i > 1) {
            levels_[i-1]->next_ = levels_[i];
        }

        // Create level subdirectory
        std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
        if (!create_directory(level_dir)) {
             throw std::runtime_error("Failed to create or access level directory: " + level_dir);
        }

        // --- Load existing files for this level ---
        DIR *dirp = opendir(level_dir.c_str());
        if (dirp) {
            struct dirent *dp;
            while ((dp = readdir(dirp)) != nullptr) {
                std::string filename = dp->d_name;
                // Check if it's an SST file (simple check)
                if (filename.length() > 4 && filename.substr(filename.length() - 4) == SST_FILE_SUFFIX) {
                    std::string full_path = level_dir + "/" + filename;
                    levels_[i]->add_run(full_path); // Add full path
                    // std::cout << "Found existing SSTable: " << full_path << std::endl;

                    // Parse run ID from filename (e.g., "run_123.sst") - basic example
                    size_t run_pos = filename.find("run_");
                    size_t sst_pos = filename.rfind(SST_FILE_SUFFIX);
                    if (run_pos != std::string::npos && sst_pos != std::string::npos) {
                         try {
                             long long run_id = std::stoll(filename.substr(run_pos + 4, sst_pos - (run_pos + 4)));
                             if (run_id > max_run_id_found) {
                                 max_run_id_found = run_id;
                             }
                         } catch (...) {
                              std::cerr << "Warning: Could not parse run ID from filename: " << filename << std::endl;
                         }
                    }
                }
            }
            closedir(dirp);
        } else {
             std::cerr << "Warning: Could not open level directory for reading: " << level_dir << std::endl;
        }
        // Sort runs after loading? Maybe by run_id if needed, but order added matters for tiering.
        // For tiering, the order usually doesn't strictly matter as they all get merged.
        // For lookup, searching newest (highest run_id) first might be desired. This requires sorting or smarter loading.

        current_capacity *= SIZE_RATIO;
    }

     // Set the next run ID to be one greater than the highest found
    next_run_id_ = max_run_id_found + 1;
    // std::cout << "Starting next run ID at: " << next_run_id_ << std::endl;
}

lsm_tree::~lsm_tree() {
    if (memtable_ptr_ && memtable_ptr_->curr_size_ > 0) {
        // std::cout << "LSM Tree Destructor: Memtable not empty, performing final flush..." << std::endl;
        std::vector<key_value> data_to_flush = memtable_ptr_->flush(); // Flush remaining data

        if (!data_to_flush.empty()) {
            // Generate filename for Level 1
            std::string final_sstable_file = generate_sstable_filename(1); // Will use the next available run ID

            // Write flushed data to disk
            if (write_sstable(data_to_flush, final_sstable_file)) {
                 // Add run to Level 1's list (in memory, but won't persist unless saved)
                 // This write operation itself is the persistence step.
                 // The in-memory list update doesn't matter much here as the object is being destroyed.
                 if (levels_.size() > 1 && levels_[1]) { // Basic check
                      levels_[1]->add_run(final_sstable_file); // Update list for consistency if needed elsewhere
                 }
                 std::cout << "Final flush successful to: " << final_sstable_file << std::endl;
                 // NOTE: This final flush might trigger merges if SIZE_RATIO is met.
                 // Consider if check_and_trigger_merge(1) should be called here.
                 // Usually, shutdown flushes just write the file and don't trigger further compactions.
            } else {
                //  std::cerr << "Error: Failed to write final memtable flush to disk during shutdown!" << std::endl;
            }
        }
    }
    // --- End Shutdown Flush Logic ---


    delete memtable_ptr_;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) { // Check if the pointer is valid before deleting
             delete levels_[i];
        }
    }
}


// Helper to generate unique SSTable filenames
std::string lsm_tree::generate_sstable_filename(int level_num) {
    // Construct path: DATA_DIR / L<level_num> / run_<id>.sst
    std::string level_dir = DATA_DIR + "/L" + std::to_string(level_num);
    return level_dir + "/run_" + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

// Helper to write sorted data to an SSTable file
bool lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename) {
    // Open file in text mode (default) for writing
    // ios::trunc ensures it's a new file or overwrites
    std::ofstream outfile(filename, std::ios::trunc);
    if (!outfile) {
        // std::cerr << "Error: Could not open SSTable TXT file for writing: " << filename << std::endl;
        return false;
    }

    // Write each key-value pair as a line of text
    for (const auto& kv : data) {
        outfile << kv.key << " " << kv.value << " " << (kv.tombstone ? 1 : 0) << "\n";
        if (!outfile) { // Check stream state after each write
            //  std::cerr << "Error: Failed to write to SSTable TXT file: " << filename << std::endl;
             outfile.close();
             return false;
        }
    }

    outfile.close();
    if (!outfile) { // Check close status (important!)
        //  std::cerr << "Error: Failed to close SSTable TXT file properly: " << filename << std::endl;
         // File might be corrupted or incomplete even if writes seemed okay.
         return false;
    }
    // std::cout << "Successfully wrote SSTable TXT: " << filename << std::endl;
    return true;
}

// Helper to delete SSTable files
void lsm_tree::delete_sst_files(const std::vector<std::string>& filenames) {
    for (const auto& filename : filenames) {
        // Filenames should now be full paths
        if (std::remove(filename.c_str()) != 0) {
            // Use perror or strerror(errno) for better error reporting
            // std::cerr << "Warning: Could not delete SSTable file: " << filename << " (" << strerror(errno) << ")" << std::endl;
        } else {
            //  std::cout << "Deleted old SSTable: " << filename << std::endl;
        }
    }
}
// --- Merge Logic ---
// Performs a k-way merge on the given run files, writes result to a new file, returns new filename.
// Performs a k-way merge on the given run TXT files, writes result to a new TXT file.
std::string lsm_tree::merge_runs(int target_level_num, const std::vector<std::string>& runs_to_merge) {
    // ... (initial checks for empty runs, target_level_num remain same) ...

    // std::cout << "Merging " << runs_to_merge.size() << " TXT runs into level " << target_level_num << "..." << std::endl;

    std::vector<std::ifstream> input_streams;
    input_streams.reserve(runs_to_merge.size());

    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input TEXT files and read the first entry from each
    for (size_t i = 0; i < runs_to_merge.size(); ++i) {
        // Open in text mode (default)
        input_streams.emplace_back(runs_to_merge[i]);
        if (!input_streams.back()) {
            // std::cerr << "Error: Could not open TXT file for merge: " << runs_to_merge[i] << std::endl;
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
            return ""; // Indicate merge failure
        }

        int current_key, current_value, tombstone_flag;
        // Read the first line
        if (input_streams.back() >> current_key >> current_value >> tombstone_flag) {
            min_heap.push({{current_key, current_value, (tombstone_flag == 1)}, i}); // Construct key_value and push
        } else {
            // File might be empty or failed initial read
             if (!input_streams.back().eof()) { // Check if it wasn't just an empty file
                 std::cerr << "Warning: Failed initial read from TXT file: " << runs_to_merge[i] << std::endl;
             }
             input_streams.back().close();
        }
    }

    // Generate filename for the new merged run (will have .txt suffix)
    std::string output_filename = generate_sstable_filename(target_level_num);
    // Open output in text mode
    std::ofstream outfile(output_filename, std::ios::trunc);
    if (!outfile) {
        // std::cerr << "Error: Could not open output TXT file for merge: " << output_filename << std::endl;
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return ""; // Indicate merge failure
    }

    key_value last_written_kv;
    bool first_write = true;

    // Merge process
    while (!min_heap.empty()) {
        merge_entry smallest = min_heap.top();
        min_heap.pop();

        // --- Compaction/Duplicate Handling (Logic remains same, writing changes) ---
        if (first_write || smallest.kv.key != last_written_kv.key) {
            // --- Write using TEXT format ---
            outfile << smallest.kv.key << " " << smallest.kv.value << " " << (smallest.kv.tombstone ? 1 : 0) << "\n";
            if (!outfile) {
                std::cerr << "Error writing during merge to TXT file: " << output_filename << std::endl;
                // Cleanup needed
                 outfile.close();
                 for(auto& stream : input_streams) if(stream.is_open()) stream.close();
                 std::remove(output_filename.c_str()); // Attempt to remove bad output file
                 return ""; // Indicate failure
            }
            // --- End Text Write ---
            last_written_kv = smallest.kv;
            first_write = false;
        } else {
             // Duplicate key logic (no writing, just update last_written_kv if needed)
             if (!last_written_kv.tombstone && smallest.kv.tombstone) {
                 last_written_kv = smallest.kv; // Update tombstone status
             }
        }

        // Read the next element (line) from the same stream
        size_t stream_idx = smallest.stream_index;
        if (input_streams[stream_idx].is_open() && !input_streams[stream_idx].eof()) {
             int next_key, next_value, next_tombstone_flag;
             if (input_streams[stream_idx] >> next_key >> next_value >> next_tombstone_flag) {
                 min_heap.push({{next_key, next_value, (next_tombstone_flag == 1)}, stream_idx});
             } else {
                 // End of this stream reached or read error
                 if (!input_streams[stream_idx].eof() && input_streams[stream_idx].fail()) {
                     std::cerr << "Warning: Read error or parsing issue mid-merge in file: " << runs_to_merge[stream_idx] << std::endl;
                 }
                 input_streams[stream_idx].close();
             }
        }
    }

    // Close output file
    outfile.close();
    if (!outfile) { // Check close status
        std::cerr << "Error closing merged output TXT file: " << output_filename << std::endl;
         std::remove(output_filename.c_str());
         return ""; // Indicate failure
    }

    // Close any remaining input streams
    for (auto& stream : input_streams) {
        if (stream.is_open()) {
            stream.close();
        }
    }

    // std::cout << "Merge complete. New TXT run: " << output_filename << std::endl;
    return output_filename; // Return the name of the newly created merged TXT file
}


// Function to check and trigger merges starting from a level
void lsm_tree::check_and_trigger_merge(int level_num) {
    if (level_num < 1 || level_num > MAX_LEVELS) {
        return; // Invalid level
    }

    level* current_level = levels_[level_num];

    // Check if the current level needs merging (tiering threshold reached)
    if (current_level->get_run_count() >= SIZE_RATIO) {
        // cout << "Level " << level_num << " reached threshold (" << current_level->get_run_count() << "/" << SIZE_RATIO << "). Triggering merge." << endl;

        // Prepare list of files to merge (all files in the current level)
        std::vector<std::string> files_to_merge = current_level->sstable_files_;

        // Perform the merge. The result goes into the *next* level.
        int target_level_num = level_num + 1;
        std::string merged_filename = merge_runs(target_level_num, files_to_merge);

        if (!merged_filename.empty()) {
             // Merge successful

             // 1. Clear the runs from the current level (they are now merged)
             current_level->sstable_files_.clear();

             // 2. Delete the physical files that were merged
             delete_sst_files(files_to_merge);

             // 3. Add the new merged run to the *next* level (if valid level)
             if (target_level_num <= MAX_LEVELS) {
                 levels_[target_level_num]->add_run(merged_filename);

                 // 4. Recursively check if the *next* level now needs merging
                 check_and_trigger_merge(target_level_num);
             } else {
                 // Merged into the max level. Decide what to do with the file.
                 // Option A: Keep it in the max level (simplest tiering)
                 // levels_[MAX_LEVELS]->add_run(merged_filename); // Add run back to max level if needed by policy
                 cout << "Merged run placed in MAX level (" << MAX_LEVELS << "): " << merged_filename << endl;
                 // Option B: If max level also has a size limit/policy, handle it here.
                 // Option C: Discard the file if it truly exceeds capacity (data loss!)
                 // For now, we assume the max level can hold multiple runs from merges.
                 // If the merge_runs function returned a filename for a level > MAX_LEVELS,
                 // we might just delete it, or log an error. Our merge_runs currently prevents this.
                  if(level_num == MAX_LEVELS) {
                       // If the merge was triggered *at* the max level, the target was MAX+1.
                       // merge_runs should have returned "" or handled it.
                       // If somehow we get here, it might mean the policy needs refinement
                       // for the last level. A common approach is the last level uses leveling.
                       // For simple tiering, we might just let the last level grow.
                       levels_[MAX_LEVELS]->add_run(merged_filename); // Add it back if needed
                    //    cout << "Warning: Merge occurred at MAX level. Result added back to MAX level." << endl;
                  }


             }

        } else {
            // cerr << "Error: Merge failed for level " << level_num << ". Files remain." << endl;
            // Decide on error handling - retry? Stop? Log?
        }
    }
}


// --- Public Interface Implementation ---

bool lsm_tree::insert(key_value kv_pair) {
    // Try inserting into memtable
    if (!memtable_ptr_->insert(kv_pair)) {
        // Memtable is full, need to flush it

        // cout << "Memtable full. Flushing to Level 1..." << endl;
        std::vector<key_value> data_to_flush = memtable_ptr_->flush();

        if (!data_to_flush.empty()) {
            // Generate a filename for the new run in Level 1
            std::string new_sstable_file = generate_sstable_filename(1);

            // Write the flushed data to the new SSTable file
            if (write_sstable(data_to_flush, new_sstable_file)) {
                // Add the new run (file) to Level 1
                levels_[1]->add_run(new_sstable_file);

                // Check if Level 1 needs merging now
                check_and_trigger_merge(1);
            } else {
                //  cerr << "Error: Failed to write flushed memtable to disk. Data potentially lost." << endl;
                 // Error handling: what to do? Retry? Stop? Log? Maybe try inserting again?
                 // For now, we proceed but the data from this flush is lost.
                 // Re-insert the current kv_pair might be needed if it wasn't the cause of the flush.
                 // We need to insert the original kv_pair *after* the flush attempt.
                 bool retry_insert = memtable_ptr_->insert(kv_pair); // Try inserting the triggering pair again
                 if(!retry_insert){
                    // cerr << "Critical Error: Cannot insert into empty memtable after flush failure." << endl;
                    return false; // Indicate failure
                 }
                 return true; // Insert succeeded after handling flush failure (partially)
            }
        } else {
            //  cout << "Memtable was full but flush returned no data??" << endl;
        }

        // After successful flush (or handled failure), try inserting the original pair again
        // (it wasn't added because insert returned false)
        if (!memtable_ptr_->insert(kv_pair)) {
            //  cerr << "Critical Error: Could not insert element into memtable even after flushing." << endl;
             return false; // Should not happen if flush worked
        }
    }
    return true; // Insert successful (either directly or after flush)
}


int lsm_tree::get(int key, bool called_from_range) {
    int value = -1;
    bool is_tombstone = false;
    bool found = false;

    // 1. Check Memtable (most recent data)
    if (memtable_ptr_->find_key(key, value, is_tombstone)) {
        found = true;
        if (is_tombstone) {
            if (!called_from_range) cout << endl;
            return -1; // Deleted
        }
        // Found in memtable and not deleted
    } else {
        // 2. Check Levels (SSTables on disk) from L1 to MAX_LEVELS
        for (int i = 1; i <= MAX_LEVELS; ++i) {
            if (levels_[i]->find_key(key, value, is_tombstone)) {
                 found = true;
                 if (is_tombstone) {
                    if (!called_from_range) cout << endl;
                    return -1; // Found tombstone in SSTable, stop searching
                 }
                 // Found valid entry in this level, stop searching lower levels
                 break;
            }
        }
    }

    // Output based on findings
    if (found && !is_tombstone) {
         if (!called_from_range) {
             cout << value << endl;
         } else {
             cout << key << ":" << value << " ";
         }
         return value;
    } else {
         // Not found or found as tombstone (handled earlier)
         if (!called_from_range) {
             cout << endl;
         }
         return -1;
    }
}


void lsm_tree::range(int start, int end) {
    cout << "Range (" << start << " to " << end << "): ";
    // bool first = true; // Remove this line
    for (int k = start; k <= end; ++k) { // Inclusive range? Adjust if needed.
        get(k, true); // Call get in range mode, discard return value (it already prints)
    }
    cout << endl;
}


void lsm_tree::delete_key(int key) {
    // Insert a tombstone entry for the key.
    // The insert logic will handle updates/memtable flushing.
    insert({key, 0, true}); // Value doesn't matter for tombstone
}


void lsm_tree::printStats() {
    std::cout << "--- LSM Tree Stats ---" << std::endl;

    // Data structures to hold intermediate results
    std::map<int, std::pair<int, std::string>> logical_data; // Map<key, Pair<value, location>>
    std::set<int> deleted_keys;                             // Keep track of keys confirmed deleted
    std::vector<long long> physical_key_counts(MAX_LEVELS + 1, 0); // Count all keys per level file

    key_value temp_kv; // Reusable buffer for reading from files

    // --- Stage 1: Process data from newest to oldest to find logical state ---

    // 1.a Process Memtable
    // std::cout << "Debug: Processing Memtable..." << std::endl; // Optional debug line
    for (int i = memtable_ptr_->curr_size_ - 1; i >= 0; --i) { // Iterate reverse for latest memtable entries first
        const auto& kv = memtable_ptr_->memtable_[i];

        // If key already processed (found newer version or deleted), skip
        if (logical_data.count(kv.key) || deleted_keys.count(kv.key)) {
            continue;
        }

        if (kv.tombstone) {
            deleted_keys.insert(kv.key); // Mark as deleted, don't add to logical_data
        } else {
            logical_data[kv.key] = {kv.value, "M"}; // Found latest version in Memtable
        }
    }

    // 1.b Process Levels (from L1 down to MAX_LEVELS)
    for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
        level* current_level = levels_[level_num];
        if (!current_level) continue; // Skip if level doesn't exist

        // Process runs within the level (newest run first - reverse iteration)
        for (auto it = current_level->sstable_files_.rbegin(); it != current_level->sstable_files_.rend(); ++it) {
            const std::string& filename = *it;
            // std::cout << "Debug:   Reading file " << filename << "..." << std::endl; // Optional debug line

            // Open in text mode
            std::ifstream infile(filename);
            if (!infile) {
                // std::cerr << "Warning: Could not open SSTable TXT file for stats: " << filename << std::endl;
                continue;
            }

            long long current_file_key_count = 0;
            int current_key, current_value, tombstone_flag;

            // Read using text extraction
            while (infile >> current_key >> current_value >> tombstone_flag) {
                current_file_key_count++;
                bool current_tombstone = (tombstone_flag == 1);

                // Check if key already has a newer version or is known to be deleted
                if (logical_data.count(current_key) || deleted_keys.count(current_key)) {
                    continue; // Skip older/deleted versions
                }

                // This is the newest version encountered so far for this key
                if (current_tombstone) {
                    deleted_keys.insert(current_key); // Mark as deleted
                } else {
                    logical_data[current_key] = {current_value, "L" + std::to_string(level_num)}; // Store value and location
                }
            }
            if (!infile.eof() && infile.fail()) {
                std::cerr << "Warning: Read error or parsing issue during stats in file: " << filename << std::endl;
            }

            physical_key_counts[level_num] += current_file_key_count;
            infile.close();
        }
    }

    // --- Stage 2: Print the statistics based on collected data ---

    // (1) Logical Pair Count
    // The size of logical_data map contains exactly the unique, non-deleted keys
    std::cout << "Logical Pairs: " << logical_data.size() << std::endl;

    // (2) Keys Per Level (Physical count including tombstones/stale data in files)
    std::cout << "LVL1: " << physical_key_counts[1];
    for (int i = 2; i <= MAX_LEVELS; ++i) {
        std::cout << ", LVL" << i << ": " << physical_key_counts[i];
    }
    std::cout << std::endl;

    // (3) Dump Tree (Logical view: Key:Value:Level)
    // Iterate through the sorted map (logical_data)
    // bool first_entry = true;
    std::map<int, std::vector<std::pair<int, int>>> entries_by_level; // Group for printing

     // Group by level first
    for(const auto& pair : logical_data) {
        int key = pair.first;
        int value = pair.second.first;
        std::string location = pair.second.second;
        int level_num = 0; // 0 for Memtable
        if(location != "M") {
             try {
                level_num = std::stoi(location.substr(1)); // Extract level number after "L"
             } catch(...) { /* Handle potential error if location format is wrong */ }
        }
        entries_by_level[level_num].push_back({key, value});
    }

    // Print Memtable entries first (Level 0)
    if(entries_by_level.count(0)) {
        for(const auto& kv_pair : entries_by_level[0]) {
             std::cout << kv_pair.first << ":" << kv_pair.second << ":M ";
        }
         std::cout << std::endl; // Newline after memtable entries
    }


    // Print Level entries (Level 1 to MAX_LEVELS)
     for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
         if (entries_by_level.count(level_num)) {
             for (const auto& kv_pair : entries_by_level[level_num]) {
                 std::cout << kv_pair.first << ":" << kv_pair.second << ":L" << level_num << " ";
             }
             std::cout << std::endl; // Newline after each level's entries
         }
     }


    std::cout << "----------------------" << std::endl;
}

// Explicit function to delete all SSTable files
void lsm_tree::cleanup_files() {
    std::cout << "Cleaning up ALL SSTable files and directories..." << std::endl;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
             // Delete files stored in memory first
            delete_sst_files(levels_[i]->sstable_files_);
            levels_[i]->sstable_files_.clear(); // Clear the list in memory

            // Optionally remove the directory itself
            std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
             // Careful with recursive delete! Basic remove dir:
             if (rmdir(level_dir.c_str()) != 0) {
                 if (errno != ENOTEMPTY) { // Ignore error if dir not empty (we just deleted files)
                      std::cerr << "Warning: Could not remove directory " << level_dir << ": " << strerror(errno) << std::endl;
                 } else {
                     // If you want to force remove non-empty dirs, you need a recursive function or system("rm -rf ...") (use with caution!)
                     std::cerr << "Info: Directory not empty, not removed: " << level_dir << std::endl;
                 }
             } else {
                 std::cout << "Removed directory: " << level_dir << std::endl;
             }
        }
    }
    // Optionally remove the root data directory
    if (rmdir(DATA_DIR.c_str()) != 0) {
         if (errno != ENOTEMPTY) {
             std::cerr << "Warning: Could not remove root data directory " << DATA_DIR << ": " << strerror(errno) << std::endl;
         } else {
              std::cerr << "Info: Root data directory not empty, not removed: " << DATA_DIR << std::endl;
         }
    } else {
        std::cout << "Removed directory: " << DATA_DIR << std::endl;
    }

     // Reset run ID generator
     next_run_id_ = 0;
}
