#include "lsm_tree.hh"
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <fstream>
#include <cstdio>   // For remove
#include <queue>    // For priority_queue
#include <limits>   // For numeric_limits
#include <map>
#include <set>
#include <sys/stat.h> // For stat, mkdir
#include <sys/types.h> // For stat, mkdir
#include <unistd.h> // For rmdir (on POSIX systems)
#include <dirent.h> // For directory listing
#include <cerrno>   // For errno
#include <cstring>  // For strerror
#include <stdexcept> // For runtime_error
#include <sstream> // For stringstream
#include <cmath> // For std::abs (though streamoff difference is usually positive)


using namespace std;

// --- Define data directory constant ---
const std::string DATA_DIR = "data";

// --- Helper Functions ---
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

// Helper function to rebuild fence pointers from an existing text file
std::vector<std::pair<int, long long>> lsm_tree::rebuild_fence_pointers(const std::string& filename) {
    std::vector<std::pair<int, long long>> fence_pointers;
    std::ifstream infile(filename);
    if (!infile) {
        std::cerr << "Warning: Could not open SSTable TXT file to rebuild fence pointers: " << filename << std::endl;
        return fence_pointers; // Return empty vector
    }

    std::string line;
    long long current_block_byte_count = 0; // Bytes accumulated *within* the current potential block

    while (true) {
        long long line_start_offset = infile.tellg(); // Offset *before* reading the line

        if (line_start_offset == -1) {
             std::cerr << "Warning: tellg() failed during fence pointer rebuild in " << filename << std::endl;
             break; // Fatal stream error
        }

        std::string current_line;
        if (!std::getline(infile, current_line)) {
            // getline failed - either EOF or read error
            if (!infile.eof() && infile.fail()) {
                 std::cerr << "Warning: Read error during fence pointer rebuild in " << filename << std::endl;
            }
            break; // Exit the loop on EOF or read error
        }

        // Successfully read a line. Calculate its byte length including newline.
        long long line_end_offset = infile.tellg(); // Offset *after* reading the line and newline
        long long line_byte_size = line_end_offset - line_start_offset;

        if (current_line.empty()) {
            // Empty lines contribute to byte count but don't have a key for a fence pointer
            current_block_byte_count += line_byte_size;
            continue;
        }

        // Parse the key from the non-empty line
        std::stringstream ss(current_line);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
             // This is a valid key-value line

             // Logic for adding a fence pointer:
             // Add a fence pointer if this is the first entry in the file OR
             // if adding the previous entry's size caused the block threshold to be crossed,
             // meaning this *current* entry is the start of a new block.
             // The offset for the fence pointer is the start offset of this line (`line_start_offset`).
             // The key for the fence pointer is the key on this line (`current_key`).
             // The condition `current_block_byte_count >= BLOCK_SIZE` checks if the *previous* entries filled the block.
             // The condition `fence_pointers.empty()` handles the first entry of the file.

             if (current_block_byte_count >= BLOCK_SIZE || fence_pointers.empty()) {
                 // Add the fence pointer for the *start* of this new block.
                 // The key is `current_key`, the offset is where this line started (`line_start_offset`).
                 fence_pointers.push_back({current_key, line_start_offset});

                 // Reset block tracking for the new block starting here
                 current_block_byte_count = line_byte_size; // The size of the *current* line is the first contribution to the new block's count
             } else {
                 // This entry is within the current block, just add its size to the block count
                 current_block_byte_count += line_byte_size;
             }
        } else {
             // Failed to parse a non-empty line. Treat it like an empty line for byte counting, but warn.
             std::cerr << "Warning: Failed to parse line during fence pointer rebuild: '" << current_line << "' in file: " << filename << std::endl;
             current_block_byte_count += line_byte_size; // Still account for its bytes within the current block
        }
    } // End while(true) loop

    infile.close();
    // std::cout << "Rebuilt " << fence_pointers.size() << " fence pointers for " << filename << std::endl;
    return fence_pointers;
}


// --- Helper Struct for Merge ---
struct merge_entry {
    key_value kv;
    size_t stream_index; // Which input file this entry came from
    // Note: We don't need offset here, the ifstream handles it internally

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
    // sstable_runs_ is already default-initialized (empty vector)
}

level::~level() {
    // Destructor itself doesn't delete files; lsm_tree destructor or cleanup does this explicitly.
    // We just need to ensure memory managed by level is released (like sstable_runs_ vector).
    // The SSTableInfo structs within the vector are automatically destructed.
}

