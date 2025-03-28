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
#include <cstring> // For strerror
#include <stdexcept> // For runtime_error
#include <cmath> // For std::ceil (though not strictly needed with byte counting)


using namespace std;

// --- Define data directory constant ---
const std::string DATA_DIR = "data";

// Size of a single key_value entry in binary format
const size_t KV_ENTRY_SIZE = sizeof(int) + sizeof(int) + sizeof(char); // key + value + tombstone

// Size of a single index entry in binary format
const size_t INDEX_ENTRY_SIZE = sizeof(int) + sizeof(long long); // last_key + block_offset


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

// Helper to read metadata (including block index) from an existing BINARY SSTable file
SstMetadata lsm_tree::read_sst_metadata(const std::string& filename) {
    SstMetadata metadata(filename); // Initialize with filename

    // Open file in binary mode at the end
    std::ifstream infile(filename, std::ios::binary | std::ios::ate);
    if (!infile) {
        std::cerr << "Warning: Could not open SSTable BINARY file for reading metadata: " << filename << std::endl;
        return metadata; // Return invalid metadata
    }

    std::streampos file_size = infile.tellg();

    // File must be large enough to hold at least the index size marker
    if (file_size < (std::streampos)sizeof(size_t)) {
        // std::cerr << "Warning: SSTable file too small for index size: " << filename << std::endl;
        infile.close();
        return metadata; // Return invalid metadata
    }

    // Read the index entry count from the end of the file
    infile.seekg(file_size - (std::streampos)sizeof(size_t));
    size_t index_entry_count = 0;
    infile.read(reinterpret_cast<char*>(&index_entry_count), sizeof(size_t));

    if (infile.fail()) {
         std::cerr << "Warning: Failed to read index entry count from file: " << filename << std::endl;
         infile.close();
         return metadata; // Return invalid metadata
    }


    // Calculate the size of the index section in bytes
    std::streampos index_section_size = (std::streampos)index_entry_count * INDEX_ENTRY_SIZE;
    std::streampos index_section_start = file_size - (std::streampos)sizeof(size_t) - index_section_size;

    // Basic sanity check on calculated index position
    if (index_section_start < 0 || index_section_start > file_size - (std::streampos)sizeof(size_t)) {
        std::cerr << "Warning: Calculated index start offset is invalid: " << filename << std::endl;
        infile.close();
        return metadata; // Return invalid metadata
    }


    // Seek to the beginning of the index section
    infile.seekg(index_section_start);

    // Read the index entries
    metadata.block_index_.resize(index_entry_count);
    for (size_t i = 0; i < index_entry_count; ++i) {
        int last_key;
        long long block_offset;
        infile.read(reinterpret_cast<char*>(&last_key), sizeof(int));
        infile.read(reinterpret_cast<char*>(&block_offset), sizeof(long long));
        if (infile.fail()) {
            std::cerr << "Warning: Failed to read index entry " << i << " from file: " << filename << std::endl;
             metadata.block_index_.clear(); // Invalidate partial index
             infile.close();
             return metadata; // Return invalid metadata
        }
        metadata.block_index_[i] = {last_key, block_offset};
    }

    infile.close();

    // Populate min_key and max_key from the loaded index
    if (!metadata.block_index_.empty()) {
        // Min key is the first key in the first block. We need to read it from the file.
        // Let's open the file again just to read the first key. This is acceptable during loading.
        std::ifstream first_key_file(filename, std::ios::binary);
        if(first_key_file && metadata.block_index_[0].second == 0) { // First block starts at offset 0
             int first_key;
             first_key_file.read(reinterpret_cast<char*>(&first_key), sizeof(int));
             if(!first_key_file.fail()) {
                 metadata.min_key = first_key;
             }
        }
        // Max key is the last key in the last block, which is stored in the index.
        metadata.max_key = metadata.block_index_.back().first;
    } else {
         // Index is empty, implies the file is empty or corrupt, min/max remain sentinels.
         // This should be caught by the size check or subsequent usage.
         metadata.min_key = std::numeric_limits<int>::max();
         metadata.max_key = std::numeric_limits<int>::min();
    }

    // std::cout << "Loaded metadata for " << filename << " [" << metadata.min_key << "," << metadata.max_key << "], " << metadata.block_index_.size() << " index entries." << std::endl;

    return metadata;
}


// --- Helper Struct for Merge (remains similar) ---
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
    // sstable_metadata_ is already default-initialized (empty vector)
}

level::~level() {
    // Destructor does not delete physical files. Use cleanup_files().
}

void level::add_run(const SstMetadata& metadata) {
    // Could potentially sort metadata by run_id here for consistent newest-first iteration
    // but current simple push_back relies on load order or manual sorting if needed.
    // For tiering, iteration order for find_key (rbegin) is key. If loading happens by run_id order,
    // reverse iteration correctly searches newer files first.
    sstable_metadata_.push_back(metadata);
}

