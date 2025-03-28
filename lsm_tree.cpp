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

using namespace std;

// --- Helper Struct for Merge ---
struct merge_entry {
    key_value kv;
    size_t stream_index; // Which input file this entry came from

    // Custom comparator for min-heap (priority queue) based on key
    bool operator>(const merge_entry& other) const {
        // If keys are equal, prefer the one from the lower stream index (arbitrary but consistent)
        // In a real system, you might prefer based on sequence number if available.
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
        ifstream infile(filename, ios::binary);
        if (!infile) {
            cerr << "Error: Could not open SSTable file for reading: " << filename << endl;
            continue; // Skip this file if it can't be opened
        }

        key_value current_kv;
        // Read file sequentially (can be optimized with index blocks/binary search later)
        while (infile.read(reinterpret_cast<char*>(&current_kv), sizeof(key_value))) {
            if (current_kv.key == key) {
                value = current_kv.value;
                is_tombstone = current_kv.tombstone;
                infile.close();
                return true; // Key found
            }
            // Optimization: If we pass the key in a sorted file, it won't be found later in this file
            if (current_kv.key > key) {
                 break; // Stop reading this file
            }
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
lsm_tree::lsm_tree() {
    memtable_ptr_ = new memtable();
    levels_.resize(MAX_LEVELS + 1, nullptr); // Initialize vector with nullptrs

    // Create levels
    int current_capacity = INITIAL_LEVEL_CAPACITY;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        levels_[i] = new level(current_capacity, i);
        if (i > 1) {
            levels_[i-1]->next_ = levels_[i];
        }
        // Capacity scaling isn't strictly enforced by tiering run count,
        // but can be used as a guideline or for future policies.
        current_capacity *= SIZE_RATIO; // Or another scaling factor
    }
}

lsm_tree::~lsm_tree() {
    delete memtable_ptr_;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        delete levels_[i];
    }
    // Consider adding cleanup_files() here if desired on program exit
    // cleanup_files();
}

// Helper to generate unique SSTable filenames
std::string lsm_tree::generate_sstable_filename(int level_num) {
    return SST_FILE_PREFIX + std::to_string(level_num) + "_run_" + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

// Helper to write sorted data to an SSTable file
bool lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename) {
    // Open file in binary mode for writing
    ofstream outfile(filename, ios::binary | ios::trunc); // Trunc ensures it's a new file or overwrites
    if (!outfile) {
        cerr << "Error: Could not open SSTable file for writing: " << filename << endl;
        return false;
    }

    // Write data block by block (or element by element for simplicity here)
    for (const auto& kv : data) {
        outfile.write(reinterpret_cast<const char*>(&kv), sizeof(key_value));
        if (!outfile) {
             cerr << "Error: Failed to write to SSTable file: " << filename << endl;
             outfile.close();
             return false;
        }
    }

    outfile.close();
    if (!outfile) { // Check close status
         cerr << "Error: Failed to close SSTable file properly: " << filename << endl;
         return false; // File might be corrupted
    }
    cout << "Successfully wrote SSTable: " << filename << endl;
    return true;
}

// Helper to delete SSTable files
void lsm_tree::delete_sst_files(const std::vector<std::string>& filenames) {
    for (const auto& filename : filenames) {
        if (std::remove(filename.c_str()) != 0) {
            cerr << "Warning: Could not delete SSTable file: " << filename << endl;
        } else {
             cout << "Deleted old SSTable: " << filename << endl;
        }
    }
}

// --- Merge Logic ---
// Performs a k-way merge on the given run files, writes result to a new file, returns new filename.
std::string lsm_tree::merge_runs(int target_level_num, const std::vector<std::string>& runs_to_merge) {
    if (runs_to_merge.empty()) {
        return ""; // Should not happen in tiering merge typically
    }
    if (target_level_num > MAX_LEVELS) {
        cerr << "Error: Cannot merge into level " << target_level_num << " (max level is " << MAX_LEVELS << ")" << endl;
        // Handle this error - maybe discard data, maybe log and stop?
        // For now, just return empty, indicating failure.
        return "";
    }


    cout << "Merging " << runs_to_merge.size() << " runs into level " << target_level_num << "..." << endl;

    std::vector<ifstream> input_streams;
    input_streams.reserve(runs_to_merge.size());

    // Min-heap to manage the next available element from each run
    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input files and read the first element from each
    for (size_t i = 0; i < runs_to_merge.size(); ++i) {
        input_streams.emplace_back(runs_to_merge[i], ios::binary);
        if (!input_streams.back()) {
            cerr << "Error: Could not open file for merge: " << runs_to_merge[i] << endl;
            // Cleanup: Close already opened streams
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
            return ""; // Indicate merge failure
        }

        key_value kv;
        if (input_streams.back().read(reinterpret_cast<char*>(&kv), sizeof(key_value))) {
            min_heap.push({kv, i});
        } else {
            // File might be empty, just close it. It won't participate further.
             input_streams.back().close();
        }
    }

    // Generate filename for the new merged run in the *target* level
    std::string output_filename = generate_sstable_filename(target_level_num);
    ofstream outfile(output_filename, ios::binary | ios::trunc);
    if (!outfile) {
        cerr << "Error: Could not open output file for merge: " << output_filename << endl;
        // Cleanup: Close input streams
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return ""; // Indicate merge failure
    }

    key_value last_written_kv;
    bool first_write = true;

    // Merge process
    while (!min_heap.empty()) {
        merge_entry smallest = min_heap.top();
        min_heap.pop();

        // --- Compaction/Duplicate Handling ---
        // Only write the key if it's different from the last written key.
        // If keys are the same, the one processed first (from heap) is considered 'newer'
        // due to how levels/runs are typically ordered or based on tie-breaking.
        // Also, propagate tombstones unless a newer value for the same key appears.
        if (first_write || smallest.kv.key != last_written_kv.key) {
             // Only write non-tombstones from the *final* level merge if needed (optional optimization)
             // For now, write everything to maintain history correctly for lookups
            outfile.write(reinterpret_cast<const char*>(&smallest.kv), sizeof(key_value));
            last_written_kv = smallest.kv;
            first_write = false;
        } else {
             // Duplicate key. The one already processed (last_written_kv) takes precedence.
             // If the new one (smallest.kv) is a tombstone and the last written wasn't,
             // the tombstone should ideally win if it's logically 'newer'.
             // Our simple heap doesn't track time, but level order gives some precedence.
             // If last_written was not a tombstone, but smallest.kv IS a tombstone for same key:
             if (!last_written_kv.tombstone && smallest.kv.tombstone) {
                 // Overwrite the previous entry requires seeking back, which is complex.
                 // Standard approach: Just write the tombstone. Lookups find the latest version.
                 // Seek back and overwrite (more complex, less common):
                 // outfile.seekp(-static_cast<std::streamoff>(sizeof(key_value)), ios::cur);
                 // outfile.write(reinterpret_cast<const char*>(&smallest.kv), sizeof(key_value));
                 // last_written_kv = smallest.kv; // Update last written

                 // Simpler: write the tombstone anyway. The latest read wins during GET.
                 // But to avoid redundant entries, we just *update* last_written_kv
                 // and rely on the *next* distinct key write.
                 last_written_kv = smallest.kv; // This tombstone now shadows the previous value
             }
             // Else (newest is not tombstone, or both are tombstones), keep last_written_kv.
        }


        // Read the next element from the same stream the smallest element came from
        size_t stream_idx = smallest.stream_index;
        if (input_streams[stream_idx].is_open() && !input_streams[stream_idx].eof()) {
             key_value next_kv;
             if (input_streams[stream_idx].read(reinterpret_cast<char*>(&next_kv), sizeof(key_value))) {
                 min_heap.push({next_kv, stream_idx});
             } else {
                 // End of this stream reached or read error
                 input_streams[stream_idx].close();
             }
        }
    }

    // Close output file
    outfile.close();
    if (!outfile) { // Check close status
        cerr << "Error closing merged output file: " << output_filename << endl;
         // Attempt to delete potentially corrupted output file
         std::remove(output_filename.c_str());
         return ""; // Indicate failure
    }

    // Close any remaining input streams (should be closed already if read fully)
    for (auto& stream : input_streams) {
        if (stream.is_open()) {
            stream.close();
        }
    }

    cout << "Merge complete. New run: " << output_filename << endl;
    return output_filename; // Return the name of the newly created merged file
}


// Function to check and trigger merges starting from a level
void lsm_tree::check_and_trigger_merge(int level_num) {
    if (level_num < 1 || level_num > MAX_LEVELS) {
        return; // Invalid level
    }

    level* current_level = levels_[level_num];

    // Check if the current level needs merging (tiering threshold reached)
    if (current_level->get_run_count() >= SIZE_RATIO) {
        cout << "Level " << level_num << " reached threshold (" << current_level->get_run_count() << "/" << SIZE_RATIO << "). Triggering merge." << endl;

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
                       cout << "Warning: Merge occurred at MAX level. Result added back to MAX level." << endl;
                  }


             }

        } else {
            cerr << "Error: Merge failed for level " << level_num << ". Files remain." << endl;
            // Decide on error handling - retry? Stop? Log?
        }
    }
}