void level::add_run(const SSTableInfo& info) {
    sstable_runs_.push_back(info);
    // In tiering, order might not strictly matter, but for lookups newest first is better.
    // The find_key logic searches runs within a level newest-first (reverse iterator).
}

void level::add_run(const std::string& filename) {
    // This version should ideally not be used after implementing rebuild_fence_pointers on load.
    // It's kept for compatibility but will result in a run with no fence pointers.
     std::cerr << "Warning: Calling add_run(string) - file added without fence pointers." << std::endl;
     sstable_runs_.push_back({filename, {}}); // Add with empty fence pointers initially
}


// Search key in this level's SSTables (files) using fence pointers
bool level::find_key(int key, int& value, bool& is_tombstone) {
    // Search runs in reverse order (newest first)
    for (auto it = sstable_runs_.rbegin(); it != sstable_runs_.rend(); ++it) {
        const SSTableInfo& run_info = *it;
        const std::string& filename = run_info.filename;
        const auto& fence_pointers = run_info.fence_pointers;

        // --- Use Fence Pointers to find potential block ---
        long long search_offset = 0; // Default to start of file
        bool used_fence_pointer = false;

        if (!fence_pointers.empty()) {
             // Find the first fence pointer whose key is >= target key
            auto fp_it = std::lower_bound(fence_pointers.begin(), fence_pointers.end(), key,
                                         [](const std::pair<int, long long>& fp, int target_key){
                                             return fp.first < target_key;
                                         });

            if (fp_it != fence_pointers.begin()) {
                // The key might be in the block starting at the offset of the *previous* fence pointer
                --fp_it;
                search_offset = fp_it->second;
                used_fence_pointer = true;
                // std::cout << "Debug: Found relevant block starting at offset " << search_offset << " in " << filename << " using fence pointer key " << fp_it->first << std::endl;
            } else if (key < fence_pointers.front().first) {
                 // Key is smaller than the first fence pointer's key - means it should be before the first fence pointer
                 // search_offset remains 0. (This case is covered by fp_it == fence_pointers.begin() and not going back)
                 // std::cout << "Debug: Key smaller than first fence pointer, searching from start in " << filename << std::endl;
                 used_fence_pointer = true; // We used the knowledge from the first fence pointer
                 search_offset = 0; // Explicitly set to 0
            } else { // fp_it == fence_pointers.end()
                // Key is greater than or equal to the last fence pointer's key. Search starts at the last fence pointer's offset.
                search_offset = fence_pointers.back().second;
                used_fence_pointer = true;
                // std::cout << "Debug: Key greater than or equal to last fence pointer, searching from offset " << search_offset << " in " << filename << std::endl;
            }
        } else {
             // No fence pointers, scan the whole file from the beginning (search_offset is 0)
             // std::cout << "Debug: No fence pointers, scanning entire file " << filename << std::endl;
             search_offset = 0;
        }
        // --- End Fence Pointer Logic ---


        // Open the file and seek to the calculated offset
        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Warning: Could not open SSTable TXT file for reading: " << filename << std::endl;
            continue; // Skip this file
        }

        // Seek to the calculated starting position
        infile.seekg(search_offset);
        if (infile.fail()) {
            std::cerr << "Warning: Failed to seek to offset " << search_offset << " in file: " << filename << std::endl;
            infile.close();
            continue; // Skip this file
        }


        std::string line;
        int current_key;
        int current_value;
        int tombstone_flag;

        // Read line by line from the seeked position
        while (std::getline(infile, line)) {
            if (line.empty()) continue; // Skip empty lines

            std::stringstream ss(line);

            // Read from the stringstream
            if (ss >> current_key >> current_value >> tombstone_flag) {
                if (current_key == key) {
                    value = current_value;
                    is_tombstone = (tombstone_flag == 1);
                    infile.close(); // Found the key
                    return true;
                }
                // Optimization: Since the file is sorted, if we pass the key, it's not in this file
                // We only need this check if we used fence pointers. If no fence pointers, we scan the whole file.
                // If we used fence pointers, the target key should be >= the starting block's key.
                // So if current_key > key, we've gone past it.
                 if (used_fence_pointer && current_key > key) {
                     break; // Key not found in this block/file
                 }
                 // If not using fence pointers (whole file scan), we still break if key is passed
                 if (!used_fence_pointer && current_key > key) {
                      break; // Key not found in this file
                 }


            } else {
                 // Handle parsing error on a line
                 std::cerr << "Warning: Parsing error during find_key in file: " << filename << ", line: " << line << std::endl;
                 // Decide how to handle: skip line, or assume file corrupted and break?
                 // Skipping seems more resilient for stats/gets.
            }
        }

        // Check for read errors that didn't result in EOF or parsing failure within the loop
        if (!infile.eof() && infile.fail()) {
             std::cerr << "Warning: Read error or parsing issue near EOF in SSTable TXT file: " << filename << std::endl;
        }

        infile.close(); // Close the file stream before checking the next run
    }

    return false; // Key not found in any run of this level
}