// Search key in this level's SSTables (files) using block index
bool level::find_key(int key, int& value, bool& is_tombstone) {
    // Search runs in reverse order (newest first)
    for (auto it = sstable_metadata_.rbegin(); it != sstable_metadata_.rend(); ++it) {
        const SstMetadata& metadata = *it;
        const std::string& filename = metadata.filename;

        if (!metadata.is_valid()) {
             std::cerr << "Warning: Skipping invalid SSTable metadata: " << filename << std::endl;
             continue;
        }

        // Use min/max fence pointers to quickly check if the key might be in this file
        if (key < metadata.min_key || key > metadata.max_key) {
            // std::cout << "Skipping file " << filename << " (key " << key << " outside range [" << metadata.min_key << "," << metadata.max_key << "])" << std::endl; // Debug skip
            continue; // Key is outside this file's overall range, skip opening it
        }

        // Key might be in this file. Use the block index to find the potential block.
        // Search the index for the first block whose last key is >= search_key.
        // upper_bound finds the first element GREATER than the search key.
        // We want the block whose last key is >= the search key.
        // If search_key is S, and index is [(L1,O1), (L2,O2), (L3,O3)], we want block k where L_k >= S.
        // std::lower_bound finds the first element not less than value.
        // Let's use lower_bound on a pair {key, min_long_long} to find the first index entry
        // where last_key >= key.
        auto index_it = std::lower_bound(
            metadata.block_index_.begin(), metadata.block_index_.end(),
            std::make_pair(key, std::numeric_limits<long long>::min()),
            // Use explicit types instead of auto for lambda parameters
            [](const std::pair<int, long long>& entry, const std::pair<int, long long>& val) {
                return entry.first < val.first; // Compare based on last_key (the 'int' part)
            }
        );

        if (index_it == metadata.block_index_.end()) {
             // This case happens if key > the last_key of the very last block.
             // Since we already checked key <= metadata.max_key, this shouldn't happen if max_key is correct.
             // But as a safeguard or if max_key wasn't set correctly, it means the key is not in this file.
             // std::cout << "Debug: Key " << key << " greater than max_key or outside last block's range in " << filename << std::endl;
             continue; // Key is not in this file
        }

        // 'index_it' now points to the metadata for the first block whose last key is >= `key`.
        // This is the block that might contain `key`.
        long long block_start_offset = index_it->second;

        // Determine the end offset of this block
        long long block_end_offset;
        auto next_index_it = std::next(index_it);
        if (next_index_it != metadata.block_index_.end()) {
            block_end_offset = next_index_it->second; // The start of the next block
        } else {
            // This is the last block. Its end is the start of the index section.
            // We need the total file size and index size to calculate this.
            // Re-reading metadata here is inefficient. We could store file_size and index_start_offset in SstMetadata.
            // For simplicity now, let's re-read the file size and index count.
            // A better approach: Store index_start_offset in SstMetadata.
            std::ifstream size_file(filename, std::ios::binary | std::ios::ate);
            if (!size_file) {
                std::cerr << "Warning: Could not get file size for block calculation: " << filename << std::endl;
                continue;
            }
            std::streampos total_file_size = size_file.tellg();
            size_file.seekg(total_file_size - (std::streampos)sizeof(size_t));
            size_t index_count = 0;
            size_file.read(reinterpret_cast<char*>(&index_count), sizeof(size_t));
            if (size_file.fail()) {
                 std::cerr << "Warning: Failed to read index count for block calculation: " << filename << std::endl;
                 continue;
            }
            block_end_offset = total_file_size - (std::streampos)sizeof(size_t) - (std::streampos)index_count * INDEX_ENTRY_SIZE;
             size_file.close();
        }


        // Open the SSTable file and seek to the determined block offset
        std::ifstream infile(filename, std::ios::binary);
        if (!infile) {
            std::cerr << "Warning: Could not open SSTable BINARY file for reading: " << filename << std::endl;
            continue; // Skip this file if it can't be opened
        }

        infile.seekg(block_start_offset);

        // Read entries within this block
        while (infile.tellg() < block_end_offset) {
            int current_key;
            int current_value;
            char tombstone_flag_char;

            // Read binary entry
            infile.read(reinterpret_cast<char*>(&current_key), sizeof(int));
            infile.read(reinterpret_cast<char*>(&current_value), sizeof(int));
            infile.read(reinterpret_cast<char*>(&tombstone_flag_char), sizeof(char));

            if (infile.fail()) {
                // Reached end of file unexpectedly or read error
                // std::cerr << "Warning: Read error or premature EOF within block in file: " << filename << std::endl;
                infile.close();
                break; // Exit inner while loop, continue to next file
            }

            bool current_tombstone = (tombstone_flag_char == 1);

            if (current_key == key) {
                value = current_value;
                is_tombstone = current_tombstone;
                infile.close();
                return true; // Key found
            }

            // Optimization: Since entries within a block are sorted, if we pass the key, it's not in this block or this file.
            if (current_key > key) {
                 infile.close();
                 return false; // Key is not in this file
            }
        }

        // If the loop finishes without finding the key or passing it,
        // it means the key wasn't in this block (and thus not in this file).
        infile.close();
        // Continue to the next file in the level (reverse iteration)

    }
    return false; // Key not found in any run of this level
}


// --- Memtable Class Implementation (No change needed here) ---
memtable::memtable() {
    memtable_.reserve(MEMTABLE_CAPACITY);
}