// --- Public Interface Implementation ---

bool lsm_tree::insert(key_value kv_pair) {
    // Try inserting into memtable
    if (!memtable_ptr_->insert(kv_pair)) {
        // Memtable is full, need to flush it

        cout << "Memtable full. Flushing to Level 1..." << endl;
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
                 cerr << "Error: Failed to write flushed memtable to disk. Data potentially lost." << endl;
                 // Error handling: what to do? Retry? Stop? Log? Maybe try inserting again?
                 // For now, we proceed but the data from this flush is lost.
                 // Re-insert the current kv_pair might be needed if it wasn't the cause of the flush.
                 // We need to insert the original kv_pair *after* the flush attempt.
                 bool retry_insert = memtable_ptr_->insert(kv_pair); // Try inserting the triggering pair again
                 if(!retry_insert){
                    cerr << "Critical Error: Cannot insert into empty memtable after flush failure." << endl;
                    return false; // Indicate failure
                 }
                 return true; // Insert succeeded after handling flush failure (partially)
            }
        } else {
             cout << "Memtable was full but flush returned no data??" << endl;
        }

        // After successful flush (or handled failure), try inserting the original pair again
        // (it wasn't added because insert returned false)
        if (!memtable_ptr_->insert(kv_pair)) {
             cerr << "Critical Error: Could not insert element into memtable even after flushing." << endl;
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
        // std::cout << "Debug: Processing Level " << level_num << "..." << std::endl; // Optional debug line
        for (auto it = current_level->sstable_files_.rbegin(); it != current_level->sstable_files_.rend(); ++it) {
            const std::string& filename = *it;
            // std::cout << "Debug:   Reading file " << filename << "..." << std::endl; // Optional debug line

            std::ifstream infile(filename, std::ios::binary);
            if (!infile) {
                std::cerr << "Warning: Could not open SSTable file for stats: " << filename << std::endl;
                continue;
            }

            // Count physical keys while reading for logical state
            long long current_file_key_count = 0;
            while (infile.read(reinterpret_cast<char*>(&temp_kv), sizeof(key_value))) {
                current_file_key_count++;

                // Check if key already has a newer version or is known to be deleted
                if (logical_data.count(temp_kv.key) || deleted_keys.count(temp_kv.key)) {
                    continue; // Skip older/deleted versions
                }

                // This is the newest version encountered so far for this key
                if (temp_kv.tombstone) {
                    deleted_keys.insert(temp_kv.key); // Mark as deleted
                } else {
                    logical_data[temp_kv.key] = {temp_kv.value, "L" + std::to_string(level_num)}; // Store value and location
                }
            }
            physical_key_counts[level_num] += current_file_key_count; // Add file's count to level total
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
    cout << "Cleaning up SSTable files..." << endl;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
            delete_sst_files(levels_[i]->sstable_files_);
            levels_[i]->sstable_files_.clear(); // Clear the list in memory too
        }
    }
     // Also reset the run ID generator if starting fresh
     next_run_id_ = 0;
}