std::vector<std::string> level::get_run_filenames() const {
    std::vector<std::string> filenames;
    filenames.reserve(sstable_runs_.size());
    for(const auto& info : sstable_runs_) {
        filenames.push_back(info.filename);
    }
    return filenames;
}

void level::clear_runs() {
    sstable_runs_.clear();
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
            return true; // Key updated
        }
    }

    // If key not found and memtable is full, signal to flush (caller handles flush)
    if (is_full()) {
       return false; // Indicate memtable is full
    }

    // Add new entry
    memtable_.push_back(kv_pair);
    ++curr_size_;
    return true; // New key inserted
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
    // Search in reverse for newest value
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
    levels_.resize(MAX_LEVELS + 1, nullptr); // levels_[0] unused

    // 1. Create root data directory
    if (!create_directory(DATA_DIR)) {
        throw std::runtime_error("Failed to create or access data directory: " + DATA_DIR);
    }

    long long max_run_id_found = -1;

    // 2. Create levels and load existing SSTables, rebuilding fence pointers
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

        // --- Load existing files for this level and rebuild fence pointers ---
        DIR *dirp = opendir(level_dir.c_str());
        if (dirp) {
            struct dirent *dp;
            while ((dp = readdir(dirp)) != nullptr) {
                std::string filename = dp->d_name;
                std::string full_path = level_dir + "/" + filename;

                // Check if it's a regular file and potentially an SST file
                struct stat file_stat;
                // Need full path for stat/fstatat depending on OS/API
                if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode))
                {
                     if (filename.length() > SST_FILE_SUFFIX.length() &&
                         filename.substr(filename.length() - SST_FILE_SUFFIX.length()) == SST_FILE_SUFFIX &&
                         filename.rfind(SST_FILE_PREFIX) != std::string::npos)
                    {
                        // std::cout << "Found existing SSTable: " << full_path << std::endl;

                        // Rebuild fence pointers for the loaded file
                        std::vector<std::pair<int, long long>> fps = rebuild_fence_pointers(full_path);

                        // Add SSTableInfo to the level
                        levels_[i]->add_run({full_path, fps});

                        // Parse run ID from filename (e.g., "run_123.txt")
                        size_t run_pos = filename.rfind(SST_FILE_PREFIX);
                        size_t sst_pos = filename.rfind(SST_FILE_SUFFIX);
                        if (run_pos != std::string::npos && sst_pos != std::string::npos && run_pos < sst_pos) {
                             try {
                                 long long run_id = std::stoll(filename.substr(run_pos + SST_FILE_PREFIX.length(), sst_pos - (run_pos + SST_FILE_PREFIX.length())));
                                 if (run_id > max_run_id_found) {
                                     max_run_id_found = run_id;
                                 }
                             } catch (...) {
                                  std::cerr << "Warning: Could not parse run ID from filename: " << filename << std::endl;
                             }
                        }
                    }
                }
            }
            closedir(dirp);
        } else {
             std::cerr << "Warning: Could not open level directory for reading: " << level_dir << std::endl;
        }
        // Note: We assume existing files are added to levels_[i]->sstable_runs_ in arbitrary order.
        // The find_key logic searches runs within a level newest-first (reverse iterator).
        // If the filename structure doesn't guarantee this order, or if run ID order matters,
        // levels_[i]->sstable_runs_ would need sorting after loading. Sorting by parsed run_id is one way.

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

            // Write flushed data to disk and get its info (including fence pointers)
            SSTableInfo final_run_info = write_sstable(data_to_flush, final_sstable_file);

            if (!final_run_info.filename.empty()) { // write_sstable returns empty filename on failure
                 // Add run to Level 1's list (in memory, but won't persist unless saved)
                 // This write operation itself is the persistence step.
                 // The in-memory list update doesn't matter much here as the object is being destroyed,
                 // but we'll add it for consistency if cleanup_files were called after destruction.
                 if (levels_.size() > 1 && levels_[1]) {
                      levels_[1]->add_run(final_run_info);
                 }
                //  std::cout << "Final flush successful to: " << final_run_info.filename << std::endl;
                 // No compactions triggered during destruction flush.
            } else {
                 std::cerr << "Error: Failed to write final memtable flush to disk during shutdown!" << std::endl;
            }
        }
    }
    // --- End Shutdown Flush Logic ---


    delete memtable_ptr_;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) { // Check if the pointer is valid before deleting
             // Note: The level destructor doesn't delete files. That's done by cleanup_files or delete_sst_files explicitly.
             delete levels_[i];
        }
    }
     // The data directories remain unless cleanup_files is called explicitly.
}