bool memtable::insert(key_value kv_pair) {
    // Linear search for update
    for (int i = 0; i < curr_size_; ++i) {
        if (memtable_[i].key == kv_pair.key) {
            memtable_[i].value = kv_pair.value;
            memtable_[i].tombstone = kv_pair.tombstone;
            return true; // Update successful
        }
    }

    // If key not found and memtable is full
    if (is_full()) {
       return false; // Indicate memtable is full
    }

    // Add new entry
    memtable_.push_back(kv_pair);
    ++curr_size_;
    return true; // Insert successful
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
                // Check if it's an SST file (check suffix)
                if (filename.length() > SST_FILE_SUFFIX.length() &&
                    filename.substr(filename.length() - SST_FILE_SUFFIX.length()) == SST_FILE_SUFFIX) {

                    std::string full_path = level_dir + "/" + filename;
                    // Read metadata (including block index) from the binary file
                    SstMetadata metadata = read_sst_metadata(full_path);

                    // Only add if metadata is valid (implies file was read correctly and has index)
                    if (metadata.is_valid()) {
                        levels_[i]->add_run(metadata); // Add metadata
                        // std::cout << "Found existing SSTable: " << full_path << " [" << metadata.min_key << "," << metadata.max_key << "], " << metadata.block_index_.size() << " blocks" << std::endl;

                        // Parse run ID from filename (e.g., "run_123.sst")
                        size_t run_pos = filename.find("run_");
                        size_t sst_pos = filename.rfind(SST_FILE_SUFFIX);
                        if (run_pos != std::string::npos && sst_pos != std::string::npos && run_pos + 4 < sst_pos) {
                             try {
                                 long long run_id = std::stoll(filename.substr(run_pos + 4, sst_pos - (run_pos + 4)));
                                 if (run_id > max_run_id_found) {
                                     max_run_id_found = run_id;
                                 }
                             } catch (...) {
                                  std::cerr << "Warning: Could not parse run ID from filename: " << filename << std::endl;
                             }
                        } else {
                             std::cerr << "Warning: Filename format unexpected, could not parse run ID: " << filename << std::endl;
                        }
                    } else {
                         // File might be empty, corrupt, or metadata read failed
                         std::cerr << "Warning: Skipping invalid or empty SSTable file during load: " << full_path << std::endl;
                    }
                }
            }
            closedir(dirp);
        } else {
             std::cerr << "Warning: Could not open level directory for reading: " << level_dir << std::endl;
        }
        // Note: Add sorting by run ID here if you want strict newest-first iteration in find_key
        // std::sort(levels_[i]->sstable_metadata_.begin(), levels_[i]->sstable_metadata_.end(), /* custom comparator based on run_id */);

        current_capacity *= SIZE_RATIO; // Capacity logic is less relevant for tiering size threshold
    }

     // Set the next run ID to be one greater than the highest found
    next_run_id_ = max_run_id_found + 1;
    // std::cout << "Starting next run ID at: " << next_run_id_ << std::endl;
}

lsm_tree::~lsm_tree() {
    // --- Start Shutdown Flush Logic ---
    // Flush remaining memtable on shutdown
    if (memtable_ptr_ && memtable_ptr_->curr_size_ > 0) {
        // std::cout << "LSM Tree Destructor: Memtable not empty, performing final flush..." << std::endl;
        std::vector<key_value> data_to_flush = memtable_ptr_->flush(); // Flush remaining data

        if (!data_to_flush.empty()) {
            // Generate filename for Level 1
            std::string final_sstable_file = generate_sstable_filename(1); // Will use the next available run ID

            // Write flushed data to disk and get metadata
            SstMetadata flushed_metadata = write_sstable(data_to_flush, final_sstable_file);

            if (flushed_metadata.is_valid()) { // Check if write was successful and produced a valid file
                 // Add run metadata to Level 1's list (in memory, won't persist unless saved)
                 if (levels_.size() > 1 && levels_[1]) { // Basic check
                      levels_[1]->add_run(flushed_metadata); // Add metadata for consistency
                 }
                 // std::cout << "Final flush successful to: " << final_sstable_file << " [" << flushed_metadata.min_key << "," << flushed_metadata.max_key << "]" << std::endl;
                 // Compaction is skipped on shutdown.
            } else {
                 // std::cerr << "Error: Failed to write final memtable flush to disk during shutdown! Data potentially lost." << std::endl;
            }
        } else {
             // std::cout << "Memtable flushed but was empty." << std::endl;
        }
    }
    // --- End Shutdown Flush Logic ---


    delete memtable_ptr_;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) { // Check if the pointer is valid before deleting
             delete levels_[i]; // This deletes the level object, not the files
        }
    }
     // Note: Physical files on disk are NOT deleted by the destructor by default.
     // The `cleanup_files()` function is provided for explicit cleanup.
}


// Helper to generate unique SSTable filenames
std::string lsm_tree::generate_sstable_filename(int level_num) {
    // Construct path: DATA_DIR / L<level_num> / run_<id>.sst
    std::string level_dir = DATA_DIR + "/L" + std::to_string(level_num);
    return level_dir + "/run_" + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

// Helper to write sorted data to a BINARY SSTable file with block index, returns metadata
SstMetadata lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename) {
    SstMetadata metadata(filename); // Initialize metadata with the filename

    // Open file in binary mode for writing
    // ios::trunc ensures it's a new file or overwrites
    std::ofstream outfile(filename, std::ios::binary | std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open SSTable BINARY file for writing: " << filename << std::endl;
        return SstMetadata(); // Return invalid metadata to indicate failure
    }

    if (data.empty()) {
        // std::cout << "Warning: Writing empty SSTable (or all tombstones) to: " << filename << std::endl;
        // File is created, but index will be empty. is_valid() will be false.
         outfile.close();
         return metadata; // Return metadata with empty index
    }

    long long current_block_start_offset = 0;
    size_t bytes_in_current_block = 0;
    int last_key_in_current_block = data[0].key; // Will be updated

    metadata.min_key = data.front().key; // First key is the overall min key

    // Write each key-value pair as a binary entry
    for (const auto& kv : data) {
        // Check if writing the current entry would exceed block size
        if (bytes_in_current_block > 0 && bytes_in_current_block + KV_ENTRY_SIZE > BLOCK_SIZE_BYTES) {
            // Finish the current block
            metadata.block_index_.push_back({last_key_in_current_block, current_block_start_offset});

            // Start a new block
            current_block_start_offset = outfile.tellp();
            bytes_in_current_block = 0;
            // The last_key_in_current_block will be the key of the *first* entry in the *next* iteration
        }

        // Write the entry: key, value, tombstone flag (as char 0 or 1)
        char tombstone_flag_char = kv.tombstone ? 1 : 0;
        outfile.write(reinterpret_cast<const char*>(&kv.key), sizeof(int));
        outfile.write(reinterpret_cast<const char*>(&kv.value), sizeof(int));
        outfile.write(reinterpret_cast<const char*>(&tombstone_flag_char), sizeof(char));

        if (!outfile) { // Check stream state after each write
            std::cerr << "Error: Failed to write entry to SSTable BINARY file: " << filename << std::endl;
             outfile.close();
             std::remove(filename.c_str()); // Attempt to clean up
             return SstMetadata(); // Indicate failure
        }

        bytes_in_current_block += KV_ENTRY_SIZE;
        last_key_in_current_block = kv.key; // Update last key in the potential current block
    }

    // Add the index entry for the last block
    metadata.block_index_.push_back({last_key_in_current_block, current_block_start_offset});
    metadata.max_key = last_key_in_current_block; // Last key written is the overall max key

    // Write the index section
    for (const auto& index_entry : metadata.block_index_) {
        outfile.write(reinterpret_cast<const char*>(&index_entry.first), sizeof(int)); // last_key
        outfile.write(reinterpret_cast<const char*>(&index_entry.second), sizeof(long long)); // block_offset
        if (!outfile) {
            std::cerr << "Error: Failed to write index entry to SSTable BINARY file: " << filename << std::endl;
             outfile.close();
             std::remove(filename.c_str()); // Attempt to clean up
             return SstMetadata(); // Indicate failure
        }
    }

    // Write the count of index entries at the very end
    size_t index_entry_count = metadata.block_index_.size();
    outfile.write(reinterpret_cast<const char*>(&index_entry_count), sizeof(size_t));
    if (!outfile) {
        std::cerr << "Error: Failed to write index size to SSTable BINARY file: " << filename << std::endl;
         outfile.close();
         std::remove(filename.c_str()); // Attempt to clean up
         return SstMetadata(); // Indicate failure
    }


    outfile.close();
    if (!outfile) { // Check close status
        std::cerr << "Error: Failed to close SSTable BINARY file properly: " << filename << std::endl;
         std::remove(filename.c_str());
         return SstMetadata(); // Indicate failure
    }
    // std::cout << "Successfully wrote SSTable BINARY: " << filename << " [" << metadata.min_key << "," << metadata.max_key << "], " << metadata.block_index_.size() << " blocks" << std::endl;
    return metadata; // Return the metadata of the successfully written file
}