// Helper to generate unique SSTable filenames
std::string lsm_tree::generate_sstable_filename(int level_num) {
    // Construct path: DATA_DIR / L<level_num> / run_<id>.txt
    std::string level_dir = DATA_DIR + "/L" + std::to_string(level_num);
    return level_dir + "/" + SST_FILE_PREFIX + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

// Helper to write sorted data to an SSTable file, returning SSTableInfo
SSTableInfo lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename) {
    SSTableInfo run_info;
    run_info.filename = filename;

    // Open file in text mode for writing
    // ios::trunc ensures it's a new file or overwrites
    std::ofstream outfile(filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open SSTable TXT file for writing: " << filename << std::endl;
        return {"", {}}; // Indicate failure by returning empty filename
    }

    long long current_block_byte_count = 0; // Bytes accumulated *within* the current potential block

    // Write each key-value pair as a line of text
    for (size_t i = 0; i < data.size(); ++i) {
        const auto& kv = data[i];

        // Record offset *before* writing the entry for the fence pointer/byte count
        long long entry_start_offset = outfile.tellp();
         if (entry_start_offset == -1) {
             std::cerr << "Warning: tellp() failed before writing entry to " << filename << ". Fence pointers might be inaccurate." << std::endl;
             // Decide how to handle - maybe continue but fence pointers will be unreliable
             // Or return error? For now, log and continue.
         }


        // Write the data line
        outfile << kv.key << " " << kv.value << " " << (kv.tombstone ? 1 : 0) << "\n";

        if (!outfile) { // Check stream state after write
             std::cerr << "Error: Failed to write to SSTable TXT file: " << filename << std::endl;
             outfile.close();
             std::remove(filename.c_str()); // Clean up partial file
             return {"", {}}; // Indicate failure
        }

        // Calculate bytes written for this line (approximate for text)
        // tellp() after writing is the position *after* the newline
        long long entry_end_offset = outfile.tellp();
        long long line_byte_size = 0;
        if (entry_end_offset != -1 && entry_start_offset != -1) { // Check if tellp was successful before and after
             line_byte_size = entry_end_offset - entry_start_offset;
        } else if (entry_end_offset == -1) {
             std::cerr << "Warning: tellp() failed after writing entry to " << filename << ". Cannot calculate line size." << std::endl;
             // We cannot accurately track bytes if tellp fails. Fence pointers will be wrong.
             // A robust system would fail here or switch to a different tracking method.
             // For this exercise, we'll log and add 0, making fp generation potentially incorrect.
             line_byte_size = 0; // Cannot get accurate size
        }


        // Add fence pointer if block size reached or it's the very first entry
        // Check against BLOCK_SIZE *before* adding the current line's size
        if (current_block_byte_count >= BLOCK_SIZE || run_info.fence_pointers.empty()) {
             // Add the fence pointer for the *start* of this new block.
             // The key is the current entry's key (`kv.key`).
             // The offset is where the current line started (`entry_start_offset` from the tellp before writing).
             if (entry_start_offset != -1) { // Only add if we got a valid start offset
                 run_info.fence_pointers.push_back({kv.key, entry_start_offset});
             } else {
                 std::cerr << "Warning: Skipped adding fence pointer due to failed tellp()." << std::endl;
                 // If we skipped adding a fence pointer, the next block starts conceptually after the *previous* successful point
                 // or needs a different offset calculation logic. Resetting to 0 or similar isn't right.
                 // Let's rely on the first valid tellp() as the start of the file if fence_pointers.empty() was true.
                 // If fence_pointers were not empty but tellp failed, we are in a bad state for tracking.
             }
             // Reset/Initialize byte count for the *new* block with the size of the current line
             current_block_byte_count = line_byte_size; // Start count for the new block
        } else {
            // This entry is within the current block, just add its size to the block count
            current_block_byte_count += line_byte_size;
        }
    }

    // Ensure at least one fence pointer if data was written but BLOCK_SIZE was never reached
    // The `run_info.fence_pointers.empty()` check inside the loop handles the very first entry.
    // If data was empty, file is empty, fence_pointers is empty, which is correct.

    outfile.close();
    if (!outfile) { // Check close status (important!)
        std::cerr << "Error: Failed to close SSTable TXT file properly: " << filename << std::endl;
         // File might be corrupted or incomplete even if writes seemed okay.
         // Decide if you should delete it or leave it. Leaving might allow partial recovery.
         return {"", {}}; // Indicate failure
    }
    // std::cout << "Successfully wrote SSTable TXT: " << filename << " with " << run_info.fence_pointers.size() << " fence pointers." << std::endl;
    return run_info;
}

// Helper to delete SSTable files (takes full paths)
void lsm_tree::delete_sst_files(const std::vector<std::string>& filenames) {
    for (const auto& filename : filenames) {
        if (std::remove(filename.c_str()) != 0) {
            // Use perror or strerror(errno) for better error reporting
            std::cerr << "Warning: Could not delete SSTable file: " << filename << " (" << strerror(errno) << ")" << std::endl;
        } else {
             // std::cout << "Deleted old SSTable: " << filename << std::endl;
        }
    }
}
// --- Merge Logic ---
// Performs a k-way merge on the given run TXT files, writes result to a new TXT file, returning SSTableInfo.
SSTableInfo lsm_tree::merge_runs(int target_level_num, const std::vector<SSTableInfo>& runs_to_merge_info) {

    if (runs_to_merge_info.empty()) {
        return {"", {}}; // Nothing to merge
    }

    // std::cout << "Merging " << runs_to_merge_info.size() << " TXT runs into level " << target_level_num << "..." << std::endl;

    std::vector<std::ifstream> input_streams;
    input_streams.reserve(runs_to_merge_info.size());

    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input TEXT files and read the first entry from each
    for (size_t i = 0; i < runs_to_merge_info.size(); ++i) {
        const std::string& filename = runs_to_merge_info[i].filename;
        input_streams.emplace_back(filename); // Open in text mode (default)

        if (!input_streams.back()) {
            std::cerr << "Error: Could not open TXT file for merge: " << filename << std::endl;
            // Clean up already opened streams before returning
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
            return {"", {}}; // Indicate merge failure
        }

        std::string line;
        // Read the first line from the stream
        if (std::getline(input_streams.back(), line)) {
             if (!line.empty()) {
                 std::stringstream ss(line);
                 int current_key, current_value, tombstone_flag;
                 if (ss >> current_key >> current_value >> tombstone_flag) {
                    min_heap.push({{current_key, current_value, (tombstone_flag == 1)}, i}); // Construct key_value and push
                 } else {
                      std::cerr << "Warning: Failed to parse first line in merge file: " << filename << ", line: " << line << std::endl;
                 }
             }
        } else {
            // File might be empty or failed initial read
             if (!input_streams.back().eof() && input_streams.back().fail()) {
                 std::cerr << "Warning: Failed initial read from TXT file: " << filename << std::endl;
             }
             // Do not close stream here, it will be closed later in the loop or cleanup
        }
    }

    // Generate filename for the new merged run (will have .txt suffix)
    std::string output_filename = generate_sstable_filename(target_level_num);
    // Open output in text mode
    std::ofstream outfile(output_filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open output TXT file for merge: " << output_filename << std::endl;
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return {"", {}}; // Indicate merge failure
    }

    SSTableInfo merged_run_info;
    merged_run_info.filename = output_filename;

    key_value last_written_kv; // Keep track of the last key written to handle duplicates
    bool first_write = true;

    long long current_block_byte_count = 0; // Bytes accumulated *within* the current potential block

    // Merge process
    while (!min_heap.empty()) {
        merge_entry smallest = min_heap.top();
        min_heap.pop();

        // --- Compaction/Duplicate Handling ---
        // Only write if it's the first entry or a different key than the last one written
        if (first_write || smallest.kv.key != last_written_kv.key) {
            // --- Write using TEXT format ---
            // Record offset *before* writing the entry for the fence pointer/byte count
            long long entry_start_offset = outfile.tellp();
             if (entry_start_offset == -1) {
                 std::cerr << "Warning: tellp() failed before writing entry to merged file " << output_filename << ". Fence pointers might be inaccurate." << std::endl;
             }

            outfile << smallest.kv.key << " " << smallest.kv.value << " " << (smallest.kv.tombstone ? 1 : 0) << "\n";
            if (!outfile) {
                std::cerr << "Error writing during merge to TXT file: " << output_filename << std::endl;
                // Cleanup needed
                 outfile.close();
                 for(auto& stream : input_streams) if(stream.is_open()) stream.close();
                 std::remove(output_filename.c_str()); // Attempt to remove bad output file
                 return {"", {}}; // Indicate failure
            }
            // --- End Text Write ---

            // Calculate bytes written for this line and add fence pointer
            long long entry_end_offset = outfile.tellp();
            long long line_byte_size = 0;
            if (entry_end_offset != -1 && entry_start_offset != -1) {
                 line_byte_size = entry_end_offset - entry_start_offset;
            } else if (entry_end_offset == -1) {
                 std::cerr << "Warning: tellp() failed after writing entry to merged file " << output_filename << ". Cannot calculate line size." << std::endl;
                 line_byte_size = 0;
            }


            // Add fence pointer if block size reached or it's the very first entry
            if (current_block_byte_count >= BLOCK_SIZE || merged_run_info.fence_pointers.empty()) {
                 if (entry_start_offset != -1) {
                    merged_run_info.fence_pointers.push_back({smallest.kv.key, entry_start_offset});
                 } else {
                     std::cerr << "Warning: Skipped adding fence pointer during merge due to failed tellp()." << std::endl;
                 }
                 current_block_byte_count = line_byte_size; // Start count for the new block
            } else {
                current_block_byte_count += line_byte_size;
            }

            last_written_kv = smallest.kv; // Update last written key
            first_write = false;

        }
        // Else: This entry is a duplicate key and an older version, discard it.

        // Read the next element (line) from the same stream this entry came from
        size_t stream_idx = smallest.stream_index;
        std::string next_line;
        if (input_streams[stream_idx].is_open() && !input_streams[stream_idx].eof()) {
             if (std::getline(input_streams[stream_idx], next_line)) {
                 if (!next_line.empty()) {
                     std::stringstream ss(next_line);
                     int next_key, next_value, next_tombstone_flag;
                     if (ss >> next_key >> next_value >> next_tombstone_flag) {
                         min_heap.push({{next_key, next_value, (next_tombstone_flag == 1)}, stream_idx});
                     } else {
                          std::cerr << "Warning: Failed to parse line after successful read in merge file: " << runs_to_merge_info[stream_idx].filename << ", line: " << next_line << std::endl;
                     }
                 } else {
                     // Read an empty line, just continue the loop. getline will try to read the *next* line next time.
                 }
             } else {
                 // End of this stream reached or read error
                 if (!input_streams[stream_idx].eof() && input_streams[stream_idx].fail()) {
                     std::cerr << "Warning: Read error or parsing issue mid-merge in file: " << runs_to_merge_info[stream_idx].filename << std::endl;
                 }
                 input_streams[stream_idx].close(); // Close the stream once it's exhausted
             }
        }
    }

    // Close output file
    outfile.close();
    if (!outfile) { // Check close status
        std::cerr << "Error closing merged output TXT file: " << output_filename << std::endl;
         std::remove(output_filename.c_str());
         return {"", {}}; // Indicate failure
    }

    // Close any remaining input streams
    for (auto& stream : input_streams) {
        if (stream.is_open()) {
            stream.close();
        }
    }

    // std::cout << "Merge complete. New TXT run: " << output_filename << " with " << merged_run_info.fence_pointers.size() << " fence pointers." << std::endl;
    return merged_run_info; // Return the info for the newly created merged TXT file
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

        // Prepare list of files to merge (all files in the current level) - now SSTableInfo objects
        std::vector<SSTableInfo> runs_to_merge_info = current_level->sstable_runs_;
        std::vector<std::string> files_to_delete = current_level->get_run_filenames(); // Get filenames for deletion

        // Perform the merge. The result goes into the *next* level.
        int target_level_num = level_num + 1;
        // If target_level_num exceeds MAX_LEVELS, merge_runs should return empty filename
        // or handle writing to a specific max level location/policy.
        // For this simple tiering, level MAX_LEVELS just accumulates.
        if (target_level_num > MAX_LEVELS) target_level_num = MAX_LEVELS;


        // Pass SSTableInfo vector to merge_runs
        SSTableInfo merged_run_info = merge_runs(target_level_num, runs_to_merge_info);

        if (!merged_run_info.filename.empty()) {
             // Merge successful

             // 1. Clear the runs from the current level (they are now merged)
             current_level->clear_runs(); // Use the clear_runs method

             // 2. Delete the physical files that were merged
             delete_sst_files(files_to_delete); // Use the filenames collected earlier

             // 3. Add the new merged run (with its info) to the *next* level (if valid level)
             // The target_level_num check is now done before calling merge_runs
             levels_[target_level_num]->add_run(merged_run_info);

             // 4. Recursively check if the *next* level now needs merging
             // Check if the *next* level could potentially trigger a merge
             if (target_level_num < MAX_LEVELS) { // Only trigger if target is not the absolute max level
                 check_and_trigger_merge(target_level_num);
             } else {
                 // If merged into MAX_LEVELS, check if MAX_LEVELS needs to merge into itself
                 // (This would be the transition from tiering to leveling in the last level,
                 // or just managing runs within the last tiering level).
                 // For simple tiering, it just accumulates, so no recursive call needed if > MAX_LEVELS initially.
                 // If target_level_num == MAX_LEVELS, the merge was into the last level.
                 // Check if THIS level (MAX_LEVELS) now has too many runs *after* adding the merged run.
                  if (levels_[MAX_LEVELS]->get_run_count() >= SIZE_RATIO && level_num < MAX_LEVELS) {
                      // This handles the cascading merge arriving at MAX_LEVELS
                       check_and_trigger_merge(MAX_LEVELS);
                  } else if (levels_[MAX_LEVELS]->get_run_count() >= SIZE_RATIO && level_num == MAX_LEVELS) {
                       // This is a merge *within* MAX_LEVELS itself if its run count exceeds SIZE_RATIO
                       // (e.g., 5 runs become 1 run in the same level). This isn't typical tiering behavior
                       // but could be implemented as a leveling step in the last level.
                       // The current merge_runs puts the output in target_level_num. If target_level_num == MAX_LEVELS,
                       // it goes back into MAX_LEVELS. This effectively compacts MAX_LEVELS.
                       // The recursive call should therefore still check MAX_LEVELS if the merge target was MAX_LEVELS.
                       check_and_trigger_merge(MAX_LEVELS); // Recursive call for MAX_LEVELS
                  }
             }


        } else {
            std::cerr << "Error: Merge failed for level " << level_num << ". Files remain and level state is inconsistent." << std::endl;
            // This is a critical error state. Files are not deleted, level is not cleared.
            // Recovery or specific error handling might be needed.
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

            // Write the flushed data to the new SSTable file and get its info
            SSTableInfo new_run_info = write_sstable(data_to_flush, new_sstable_file);

            if (!new_run_info.filename.empty()) { // Check if write was successful
                // Add the new run (info) to Level 1
                levels_[1]->add_run(new_run_info);

                // Check if Level 1 needs merging now
                check_and_trigger_merge(1);
            } else {
                std::cerr << "Error: Failed to write flushed memtable to disk. Data potentially lost." << std::endl;
                 // Error handling: Data from flush is lost. Attempting to re-insert the triggering pair.
                 // If insert() fails again, something is fundamentally wrong.
                 // We should probably not try to re-insert the *flushed* data, just the current kv_pair.
                 // The logic here seems slightly off from typical LSM recovery on flush fail.
                 // A simple approach for this exercise is to just fail the original insert operation.
                 // Let's return false immediately on write_sstable failure.
                 // The current kv_pair was NOT inserted yet, so the caller might retry it.
                 return false; // Indicate failure to insert (due to flush failure)
            }
        } else {
             // Memtable was full but flushed to an empty vector? Should not happen if curr_size_ > 0.
             // If it happens, the original kv_pair still needs inserting.
             // If flush was empty, it means curr_size_ was 0, which contradicts is_full().
             // This branch is likely indicative of a logic error elsewhere.
             // Let's still try inserting the original kv_pair, though it's unexpected.
              std::cerr << "Warning: Memtable full but flush returned empty data." << std::endl;
              if (!memtable_ptr_->insert(kv_pair)) {
                  std::cerr << "Critical Error: Could not insert element into memtable even after anomalous flush." << std::endl;
                   return false;
              }
        }

        // If flush was successful, the original kv_pair still needs to be inserted
        // because the initial memtable_ptr_->insert(kv_pair) returned false.
        // This is a common pattern: attempt insert, if full, flush, then insert the *same* pair again.
        // The insert after flush will definitely succeed if the memtable is now empty.
        bool insert_after_flush_ok = memtable_ptr_->insert(kv_pair);
        if (!insert_after_flush_ok) {
             std::cerr << "Critical Error: Failed to insert element into empty memtable after successful flush." << std::endl;
            return false; // Indicate critical failure
        }
        return true; // Insert successful after flush
    }
    return true; // Insert successful (directly into memtable)
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
            if (levels_[i]) { // Ensure level exists
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
    for (int k = start; k <= end; ++k) { // Inclusive range
        get(k, true); // Call get in range mode, discard return value (it already prints)
    }
    cout << endl;
}