// Helper to delete SSTable files given their metadata
void lsm_tree::delete_sst_files(const std::vector<SstMetadata>& files_metadata) {
    for (const auto& metadata : files_metadata) {
        const std::string& filename = metadata.filename;
        if (std::remove(filename.c_str()) != 0) {
            // std::cerr << "Warning: Could not delete SSTable file: " << filename << " (" << strerror(errno) << ")" << std::endl;
        } else {
            //  std::cout << "Deleted old SSTable: " << filename << std::endl;
        }
    }
}

// --- Merge Logic ---
// Performs a k-way merge on the given BINARY run files, writes result to a new BINARY file, returns metadata of the new file.
SstMetadata lsm_tree::merge_runs(int target_level_num, const std::vector<SstMetadata>& runs_to_merge_metadata) {
    // std::cout << "Merging " << runs_to_merge_metadata.size() << " BINARY runs into level " << target_level_num << "..." << std::endl;

    std::vector<std::ifstream> input_streams;
    input_streams.reserve(runs_to_merge_metadata.size());

    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input BINARY files and read the first entry from each
    for (size_t i = 0; i < runs_to_merge_metadata.size(); ++i) {
        const std::string& filename = runs_to_merge_metadata[i].filename;
        // Open in binary mode
        input_streams.emplace_back(filename, std::ios::binary);
        if (!input_streams.back()) {
            std::cerr << "Error: Could not open BINARY file for merge: " << filename << std::endl;
            // Clean up already opened streams
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
            return SstMetadata(); // Indicate merge failure
        }

        // Read the first binary entry
        int current_key, current_value;
        char tombstone_flag_char;
        input_streams.back().read(reinterpret_cast<char*>(&current_key), sizeof(int));
        input_streams.back().read(reinterpret_cast<char*>(&current_value), sizeof(int));
        input_streams.back().read(reinterpret_cast<char*>(&tombstone_flag_char), sizeof(char));

        if (!input_streams.back().fail() && !input_streams.back().eof()) { // Check if read was successful
            min_heap.push({{current_key, current_value, (tombstone_flag_char == 1)}, i}); // Construct key_value and push
        } else {
            // File might be empty or failed initial read
             if (!input_streams.back().eof()) { // Check if it wasn't just an empty file
                 std::cerr << "Warning: Failed initial BINARY read from file: " << filename << std::endl;
             }
             input_streams.back().close();
        }
    }

    // Generate filename for the new merged run
    std::string output_filename = generate_sstable_filename(target_level_num);
    SstMetadata merged_metadata(output_filename); // Prepare metadata for the output file

    // Open output in binary mode
    std::ofstream outfile(output_filename, std::ios::binary | std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open output BINARY file for merge: " << output_filename << std::endl;
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return SstMetadata(); // Indicate merge failure
    }

    key_value last_written_kv;
    bool first_write = true;

    long long current_block_start_offset = 0;
    size_t bytes_in_current_block = 0;
    int last_key_in_current_block = std::numeric_limits<int>::min(); // Will be updated

    // Merge process
    while (!min_heap.empty()) {
        merge_entry smallest = min_heap.top();
        min_heap.pop();

        // Compaction logic: only process the newest version of a key
        // The priority queue tie-breaker ensures we get the newest first.
        if (first_write || smallest.kv.key != last_written_kv.key) {

             // If it's a new key and not a tombstone, write it.
             // Tombstones are processed but don't result in writing to the output file
             // unless they are the newest version encountered for a key that was previously non-deleted.
             // In a tiering merge, we write the newest non-tombstone, or don't write if newest is tombstone.
             // The logic here is to track the *last seen* version and only write if we haven't seen the key before.
             // Since the PQ gives us newest first, the first time we see a key is its newest version.
             // We should only write if this newest version is *not* a tombstone.

             if (!smallest.kv.tombstone) {
                // Check if writing this entry would exceed block size
                if (bytes_in_current_block > 0 && bytes_in_current_block + KV_ENTRY_SIZE > BLOCK_SIZE_BYTES) {
                    // Finish the current block
                    merged_metadata.block_index_.push_back({last_key_in_current_block, current_block_start_offset});

                    // Start a new block
                    current_block_start_offset = outfile.tellp();
                    bytes_in_current_block = 0;
                }

                // Write the entry: key, value, tombstone flag (as char 0 or 1)
                char tombstone_flag_char = smallest.kv.tombstone ? 1 : 0; // Will be 0 here
                outfile.write(reinterpret_cast<const char*>(&smallest.kv.key), sizeof(int));
                outfile.write(reinterpret_cast<const char*>(&smallest.kv.value), sizeof(int));
                outfile.write(reinterpret_cast<const char*>(&tombstone_flag_char), sizeof(char));

                if (!outfile) {
                    std::cerr << "Error writing entry during merge to BINARY file: " << output_filename << std::endl;
                    // Cleanup needed
                     outfile.close();
                     for(auto& stream : input_streams) if(stream.is_open()) stream.close();
                     std::remove(output_filename.c_str()); // Attempt to remove bad output file
                     return SstMetadata(); // Indicate failure
                }
                 bytes_in_current_block += KV_ENTRY_SIZE;
                 last_key_in_current_block = smallest.kv.key; // Update last key in block

                 if (first_write) merged_metadata.min_key = smallest.kv.key; // First key written is min_key
                 merged_metadata.max_key = smallest.kv.key; // Always update max_key with the last key written

             } // else: tombstone, do not write, just track it was the newest version

             last_written_kv = smallest.kv; // Store this as the latest state for this key
             first_write = false;

        } else {
             // Duplicate key (older version), discard.
        }


        // Read the next element (binary entry) from the same stream
        size_t stream_idx = smallest.stream_index;
        if (input_streams[stream_idx].is_open() && !input_streams[stream_idx].eof()) {
             int next_key, next_value;
             char next_tombstone_flag_char;

             // Read binary entry
             input_streams[stream_idx].read(reinterpret_cast<char*>(&next_key), sizeof(int));
             input_streams[stream_idx].read(reinterpret_cast<char*>(&next_value), sizeof(int));
             input_streams[stream_idx].read(reinterpret_cast<char*>(&next_tombstone_flag_char), sizeof(char));


             if (!input_streams[stream_idx].fail() && !input_streams[stream_idx].eof()) { // Check if read was successful and not just EOF
                 min_heap.push({{next_key, next_value, (next_tombstone_flag_char == 1)}, stream_idx});
             } else {
                 // End of this stream reached or read error
                 if (!input_streams[stream_idx].eof()) { // It was a read error, not just EOF
                     std::cerr << "Warning: Read error or parsing issue mid-merge in file: " << runs_to_merge_metadata[stream_idx].filename << std::endl;
                 }
                 input_streams[stream_idx].close();
             }
        }
    } // while (!min_heap.empty())

    // Add the index entry for the last block written, if any data was written
    if (bytes_in_current_block > 0) { // Check if the last block actually contains data
         merged_metadata.block_index_.push_back({last_key_in_current_block, current_block_start_offset});
    }
    // If no data was ever written (e.g., merging only tombstones or empty files), block_index_ is empty.
    // min_key and max_key remain sentinels from initialization.

    // Write the index section if there are entries
    if (!merged_metadata.block_index_.empty()) {
        // long long index_section_start_offset = outfile.tellp(); // Record where the index starts - not needed for metadata
        for (const auto& index_entry : merged_metadata.block_index_) {
            outfile.write(reinterpret_cast<const char*>(&index_entry.first), sizeof(int)); // last_key
            outfile.write(reinterpret_cast<const char*>(&index_entry.second), sizeof(long long)); // block_offset
            if (!outfile) {
                std::cerr << "Error: Failed to write index entry during merge to BINARY file: " << output_filename << std::endl;
                 outfile.close();
                 for(auto& stream : input_streams) if(stream.is_open()) stream.close();
                 std::remove(output_filename.c_str()); // Attempt to clean up
                 return SstMetadata(); // Indicate failure
            }
        }

        // Write the count of index entries at the very end
        size_t index_entry_count = merged_metadata.block_index_.size();
        outfile.write(reinterpret_cast<const char*>(&index_entry_count), sizeof(size_t));
         if (!outfile) {
            std::cerr << "Error: Failed to write index size during merge to BINARY file: " << output_filename << std::endl;
             outfile.close();
             for(auto& stream : input_streams) if(stream.is_open()) stream.close();
             std::remove(output_filename.c_str()); // Attempt to clean up
             return SstMetadata(); // Indicate failure
        }

    }


    // Close output file
    outfile.close();
    if (!outfile) { // Check close status
        std::cerr << "Error closing merged output BINARY file: " << output_filename << std::endl;
         std::remove(output_filename.c_str());
         return SstMetadata(); // Indicate failure
    }

    // Close any remaining input streams (should be handled by the loop, but safety check)
    for (auto& stream : input_streams) {
        if (stream.is_open()) {
            stream.close();
        }
    }

    // If the output file was actually empty (e.g., merging only tombstones), delete the file.
    // The merged_metadata.is_valid() check relies on block_index_ not being empty.
     if (!merged_metadata.is_valid()) {
         std::remove(output_filename.c_str());
        //  std::cout << "Merged result was empty (only tombstones), deleted empty file: " << output_filename << std::endl;
     }


    // std::cout << "Merge complete. New BINARY run: " << output_filename << " [" << merged_metadata.min_key << "," << merged_metadata.max_key << "], " << merged_metadata.block_index_.size() << " blocks." << std::endl;
    return merged_metadata; // Return the metadata of the newly created merged file
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
        std::vector<SstMetadata> files_to_merge_metadata = current_level->sstable_metadata_;

        // Perform the merge. The result goes into the *next* level.
        int target_level_num = level_num + 1;
        SstMetadata merged_metadata = merge_runs(target_level_num, files_to_merge_metadata);

        if (merged_metadata.is_valid()) { // Check if merge was successful and produced a non-empty file
             // Merge successful and result is not empty

             // 1. Clear the metadata from the current level (they are now merged)
             current_level->sstable_metadata_.clear();

             // 2. Delete the physical files that were merged
             delete_sst_files(files_to_merge_metadata);

             // 3. Add the new merged run metadata to the *next* level (if valid level)
             if (target_level_num <= MAX_LEVELS) {
                 levels_[target_level_num]->add_run(merged_metadata);

                 // 4. Recursively check if the *next* level now needs merging
                 check_and_trigger_merge(target_level_num);
             } else {
                 // Merged into a conceptual level beyond MAX_LEVELS.
                 // This merged file is the final state. For simple tiering, add it back to MAX_LEVELS.
                 levels_[MAX_LEVELS]->add_run(merged_metadata);
                 // cout << "Warning: Merge occurred at MAX level (" << MAX_LEVELS << "). Result added back to MAX level: " << merged_metadata.filename << endl;
             }

        } else {
            // Merge failed or the result was an empty file (meaning all keys were deleted).
            // merge_runs already handles deleting the empty output file.
            // If merge_runs returned an invalid metadata, it means a failure occurred.
            if (!merged_metadata.filename.empty()) {
                 // This means the merge resulted in an empty file which was deleted.
                 // The files_to_merge_metadata are still in the current level's list.
                 // We *should* remove them if they were successfully processed into an empty result.
                 // Let's rely on merge_runs returning invalid metadata ONLY on failure,
                 // and return valid metadata (with empty index) for a successful merge resulting in an empty file.
                 // Re-evaluating: merge_runs currently returns SstMetadata() on error, and potentially valid SstMetadata (empty index) for empty result.
                 // is_valid() checks for non-empty index.
                 // So if (!merged_metadata.is_valid() && !merged_metadata.filename.empty()), it was an empty result file which was deleted by merge_runs.
                 // If (!merged_metadata.is_valid() && merged_metadata.filename.empty()), it was a failure.

                 // For now, if merge_runs was called and files_to_merge_metadata was not empty,
                 // assume the merge files *should* be deleted to prevent infinite merge loops on full levels,
                 // even if the result is empty or merge failed. This might lose data on failure, but prevents cascade.
                 // A production system needs better failure handling.
                 std::cerr << "Warning: Merge for level " << level_num << " resulted in an empty file or failed. Deleting source files anyway." << std::endl;
                 current_level->sstable_metadata_.clear();
                 delete_sst_files(files_to_merge_metadata);

            } else {
                 // cerr << "Error: Merge failed for level " << level_num << ". Files remain in level " << level_num << "." << endl;
                 // Files that failed to merge remain in the current level. This might break tiering size limits.
            }
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

            // Write the flushed data to the new SSTable file and get its metadata
            SstMetadata flushed_metadata = write_sstable(data_to_flush, new_sstable_file);

            if (flushed_metadata.is_valid()) { // Check if write was successful and produced a non-empty file
                // Add the new run's metadata to Level 1
                levels_[1]->add_run(flushed_metadata);

                // Check if Level 1 needs merging now
                check_and_trigger_merge(1);
            } else {
                 // This means write_sstable failed or produced an empty file (unlikely from a full memtable flush).
                 // If it failed (filename empty), data is lost. If result is empty (is_valid=false, filename exists),
                 // means all keys were tombstones? Unlikely from flush. Assume failure.
                 std::cerr << "Error: Failed to write flushed memtable to disk or result was empty. Data potentially lost." << std::endl;
                 // Error handling: what to do? For now, proceed but data from flush is lost.
                 // Try inserting the current kv_pair into the now-empty memtable.
                 bool retry_insert = memtable_ptr_->insert(kv_pair); // This should succeed
                 if(!retry_insert){
                    std::cerr << "Critical Error: Cannot insert element into empty memtable after flush failure." << std::endl;
                    return false; // Indicate critical failure
                 }
                 return true; // Insert succeeded after handling flush failure (partially)
            }
        } else {
             // This case implies memtable flush returned an empty vector, which shouldn't happen if it was full.
            //  cout << "Memtable was full but flush returned no data??" << endl;
             // Still attempt to insert the triggering key into the empty memtable.
             bool retry_insert = memtable_ptr_->insert(kv_pair);
             if(!retry_insert){
                std::cerr << "Critical Error: Cannot insert element into empty memtable." << std::endl;
                return false;
             }
             return true;
        }

        // If we reached here, the flush was successful (or partially handled failure),
        // and the original kv_pair needs to be inserted into the now empty memtable.
        if (!memtable_ptr_->insert(kv_pair)) {
             std::cerr << "Critical Error: Could not insert element into memtable even after flushing and clearing." << std::endl;
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
            if (!called_from_range) cout << endl; // Print empty line for deleted keys outside range scan
            return -1; // Key found but deleted
        }
        // Key found and is not deleted
    } else {
        // 2. Check Levels (SSTables on disk) from L1 to MAX_LEVELS (newest to oldest)
        // The level::find_key method now uses block index to prune blocks within the file.
        for (int i = 1; i <= MAX_LEVELS; ++i) {
            if (levels_[i]->find_key(key, value, is_tombstone)) {
                 found = true;
                 if (is_tombstone) {
                    if (!called_from_range) cout << endl; // Print empty line for deleted keys outside range scan
                    return -1; // Found tombstone in SSTable, stop searching lower levels
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
             // In range mode, print Key:Value format
             cout << key << ":" << value << " ";
         }
         return value;
    } else {
         // Key was not found in memtable or any level, OR found as a tombstone (already handled).
         if (!called_from_range) {
             cout << endl; // Print empty line for not found keys outside range scan
         }
         // In range mode, we print nothing for not-found/deleted keys.
         return -1;
    }
}


void lsm_tree::range(int start, int end) {
    // cout << "Range (" << start << " to " << end << "): "; // Removed as per test format
    // The get(k, true) call inside the loop handles the printing now.
    for (int k = start; k <= end; ++k) {
        get(k, true); // Call get in range mode. It prints "k:value " if found and not deleted.
    }
    cout << endl; // Newline after the range scan is complete
}


void lsm_tree::delete_key(int key) {
    // Insert a tombstone entry for the key.
    insert({key, 0, true}); // Value doesn't matter for tombstone
}


void lsm_tree::printStats() {
    std::cout << "--- LSM Tree Stats ---" << std::endl;

    // Data structures to hold intermediate results for the logical view
    std::map<int, std::pair<int, std::string>> logical_data;
    std::set<int> processed_keys_for_logical_view; // To track newest version seen

    // Map to store physical counts per level (including memtable)
    std::map<std::string, long long> physical_key_counts;

    // --- Stage 1: Process data from newest to oldest to build the logical state ---

    // 1.a Process Memtable (newest)
    physical_key_counts["Memtable"] = memtable_ptr_->curr_size_;
    // Iterate reverse for newest memtable entries first for the logical view
    for (int i = memtable_ptr_->curr_size_ - 1; i >= 0; --i) {
        const auto& kv = memtable_ptr_->memtable_[i];

        if (processed_keys_for_logical_view.count(kv.key)) {
            continue;
        }
        processed_keys_for_logical_view.insert(kv.key);

        if (!kv.tombstone) {
            logical_data[kv.key] = {kv.value, "M"};
        }
    }

    // 1.b Process Levels (from L1 down to MAX_LEVELS)
    for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
        level* current_level = levels_[level_num];
        if (!current_level) continue;

        physical_key_counts["LVL" + std::to_string(level_num)] = 0; // Initialize physical count

        // Process runs within the level (newest run first - reverse iteration over metadata)
        for (auto it = current_level->sstable_metadata_.rbegin(); it != current_level->sstable_metadata_.rend(); ++it) {
            const SstMetadata& metadata = *it;
            const std::string& filename = metadata.filename;

             if (!metadata.is_valid() && !metadata.block_index_.empty()) {
                  // Metadata might be invalid but has index (e.g. read error during load)
                  std::cerr << "Warning: Skipping invalid SSTable metadata for stats: " << filename << std::endl;
                  continue; // Skip this file
             }
             if (metadata.block_index_.empty() && metadata.is_valid()){
                 // is_valid implies non-empty index, this case shouldn't happen.
                  std::cerr << "Warning: Metadata is valid but block_index_ is empty for stats: " << filename << std::endl;
                  continue;
             }
             if (metadata.block_index_.empty() && !metadata.is_valid() && !filename.empty()){
                  // File exists but has empty index (e.g. empty file or tombstone-only merge result)
                  // Physical count is 0. Logical view is empty (no non-tombstones).
                  // std::cout << "Debug: Processing empty file for stats: " << filename << std::endl;
                   continue; // No keys to read from this file for counts or logical view
             }

            // Need to open the binary file to read keys for physical count and logical view
            std::ifstream infile(filename, std::ios::binary);
            if (!infile) {
                std::cerr << "Warning: Could not open SSTable BINARY file for stats: " << filename << std::endl;
                continue;
            }

            // Iterate through blocks using the index to get entries for counts/logical view
            for(const auto& index_entry : metadata.block_index_) {
                long long block_start_offset = index_entry.second;
                long long block_end_offset;

                 // Determine the end offset of this block (handle last block)
                 // Find the next entry in the index vector
                 auto current_index_it = std::lower_bound(
                     metadata.block_index_.begin(), metadata.block_index_.end(),
                     block_start_offset, // Use offset for search value
                     // Use explicit types instead of auto for lambda parameters
                     [](const std::pair<int, long long>& entry, const long long& val) {
                         return entry.second < val; // Compare based on offset
                     }
                 );

                 auto next_index_it = std::next(current_index_it);

                 if (next_index_it != metadata.block_index_.end()) {
                    block_end_offset = next_index_it->second; // The start of the next block
                 } else {
                    // This is the last block. Its end is the start of the index section.
                    // Re-reading file size and index count/size is inefficient but simple here.
                    std::streampos total_file_size = 0;
                    size_t index_count = 0;
                     { // Scope for temporary file stream
                         std::ifstream size_file(filename, std::ios::binary | std::ios::ate);
                          if (!size_file) {
                              std::cerr << "Warning: Could not get file size for stats block calculation: " << filename << std::endl;
                              block_end_offset = infile.tellg(); // Use current position as a fallback? No, this is wrong.
                              break; // Skip processing blocks in this file
                          }
                         total_file_size = size_file.tellg();
                         size_file.seekg(total_file_size - (std::streampos)sizeof(size_t));
                         size_file.read(reinterpret_cast<char*>(&index_count), sizeof(size_t));
                         if (size_file.fail()) {
                              std::cerr << "Warning: Failed to read index count for stats block calculation: " << filename << std::endl;
                              break; // Skip processing blocks in this file
                         }
                     } // size_file closed

                     block_end_offset = total_file_size - (std::streampos)sizeof(size_t) - (std::streampos)index_count * INDEX_ENTRY_SIZE;
                 }

                // Seek to the beginning of the block
                infile.seekg(block_start_offset);
                if (infile.fail()) {
                    std::cerr << "Warning: Seek failed to block offset " << block_start_offset << " in file for stats: " << filename << std::endl;
                    break; // Skip processing blocks in this file
                }

                // Read entries within this block
                while (infile.tellg() < block_end_offset) {
                    int current_key;
                    int current_value;
                    char tombstone_flag_char;

                    // Read binary entry
                    infile.read(reinterpret_cast<char*>(&current_key), sizeof(int));
                    infile.read(reinterpret_cast<char*>(&current_value), sizeof(int));
                    infile.read(reinterpret_cast<char*>(&tombstone_flag_char), sizeof(char));

                    if (infile.fail() || infile.eof()) {
                        // Reached end of block unexpectedly or read error
                         if(!infile.eof()) std::cerr << "Warning: Read error or premature EOF within block during stats in file: " << filename << std::endl;
                        break; // Exit inner while loop, move to next block or file
                    }

                    physical_key_counts["LVL" + std::to_string(level_num)]++; // Count every physical key

                    // Check if key already has a newer version
                    if (processed_keys_for_logical_view.count(current_key)) {
                        continue; // Skip older versions/deleted markers
                    }

                    // This is the newest version encountered so far for this key
                    processed_keys_for_logical_view.insert(current_key);

                    bool current_tombstone = (tombstone_flag_char == 1);
                    if (!current_tombstone) {
                         logical_data[current_key] = {current_value, "L" + std::to_string(level_num)}; // Store value and location
                    }
                    // If it's a tombstone, mark key as processed but don't add to logical_data.
                } // while reading block
            } // for each index_entry (block) in file
            infile.close();
        } // for each file metadata in level
    } // for each level

    // --- Stage 2: Print the statistics based on collected data ---

    // (1) Logical Pair Count
    std::cout << "Logical Pairs: " << logical_data.size() << std::endl;

    // (2) Keys Per Level (Physical count including tombstones/stale data in files)
    // Print memtable first
    std::cout << "Memtable: " << physical_key_counts["Memtable"];
    // Print levels
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        std::cout << ", LVL" << i << ": " << physical_key_counts["LVL" + std::to_string(i)];
    }
    std::cout << std::endl;

    // (3) Dump Tree (Logical view: Key:Value:Level)
    // Group by level first for output format consistency
    std::map<int, std::vector<std::pair<int, int>>> entries_by_level; // Group for printing

     // Group by level first
    for(const auto& pair : logical_data) {
        int key = pair.first;
        int value = pair.second.first;
        std::string location = pair.second.second;
        int level_num = -1; // -1 for Memtable
        if(location == "M") {
            level_num = -1;
        } else {
             try {
                level_num = std::stoi(location.substr(1)); // Extract level number after "L"
             } catch(...) {
                  std::cerr << "Warning: Could not parse level number from location string: " << location << std::endl;
                  continue; // Skip this entry if location is malformed
             }
        }
        entries_by_level[level_num].push_back({key, value});
    }

    // Print Memtable entries first (Level -1 map key)
    bool first_group_printed = false;
    if(entries_by_level.count(-1)) {
        for(const auto& kv_pair : entries_by_level[-1]) {
             std::cout << kv_pair.first << ":" << kv_pair.second << ":M ";
        }
        first_group_printed = true;
        // No newline after memtable entries, space separates groups
    }


    // Print Level entries (Level 1 to MAX_LEVELS map keys)
     for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
         if (entries_by_level.count(level_num)) {
             if (first_group_printed) {
                  std::cout << " "; // Space before the next group
             }
             for (size_t i = 0; i < entries_by_level[level_num].size(); ++i) {
                 const auto& kv_pair = entries_by_level[level_num][i];
                 std::cout << kv_pair.first << ":" << kv_pair.second << ":L" << level_num << (i == entries_by_level[level_num].size() - 1 ? "" : " "); // Space between entries in a group
             }
             first_group_printed = true;
         }
     }
    // Final newline after all entries are printed
    std::cout << std::endl;


    std::cout << "----------------------" << std::endl;
}

// Explicit function to delete all SSTable files
void lsm_tree::cleanup_files() {
    std::cout << "Cleaning up ALL SSTable files and directories..." << std::endl;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
             // Delete files stored in memory metadata first
             delete_sst_files(levels_[i]->sstable_metadata_);
             levels_[i]->sstable_metadata_.clear(); // Clear the list in memory

            // Optionally remove the directory itself
            std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
             // Use rmdir to check if directory is empty before attempting to remove
             if (rmdir(level_dir.c_str()) != 0) {
                 if (errno != ENOENT) { // Ignore error if directory doesn't exist
                      std::cerr << "Warning: Could not remove directory " << level_dir << ": " << strerror(errno) << std::endl;
                 } else {
                     // std::cout << "Info: Directory already gone or never existed: " << level_dir << std::endl;
                 }
             } else {
                 // std::cout << "Removed directory: " << level_dir << std::endl;
             }
        }
    }
    // Optionally remove the root data directory
    if (rmdir(DATA_DIR.c_str()) != 0) {
         if (errno != ENOENT) { // Ignore error if directory doesn't exist
             std::cerr << "Warning: Could not remove root data directory " << DATA_DIR << ": " << strerror(errno) << std::endl;
         } else {
            //  std::cout << "Info: Root data directory already gone or never existed: " << DATA_DIR << std::endl;
         }
    } else {
        // std::cout << "Removed directory: " << DATA_DIR << std::endl;
    }

     // Reset run ID generator
     next_run_id_ = 0;
}