void lsm_tree::delete_key(int key) {
    // Insert a tombstone entry for the key.
    // The insert logic will handle updates/memtable flushing/compaction.
    insert({key, 0, true}); // Value doesn't matter for tombstone
}

void lsm_tree::printStats() {
    std::cout << "--- LSM Tree Stats ---" << std::endl;

    // Data structures to hold intermediate results
    std::map<int, std::pair<int, std::string>> logical_data; // Map<key, Pair<value, location>>
    std::set<int> deleted_keys;                             // Keep track of keys confirmed deleted
    std::vector<long long> physical_key_counts(MAX_LEVELS + 1, 0); // Count all keys per level file

    // --- Stage 1: Process data from newest to oldest to find logical state ---

    // 1.a Process Memtable
    // std::cout << "Debug: Processing Memtable..." << std::endl;
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

        // Process runs within the level (newest run first - reverse iteration of sstable_runs_)
        for (auto it = current_level->sstable_runs_.rbegin(); it != current_level->sstable_runs_.rend(); ++it) {
            const SSTableInfo& run_info = *it;
            const std::string& filename = run_info.filename;
            // std::cout << "Debug:   Reading file " << filename << " for stats..." << std::endl;

            // Open in text mode
            std::ifstream infile(filename);
            if (!infile) {
                std::cerr << "Warning: Could not open SSTable TXT file for stats: " << filename << std::endl;
                continue;
            }

            long long current_file_key_count = 0;
            std::string line;

            // Read line by line
            while (std::getline(infile, line)) {
                 if (line.empty()) continue; // Skip empty lines

                std::stringstream ss(line);
                int current_key, current_value, tombstone_flag;

                // Read from stringstream
                 if (ss >> current_key >> current_value >> tombstone_flag) {
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
                } else {
                     std::cerr << "Warning: Parsing error during stats in file: " << filename << ", line: " << line << std::endl;
                }
            }
            if (!infile.eof() && infile.fail()) {
                std::cerr << "Warning: Read error or parsing issue near EOF during stats in file: " << filename << std::endl;
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
    // std::map iterators traverse keys in sorted order.
    // Group by level for printing as requested in original code structure
    std::map<std::string, std::vector<std::pair<int, int>>> entries_by_location; // Group by M or L<num>

     // Group by location first
    for(const auto& pair : logical_data) {
        int key = pair.first;
        int value = pair.second.first;
        std::string location = pair.second.second;
        entries_by_location[location].push_back({key, value});
    }

    // Print Memtable entries first ("M")
    if(entries_by_location.count("M")) {
         for(const auto& kv_pair : entries_by_location["M"]) {
              std::cout << kv_pair.first << ":" << kv_pair.second << ":M ";
         }
          std::cout << std::endl; // Newline after memtable entries
    }


    // Print Level entries ("L1" to "LMAX_LEVELS") in order
     for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
         std::string location_str = "L" + std::to_string(level_num);
         if (entries_by_location.count(location_str)) {
             for (const auto& kv_pair : entries_by_location[location_str]) {
                 std::cout << kv_pair.first << ":" << kv_pair.second << ":" << location_str << " ";
             }
             std::cout << std::endl; // Newline after each level's entries
         }
     }


    std::cout << "----------------------" << std::endl;
}

// Explicit function to delete all SSTable files and directories
void lsm_tree::cleanup_files() {
    std::cout << "Cleaning up ALL SSTable files and directories..." << std::endl;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
            // Get filenames from SSTableInfo objects
            std::vector<std::string> files_to_delete = levels_[i]->get_run_filenames();

            // Delete files stored in memory
            delete_sst_files(files_to_delete);

            // Clear the list of SSTableInfo objects in memory
            levels_[i]->clear_runs(); // Use the clear_runs method

            // Optionally remove the directory itself
            std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
             if (rmdir(level_dir.c_str()) != 0) {
                 if (errno != ENOTEMPTY) {
                      std::cerr << "Warning: Could not remove directory " << level_dir << ": " << strerror(errno) << std::endl;
                 } else {
                     // std::cerr << "Info: Directory not empty, not removed: " << level_dir << std::endl;
                 }
             } else {
                 // std::cout << "Removed directory: " << level_dir << std::endl;
             }
        }
    }
    // Optionally remove the root data directory
    if (rmdir(DATA_DIR.c_str()) != 0) {
         if (errno != ENOTEMPTY) {
             std::cerr << "Warning: Could not remove root data directory " << DATA_DIR << ": " << strerror(errno) << std::endl;
         } else {
              // std::cerr << "Info: Root data directory not empty, not removed: " << DATA_DIR << std::endl;
         }
    } else {
        // std::cout << "Removed directory: " << DATA_DIR << std::endl;
    }

     // Reset run ID generator
     next_run_id_ = 0;
}