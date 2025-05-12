#include "lsm_tree.hh"
#include "bloom_filter.hh"
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
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <mutex>
#include <future>
#include <tuple>
#include <optional>

using namespace std;

// --- Define data directory constant ---
const std::string DATA_DIR = "data";

// --- Helper Functions ---
bool directory_exists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    return (info.st_mode & S_IFDIR) != 0;
}

bool create_directory(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    } else {
        if (errno == EEXIST && directory_exists(path)) {
            return true;
        }
        std::cerr << "Error creating directory " << path << ": " << strerror(errno) << std::endl;
        return false;
    }
}

SSTableInfo lsm_tree::rebuild_run_info(const std::string& filename) {
    std::vector<std::pair<int, long long>> fence_pointers;

    std::ifstream infile(filename);
    if (!infile) {
        std::cerr << "Warning: Could not open SSTable TXT file to rebuild run info: " << filename << std::endl;
        return {filename, {}, BloomFilter()}; // Return empty/invalid
    }

    std::string line;
    long long current_block_byte_count = 0;
    std::vector<int> keys_in_file; // To collect keys for Bloom Filter and count

    // First pass: Collect keys and build fence pointers
    infile.clear(); // Clear EOF flags etc.
    infile.seekg(0, std::ios::beg); // Rewind to the start

    while (true) {
        long long line_start_offset = infile.tellg();

        if (line_start_offset == -1) {
             std::cerr << "Warning: tellg() failed during run info rebuild pass 1 in " << filename << std::endl;
             infile.close();
             return {filename, {}, BloomFilter()};
        }

        std::string current_line;
        if (!std::getline(infile, current_line)) {
            if (!infile.eof() && infile.fail()) {
                 std::cerr << "Warning: Read error during run info rebuild pass 1 in " << filename << std::endl;
            }
            break; // Exit loop on EOF or read error
        }

        // Peek at the next position to calculate line size *including* newline
        long long line_end_offset = infile.tellg();
        long long line_byte_size = 0;
        if (line_end_offset != -1 && line_start_offset != -1) {
             line_byte_size = line_end_offset - line_start_offset;
        } else if (line_end_offset == -1) {
             // If tellg after getline failed, try adding size of line content + estimated newline size
             line_byte_size = current_line.length() + 1; // +1 for newline (rough estimate)
        }


        if (current_line.empty()) {
            current_block_byte_count += line_byte_size;
            continue;
        }

        std::stringstream ss(current_line);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
             keys_in_file.push_back(current_key);

             if (current_block_byte_count >= BLOCK_SIZE || fence_pointers.empty()) {
                 if (line_start_offset != -1) {
                    fence_pointers.push_back({current_key, line_start_offset});
                 } else {
                     // Fallback if tellg failed at the start of the line
                      fence_pointers.push_back({current_key, 0}); // Add with offset 0 as a fallback
                      std::cerr << "Warning: Using fallback offset 0 for fence pointer in " << filename << " due to tellg failure." << std::endl;
                 }
                 current_block_byte_count = line_byte_size;
             } else {
                 current_block_byte_count += line_byte_size;
             }
        } else {
             std::cerr << "Warning: Failed to parse line during run info rebuild (pass 1): '" << current_line << "' in file: " << filename << std::endl;
             current_block_byte_count += line_byte_size; // Still add bytes even if parse fails
        }
    } // End while(true) loop pass 1

    infile.close();

    // Now initialize Bloom Filter with the actual count of keys found
    BloomFilter filter(keys_in_file.size(), BLOOM_FILTER_FALSE_POSITIVE_RATE);

    // Re-open file for second pass to populate the Bloom Filter
    std::ifstream infile_pass2(filename);
    if (!infile_pass2) {
         std::cerr << "Error: Could not re-open SSTable TXT file for rebuild pass 2: " << filename << std::endl;
         // Return SSTableInfo with collected fence pointers but potentially empty filter
         return {filename, fence_pointers, BloomFilter()};
    }

    std::string line_pass2;
    size_t keys_added_to_filter = 0;

    while (std::getline(infile_pass2, line_pass2)) {
        if (line_pass2.empty()) continue;

        std::stringstream ss(line_pass2);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
            filter.add(current_key);
            keys_added_to_filter++;
        } else {
             std::cerr << "Warning: Failed to parse line during run info rebuild (pass 2): '" << line_pass2 << "' in file: " << filename << std::endl;
        }
    }
     if (!infile_pass2.eof() && infile_pass2.fail()) {
          std::cerr << "Warning: Read error or parsing issue near EOF during rebuild (pass 2) in SSTable TXT file: " << filename << std::endl;
     }

    infile_pass2.close();

    // Optional sanity check
    if (keys_added_to_filter != keys_in_file.size()) {
         std::cerr << "Warning: Key count mismatch during rebuild for " << filename << ". Pass 1 found " << keys_in_file.size() << ", Pass 2 added " << keys_added_to_filter << " to filter." << std::endl;
    }

    return {filename, fence_pointers, std::move(filter)};
}

// --- Helper Struct for Merge ---
struct merge_entry {
    key_value kv;
    size_t stream_index; // Which input file this entry came from

    // Custom comparator for min-heap (priority queue) based on key
    bool operator>(const merge_entry& other) const {
        if (kv.key > other.kv.key) {
            return true;
        }
        if (kv.key < other.kv.key) {
            return false;
        }
        // For same key, prioritize smaller stream_index (older run) so the newest version (from largest stream_index among duplicates) is processed last among the duplicates.
        // This ensures min_heap.top() for a given key is always the newest version.
        return stream_index < other.stream_index; // Newest run index is larger
    }
};


// --- Level Class Implementation ---
level::level(int capacity, int curr_level) : capacity_(capacity), curr_level_(curr_level) {
    // sstable_runs_ is already default-initialized (empty vector)
}

level::~level() {
    // SSTableInfo vector member `sstable_runs_` will be destroyed automatically.
    // Its contained BloomFilter objects will also be destroyed automatically.
    // The files themselves are NOT deleted here.
}

// Assumes caller holds level_mutex_
void level::add_run(SSTableInfo&& info) {
    // std::cerr << "DEBUG ADD_RUN L" << curr_level_ << ": Adding run " << info.filename << std::endl; // Debug
    sstable_runs_.push_back(std::move(info));
    // runs are added oldest-first by the constructor loading and merge_runs logic.
    // find_key searches rbegin() (newest first).
}

// Assumes caller holds level_mutex_ before calling
// Search key in this level's SSTables (files) using Bloom filters and fence pointers
bool level::find_key(int key, int& value, bool& is_tombstone) const {
    // Iterate through runs in reverse order (newest first)
    for (auto it = sstable_runs_.rbegin(); it != sstable_runs_.rend(); ++it) {
        const SSTableInfo& run_info = *it;
        const std::string& filename = run_info.filename;
        const auto& fence_pointers = run_info.fence_pointers;
        const auto& filter = run_info.filter;

        // --- Bloom Filter Check ---
        if (!filter.contains(key)) {
            continue; // Key definitely not in this file, skip reading it
        }
        // --- End Bloom Filter Check ---

        // --- Use Fence Pointers to find potential block ---
        long long search_offset = 0;

        if (!fence_pointers.empty()) {
            auto fp_it = std::lower_bound(fence_pointers.begin(), fence_pointers.end(), key,
                                         [](const std::pair<int, long long>& fp, int target_key){
                                             return fp.first < target_key;
                                         });

            if (fp_it != fence_pointers.begin()) {
                --fp_it;
                search_offset = fp_it->second;
            } else {
                 // Key is <= the first fence pointer's key. Search from the beginning (offset 0).
                 // The first fence pointer's offset should typically be 0 anyway, but handle explicitly.
                 search_offset = 0;
            }
        } else {
             search_offset = 0; // No fence pointers, must search from start
        }
        // --- End Fence Pointer Logic ---


        // Open the file and seek to the calculated offset
        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Warning: Could not open SSTable TXT file for reading in find_key: " << filename << std::endl;
            continue;
        }

        // Basic check for negative or out-of-bounds offset
         if (search_offset < 0) search_offset = 0; // Should not happen with tellg >= 0

        infile.seekg(search_offset);
        if (infile.fail()) {
            std::cerr << "Warning: Failed to seek to offset " << search_offset << " in file " << filename << " during find_key. Skipping file." << std::endl;
            infile.close();
            continue;
        }

        std::string line;
        int current_key;
        int current_value;
        int tombstone_flag;

        // Read line by line from the seeked position
        while (std::getline(infile, line)) {
            if (line.empty()) continue;

            std::stringstream ss(line);

            if (ss >> current_key >> current_value >> tombstone_flag) {
                if (current_key == key) {
                    value = current_value;
                    is_tombstone = (tombstone_flag == 1);
                    infile.close();
                    return true; // Found the key in the newest relevant file
                }
                 // Optimization: If we passed the target key, it means the key isn't in this block or later in this file.
                 if (current_key > key) {
                     break; // Key cannot be found further in this file (sorted)
                 }
            } else {
                 std::cerr << "Warning: Parsing error during find_key in file: " << filename << ", line: " << line << std::endl;
            }
        }

        if (!infile.eof() && infile.fail()) {
             std::cerr << "Warning: Read error or parsing issue near EOF in SSTable TXT file: " << filename << " during find_key." << std::endl;
        }

        infile.close();

    }

    return false; // Key not found in any run in this level
}

// Helper specifically for parallel search to return optional<tuple<...>>
// Acquires and releases its own lock internally.
std::optional<std::tuple<int, int, bool, int>> level::find_key_parallel(int key) const {
    // Acquire the level's lock for searching its runs
    std::lock_guard<std::mutex> lock(level_mutex_); // Mutex lock within the parallel task

    int value;
    bool is_tombstone;
    // Use the existing find_key logic (which assumes lock held)
    if (find_key(key, value, is_tombstone)) {
        // Found the key in *some* run in *this* level.
        // This level provides the newest version found *among its own runs*
        // because level::find_key iterates runs newest-first.
        return std::make_optional(std::make_tuple(key, value, is_tombstone, curr_level_));
    }
    return std::nullopt; // Key not found in this level
}

// ********** MISSING DEFINITION ADDED **********
// Assumes caller holds level_mutex_
std::vector<std::string> level::get_run_filenames() const {
    // Caller holds lock
    std::vector<std::string> filenames;
    filenames.reserve(sstable_runs_.size());
    for(const auto& info : sstable_runs_) {
        filenames.push_back(info.filename);
    }
    return filenames;
}

// ********** MISSING DEFINITION ADDED **********
// Assumes caller holds level_mutex_
void level::clear_runs() {
    // Caller holds lock
    sstable_runs_.clear(); // Clears the vector, destroying SSTableInfo objects and their filters
}


// --- Memtable Class Implementation ---
memtable::memtable() {
    memtable_.reserve(MEMTABLE_CAPACITY);
}

bool memtable::insert(key_value kv_pair) {
    std::lock_guard<std::mutex> lock(memtable_mutex_);
    for (int i = 0; i < curr_size_; ++i) {
        if (memtable_[i].key == kv_pair.key) {
            memtable_[i].value = kv_pair.value;
            memtable_[i].tombstone = kv_pair.tombstone;
            return true;
        }
    }

    if (is_full()) {
       return false;
    }

    memtable_.push_back(kv_pair);
    ++curr_size_;
    return true;
}

std::vector<key_value> memtable::flush() {
    std::lock_guard<std::mutex> lock(memtable_mutex_);
    std::sort(memtable_.begin(), memtable_.end());
    std::vector<key_value> data_to_flush = memtable_;
    memtable_.clear();
    memtable_.reserve(MEMTABLE_CAPACITY);
    curr_size_ = 0;
    return data_to_flush;
}

bool memtable::find_key(int key, int& value, bool& is_tombstone) {
    std::lock_guard<std::mutex> lock(memtable_mutex_);
    // Search in reverse order (newest entries added last)
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
lsm_tree::lsm_tree() : next_run_id_(0) {
    memtable_ptr_ = new memtable();
    levels_.resize(MAX_LEVELS + 1, nullptr); // levels_[0] unused

    // 1. Create root data directory
    if (!create_directory(DATA_DIR)) {
        throw std::runtime_error("Failed to create or access data directory: " + DATA_DIR);
    }

    long long max_run_id_found = -1; // To track the highest run ID for generating new names

    // 2. Create levels and load existing SSTables in order, rebuilding run info
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        // Set capacity based on level number (exponentially increasing)
        // Note: Tiering doesn't use hard capacities like this, but we keep it as a concept.
        levels_[i] = new level(INITIAL_LEVEL_CAPACITY * static_cast<int>(std::pow(SIZE_RATIO, i-1)), i);
        if (i > 1) {
            levels_[i-1]->next_ = levels_[i];
        }

        // Create level subdirectory
        std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
        if (!create_directory(level_dir)) {
             throw std::runtime_error("Failed to create or access level directory: " + level_dir);
        }

        // --- Collect, Sort, and Load existing files for this level ---
        std::vector<std::string> found_sstable_paths;

        DIR *dirp = opendir(level_dir.c_str());
        if (dirp) {
            struct dirent *dp;
            while ((dp = readdir(dirp)) != nullptr) {
                std::string filename = dp->d_name;
                std::string full_path = level_dir + "/" + filename;

                struct stat file_stat;
                if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode))
                {
                     if (filename.length() > SST_FILE_SUFFIX.length() &&
                         filename.substr(filename.length() - SST_FILE_SUFFIX.length()) == SST_FILE_SUFFIX &&
                         filename.rfind(SST_FILE_PREFIX) == 0) // Check if it starts with prefix
                    {
                        // Found a potential SSTable file
                        found_sstable_paths.push_back(full_path);

                        // Also update max_run_id_found
                        size_t prefix_len = SST_FILE_PREFIX.length();
                        size_t suffix_len = SST_FILE_SUFFIX.length();
                        size_t sst_pos = filename.length() - suffix_len;

                        if (sst_pos > prefix_len) {
                             try {
                                 long long run_id = std::stoll(filename.substr(prefix_len, sst_pos - prefix_len));
                                 if (run_id > max_run_id_found) {
                                     max_run_id_found = run_id;
                                 }
                             } catch (...) {
                                  std::cerr << "Warning: Could not parse run ID from filename for max_run_id_found: " << filename << std::endl;
                             }
                        }
                    }
                }
            }
            closedir(dirp);
        } else {
             std::cerr << "Warning: Could not open level directory for reading: " << level_dir << std::endl;
        }

        // Sort the collected file paths based on the run ID (oldest first)
        // This ensures levels_[i]->sstable_runs_ is populated in chronological order (run_0, run_1, ...).
        std::sort(found_sstable_paths.begin(), found_sstable_paths.end(),
                  [](const std::string& a, const std::string& b) {
                      size_t prefix_len = SST_FILE_PREFIX.length();
                      size_t suffix_len = SST_FILE_SUFFIX.length();

                      // Ensure both paths have the expected format
                      if (a.rfind(SST_FILE_PREFIX) != 0 || a.length() < prefix_len + suffix_len ||
                          b.rfind(SST_FILE_PREFIX) != 0 || b.length() < prefix_len + suffix_len ||
                          a.substr(a.length() - suffix_len) != SST_FILE_SUFFIX ||
                          b.substr(b.length() - suffix_len) != SST_FILE_SUFFIX) {
                           // Fallback to string compare if format is unexpected s
                           return a < b;
                      }

                      try {
                          // Extract and convert run IDs
                          long long run_id_a = std::stoll(a.substr(prefix_len, a.length() - prefix_len - suffix_len));
                          long long run_id_b = std::stoll(b.substr(prefix_len, b.length() - prefix_len - suffix_len));

                          return run_id_a < run_id_b; // Sort by run ID ascending (oldest first)
                      } catch (...) {
                          std::cerr << "Warning: Exception parsing run ID for sorting (fallback to string compare): " << a << " or " << b << std::endl;
                          return a < b; // Fallback on exception
                      }
                  });

        // Now rebuild info (including BF and FP) and add runs to the level in sorted order
        for(const auto& full_path : found_sstable_paths) {
             SSTableInfo loaded_run_info = rebuild_run_info(full_path);

             if (!loaded_run_info.filename.empty()) {
                 // Need to lock L<i> to add the run during constructor load
                 // Constructor runs single-threaded, so locking isn't strictly necessary *for thread safety*
                 // during the constructor itself, but it's good practice if this logic were moved.
                 levels_[i]->add_run(std::move(loaded_run_info));
             } else {
                 std::cerr << "Error: Failed to rebuild run info for " << full_path << " during load. Skipping." << std::endl;
             }
        }
        // After this loop, levels_[i]->sstable_runs_ contains SSTableInfo objects sorted by run ID (oldest first).
        // Iterating levels_[i]->sstable_runs_.rbegin() will correctly process newer runs first.
    }

     // Set the next run ID to be one greater than the highest found
    next_run_id_ = max_run_id_found + 1;
     // std::cout << "DEBUG: Initialized next_run_id_ to " << next_run_id_ << std::endl; // Debug
}

lsm_tree::~lsm_tree() {
    // Perform final flush on shutdown
    // std::cout << "DEBUG: Shutting down LSM Tree..." << std::endl; // Debug
    if (memtable_ptr_ && memtable_ptr_->curr_size_ > 0) {
        // std::cout << "DEBUG: Final memtable flush..." << std::endl; // Debug
        std::vector<key_value> data_to_flush = memtable_ptr_->flush(); // flush acquires memtable lock
        if (!data_to_flush.empty()) {
            std::string final_sstable_file = generate_sstable_filename(1); // acquires id_mutex_
            SSTableInfo final_run_info = write_sstable(data_to_flush, final_sstable_file, data_to_flush.size());
            if (!final_run_info.filename.empty()) {
                 if (levels_.size() > 1 && levels_[1]) {
                     // Need to lock L1 to add the run
                     std::lock_guard<std::mutex> l1_lock(levels_[1]->level_mutex_);
                      levels_[1]->add_run(std::move(final_run_info)); // add_run assumes lock held
                      // std::cout << "DEBUG: Added final flush run " << final_run_info.filename << " to L1." << std::endl; // Debug
                 } else {
                     std::cerr << "Error: Level 1 not available during final flush cleanup." << std::endl;
                 }
            } else {
                 std::cerr << "Error: Failed to write final memtable flush to disk during shutdown!" << std::endl;
            }
        }
    }

    // Clean up level pointers
    // std::cout << "DEBUG: Deleting memtable and levels..." << std::endl; // Debug
    delete memtable_ptr_;
    memtable_ptr_ = nullptr; // Avoid double delete
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
             // The level destructor handles deleting the sstable_runs_ vector and its contents.
             delete levels_[i];
             levels_[i] = nullptr; // Avoid double delete
        }
    }
    // Physical files remain unless cleanup_files is called separately before deletion.
     // std::cout << "DEBUG: LSM Tree shutdown complete." << std::endl; // Debug
}

std::string lsm_tree::generate_sstable_filename(int level_num) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    std::string level_dir = DATA_DIR + "/L" + std::to_string(level_num);
    return level_dir + "/" + SST_FILE_PREFIX + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

SSTableInfo lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename, size_t estimated_n_for_filter) {
    SSTableInfo run_info;
    run_info.filename = filename;
    // If data is empty, estimated_n_for_filter might be 0, which is fine for BloomFilter.
    run_info.filter = BloomFilter(estimated_n_for_filter, BLOOM_FILTER_FALSE_POSITIVE_RATE);

    std::ofstream outfile(filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open SSTable TXT file for writing: " << filename << std::endl;
        return {"", {}, {}}; // Indicate failure
    }

    long long current_block_byte_count = 0;

    for (size_t i = 0; i < data.size(); ++i) {
        const auto& kv = data[i];

        run_info.filter.add(kv.key);

        long long entry_start_offset = outfile.tellp();
         if (entry_start_offset == -1) {
             std::cerr << "Warning: tellp() failed before writing entry to " << filename << ". Fence pointers might be inaccurate." << std::endl;
         }

        outfile << kv.key << " " << kv.value << " " << (kv.tombstone ? 1 : 0) << "\n";

        if (!outfile) {
             std::cerr << "Error: Failed to write entry (" << kv.key << ") to SSTable TXT file: " << filename << std::endl;
             outfile.close();
             // Attempt cleanup of partially written file
             std::remove(filename.c_str());
             return {"", {}, {}}; // Indicate failure
        }

        long long entry_end_offset = outfile.tellp();
        long long line_byte_size = 0;
        if (entry_end_offset != -1 && entry_start_offset != -1) {
             line_byte_size = entry_end_offset - entry_start_offset;
        } else if (entry_end_offset == -1) {
             // Fallback if tellp failed after writing
             line_byte_size = std::to_string(kv.key).length() + std::to_string(kv.value).length() + 3; // Rough estimate: key, value, tombstone, 2 spaces, 1 newline
             std::cerr << "Warning: Using estimated line size for fence pointer calculation in " << filename << " due to tellp failure." << std::endl;
        }

        // --- Corrected Fence Pointer Logic ---
        long long offset_to_add;
        if (entry_start_offset != -1) {
             offset_to_add = entry_start_offset;
        } else {
             // If we couldn't get the precise start offset, fall back to 0.
             // This is less accurate for block size calculation but safe.
             std::cerr << "Warning: Skipped adding precise fence pointer for key " << kv.key << " in " << filename << " due to failed tellp() at start of line. Using offset 0 as fallback." << std::endl;
             offset_to_add = 0;
        }

        // Add fence pointer if block condition met, using the determined offset
        if (current_block_byte_count >= BLOCK_SIZE || run_info.fence_pointers.empty()) {
             run_info.fence_pointers.push_back({kv.key, offset_to_add});
             current_block_byte_count = line_byte_size;
        } else {
            current_block_byte_count += line_byte_size;
        }
        // --- End Corrected Fence Pointer Logic ---
    }

    outfile.close();
    if (!outfile) {
        std::cerr << "Error: Failed to close SSTable TXT file properly: " << filename << std::endl;
         // The file might exist but be incomplete or corrupted.
         // Consider deleting it or warning loudly. Let's return as failure.
         return {"", {}, {}};
    }
    return run_info; // Return the successfully created SSTableInfo
}

void lsm_tree::delete_sst_files(const std::vector<std::string>& filenames) {
    std::lock_guard<std::mutex> delete_lock(file_delete_mutex_); // Acquire mutex for file deletion
    for (const auto& filename : filenames) {
        // std::cerr << "DEBUG DELETE: - Attempting delete: " << filename << std::endl; // Debug
        if (std::remove(filename.c_str()) != 0) {
            // It's OK if the file doesn't exist (ENOENT) - another thread might have deleted it.
            // Other errors (like permissions) should still be reported.
            if (errno != ENOENT) { // ENOENT is "No such file or directory"
                 std::cerr << "ERROR DELETE: Could not delete SSTable file: " << filename << " (" << strerror(errno) << ")" << std::endl;
            } else {
                 // std::cerr << "DEBUG DELETE: - File already deleted: " << filename << std::endl; // Debug
            }
        } else {
             // std::cerr << "DEBUG DELETE: - Successfully deleted: " << filename << std::endl; // Debug
        }
    }
}


SSTableInfo lsm_tree::merge_runs(int target_level_num, const std::vector<SSTableInfo>& runs_to_merge_info, size_t estimated_n_for_filter) {

    if (runs_to_merge_info.empty()) {
        return {"", {}, {}};
    }

    // std::cerr << "DEBUG MERGE: == Starting merge into level " << target_level_num << ". Merging " << runs_to_merge_info.size() << " runs. ==" << std::endl; // Debug

    // Input streams must be kept in the order of run_to_merge_info indices (oldest first)
    std::vector<std::ifstream> input_streams;
    input_streams.reserve(runs_to_merge_info.size());

    // Use min-heap to get smallest key, prioritizing newest version (larger stream_index) for duplicates
    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input TXT files and read the FIRST entry from EACH
    for (size_t i = 0; i < runs_to_merge_info.size(); ++i) {
        const std::string& filename = runs_to_merge_info[i].filename;
        input_streams.emplace_back(filename); // Open stream

        if (!input_streams.back()) {
            std::cerr << "Error: Could not open TXT file for merge: " << filename << std::endl;
            // Attempt to close already opened streams before returning
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
            return {"", {}, {}}; // Indicate failure
        }

        std::string line;
        if (std::getline(input_streams.back(), line)) {
             if (!line.empty()) {
                 std::stringstream ss(line);
                 int current_key, current_value, tombstone_flag;
                 if (ss >> current_key >> current_value >> tombstone_flag) {
                    min_heap.push({{current_key, current_value, (tombstone_flag == 1)}, i}); // Use index i as stream_index
                 } else {
                      std::cerr << "Warning: Failed to parse initial line correctly in merge file: " << filename << ", line: '" << line << "'" << std::endl;
                 }
             }
        } else {
             if (!input_streams.back().eof() && input_streams.back().fail()) {
                 std::cerr << "Warning: Failed initial read from TXT file: " << filename << std::endl;
             }
             // File is empty or failed, stream won't be pushed to heap
        }
    }


    // Generate output filename *after* opening inputs successfully
    std::string output_filename = generate_sstable_filename(target_level_num); // acquires id_mutex_
    std::ofstream outfile(output_filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open output TXT file for merge: " << output_filename << std::endl;
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return {"", {}, {}}; // Indicate failure
    }

    SSTableInfo merged_run_info;
    merged_run_info.filename = output_filename;
    // Use a heuristic estimated_n for the Bloom filter in the merged run.
    // A more precise estimate would count keys during the merge, but that adds complexity.
    merged_run_info.filter = BloomFilter(estimated_n_for_filter, BLOOM_FILTER_FALSE_POSITIVE_RATE);

    long long current_block_byte_count = 0;

    // Merge process
    while (!min_heap.empty()) {
        // Get the top element - this is the smallest key overall.
        // Due to the comparator, if duplicates exist for this key, this is the newest version.
        merge_entry current_newest_for_key = min_heap.top();
        min_heap.pop();

        int current_key = current_newest_for_key.kv.key;

        // Discard any remaining elements at the top of the heap with the SAME KEY.
        // These are older duplicates. We need to advance their streams.
        std::vector<size_t> streams_to_advance;
        streams_to_advance.push_back(current_newest_for_key.stream_index); // The newest version's stream also needs advancing

        while (!min_heap.empty() && min_heap.top().kv.key == current_key) {
            merge_entry older_version = min_heap.top();
            min_heap.pop();
            streams_to_advance.push_back(older_version.stream_index); // Collect stream index
        }

        // --- Process the newest version: Write if not tombstone. ---
        if (!current_newest_for_key.kv.tombstone) {
             merged_run_info.filter.add(current_newest_for_key.kv.key);

             long long entry_start_offset = outfile.tellp();
              if (entry_start_offset == -1) { std::cerr << "Warning: tellp() failed before writing entry to merged file " << output_filename << ". Fence pointers might be inaccurate." << std::endl; }
             outfile << current_newest_for_key.kv.key << " " << current_newest_for_key.kv.value << " " << (current_newest_for_key.kv.tombstone ? 1 : 0) << "\n";

             if (!outfile) {
                 std::cerr << "Error writing during merge to TXT file: " << output_filename << std::endl;
                  outfile.close(); for(auto& stream : input_streams) if(stream.is_open()) stream.close(); std::remove(output_filename.c_str()); return {"", {}, {}}; // Indicate failure
             }

             long long entry_end_offset = outfile.tellp();
             long long line_byte_size = 0;
             if (entry_end_offset != -1 && entry_start_offset != -1) { line_byte_size = entry_end_offset - entry_start_offset; }
              else if (entry_end_offset == -1) { line_byte_size = std::to_string(current_newest_for_key.kv.key).length() + std::to_string(current_newest_for_key.kv.value).length() + 3; } // Rough estimate

             // --- Corrected Fence Pointer Logic ---
             long long offset_to_add;
             if (entry_start_offset != -1) {
                  offset_to_add = entry_start_offset;
             } else {
                  // If we couldn't get the precise start offset, fall back to 0.
                  std::cerr << "Warning: Skipped adding precise fence pointer for key " << current_newest_for_key.kv.key << " in " << output_filename << " due to failed tellp() at start of line. Using offset 0 as fallback." << std::endl;
                  offset_to_add = 0;
             }

             if (current_block_byte_count >= BLOCK_SIZE || merged_run_info.fence_pointers.empty()) {
                  merged_run_info.fence_pointers.push_back({current_newest_for_key.kv.key, offset_to_add});
                  current_block_byte_count = line_byte_size;
             } else {
                 current_block_byte_count += line_byte_size;
             }
             // --- End Corrected Fence Pointer Logic ---

        } // else: it was a tombstone, we discard it

        // --- Advance streams for all versions collected in this batch ---
        for (size_t stream_idx : streams_to_advance) {
            if (input_streams[stream_idx].is_open()) {
                 std::string next_line;
                 if (std::getline(input_streams[stream_idx], next_line)) {
                     if (!next_line.empty()) {
                         std::stringstream ss(next_line);
                         int next_key, next_value, next_tombstone_flag;
                         if (ss >> next_key >> next_value >> next_tombstone_flag) {
                             min_heap.push({{next_key, next_value, (next_tombstone_flag == 1)}, stream_idx});
                         } else {
                              std::cerr << "Warning: Failed to parse line after advancing stream " << stream_idx << " (file: " << runs_to_merge_info[stream_idx].filename << ") for key " << current_key << ": '" << next_line << "'" << std::endl;
                         }
                     } // else: read an empty line, stream is advanced, nothing to push
                 } else {
                      // getline failed (EOF or read error)
                     if (!input_streams[stream_idx].eof() && input_streams[stream_idx].fail()) {
                         std::cerr << "Warning: Failed read from TXT file : " << runs_to_merge_info[stream_idx].filename << std::endl;
                     }
                     input_streams[stream_idx].close(); // Close stream when exhausted or failed
                 }
            }
        }
    } // End while(!min_heap.empty())

    // Close output file
    outfile.close();
    if (!outfile) {
        std::cerr << "Error closing merged output TXT file properly: " << output_filename << std::endl;
         std::remove(output_filename.c_str()); // Delete potentially incomplete file
         return {"", {}, {}}; // Indicate failure
    }

    // Close any remaining input streams (should all be closed by now if successful)
    for (auto& stream : input_streams) {
        if (stream.is_open()) {
             std::cerr << "Warning: Input stream was still open after merge loop. Closing." << std::endl;
             stream.close();
        }
    }

    return merged_run_info; // Return info for the successful merge
}

// Function to check and trigger merges starting from a level
void lsm_tree::check_and_trigger_merge(int level_num) {
    if (level_num < 1 || level_num > MAX_LEVELS) {
        return;
    }

    level* current_level = levels_[level_num];
    // Target level can be MAX_LEVELS even if source is MAX_LEVELS
    int target_level_num = std::min(level_num + 1, MAX_LEVELS);
    level* target_level = levels_[target_level_num];

    if (!target_level) {
        std::cerr << "Critical Error: Target level " << target_level_num << " for merge from L" << level_num << " is unexpectedly null. Cannot merge." << std::endl;
        return;
    }

    // --- Phase 1: Determine merge needs, identify files, clear source level ---
    std::vector<SSTableInfo> runs_to_merge_info;
    std::vector<std::string> files_to_delete;

    { // Scoped lock for the current (source) level
        std::lock_guard<std::mutex> current_level_lock(current_level->level_mutex_);

        // Check merge condition *while holding the lock*
        if (current_level->sstable_runs_.size() < SIZE_RATIO) {
            return; // Locks automatically released
        }

        // Copy runs to merge info and filenames while holding the lock
        runs_to_merge_info.reserve(current_level->sstable_runs_.size());
        files_to_delete.reserve(current_level->sstable_runs_.size());
        for(const auto& info : current_level->sstable_runs_) {
            runs_to_merge_info.push_back(info);
            files_to_delete.push_back(info.filename);
        }

        // Clear the runs from the current level *now* while the lock is held.
        current_level->clear_runs(); // Now calls the added definition
    } // current_level_lock releases mutex

    // --- Phase 2: Perform the merge (File I/O) ---
    // This is done without holding level locks.
    // Using SIZE_RATIO * MEMTABLE_CAPACITY as a heuristic for merged run size estimate.
    // A more accurate estimate would sum the sizes of the runs being merged.
    size_t estimated_n = BLOOM_FILTER_ESTIMATED_N_MERGE; // Use constant as defined

    SSTableInfo merged_run_info = merge_runs(target_level_num, runs_to_merge_info, estimated_n);

    // --- Phase 3: Add the new merged run to the target level ---
    if (!merged_run_info.filename.empty()) {
         { // Scoped lock for the target level
              std::lock_guard<std::mutex> target_level_lock(target_level->level_mutex_);
              levels_[target_level_num]->add_run(std::move(merged_run_info));
         } // target_level_lock releases mutex

         // --- Phase 4: Recursively check if the *next* level now needs merging ---
         if (target_level_num < MAX_LEVELS) { // Can only trigger a merge *from* levels 1 to MAX_LEVELS-1
              check_and_trigger_merge(target_level_num);
         } else {
             // std::cerr << "DEBUG MERGE TRIGGER: Target level " << target_level_num << " is MAX_LEVELS. No further merge triggered." << std::endl; // Debug
         }

    } else {
        std::cerr << "Error: Merge failed for level " << level_num << ". Output file not created. Files from L" << level_num << " were cleared from memory but NOT deleted from disk. Manual cleanup may be required." << std::endl;
    }

    // --- Phase 5: Delete old files ---
    delete_sst_files(files_to_delete); // uses file_delete_mutex_
}

// Internal search function used by both public get and range
std::optional<key_value> lsm_tree::getValueForKey(int key) const {
    int value;
    bool is_tombstone;

    // 1. Check Memtable first (sequentially, fastest path)
    // memtable::find_key handles its own lock.
    if (memtable_ptr_->find_key(key, value, is_tombstone)) {
        if (is_tombstone) {
            return std::nullopt; // Found tombstone in memtable - definitive
        }
        return std::make_optional<key_value>({key, value, false}); // Found valid entry in memtable - definitive
    }

    // 2. If not in memtable, search levels in parallel
    std::vector<std::pair<int, std::future<std::optional<std::tuple<int, int, bool, int>>>>> level_futures;
    level_futures.reserve(MAX_LEVELS); // Reserve space

    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
             // Launch async task for each level's parallel search helper
             level_futures.push_back({i,
                 std::async(std::launch::async, &level::find_key_parallel, levels_[i], key)
             });
        }
    }

    // Collect results from levels, prioritizing lower level numbers (newer data)
    std::optional<key_value> newest_found_kv = std::nullopt;
    int newest_level_num = MAX_LEVELS + 1; // Initialize with a value higher than any valid level

    for (auto& pair : level_futures) {
        // int level_num = pair.first; // ********** UNUSED VARIABLE REMOVED **********
        // .get() waits for the future to complete and retrieves its result
        std::optional<std::tuple<int, int, bool, int>> result = pair.second.get();

        if (result.has_value()) {
             auto [res_key, res_value, res_tombstone, res_level_num] = result.value();

             // Since level::find_key_parallel returns the *newest* entry within *that* level,
             // we just need the newest among the results returned by the parallel tasks.
             // The lower the level_num, the newer the data.
             if (res_level_num < newest_level_num) {
                  newest_level_num = res_level_num;
                  newest_found_kv = std::make_optional<key_value>({res_key, res_value, res_tombstone});
                  // We found the newest version across all levels.
                  // If it's not a tombstone, we can stop processing other results,
                  // but waiting for all futures is often simpler cleanup.
                  // The logic below the loop handles which result to return.
             }
        }
    }

    // Determine final result based on the newest entry found in levels
    if (newest_found_kv.has_value()) {
         if (newest_found_kv.value().tombstone) {
              return std::nullopt; // Found a tombstone at the newest level
         } else {
              return newest_found_kv; // Found a valid non-tombstone entry at the newest level
         }
    } else {
         // Key not found in memtable or any level
         return std::nullopt;
    }
}


// --- Public Interface Implementation ---

bool lsm_tree::insert(key_value kv_pair) {
    // std::cerr << "DEBUG INSERT: Inserting key " << kv_pair.key << std::endl; // Debug
    // Try inserting into memtable
    if (!memtable_ptr_->insert(kv_pair)) { // insert acquires memtable_mutex_
        // Memtable is full, need to flush it
        // std::cout << "Memtable full. Flushing to Level 1..." << std::endl; // Debug output

        // Flush the memtable (acquires memtable_mutex_)
        std::vector<key_value> data_to_flush = memtable_ptr_->flush();

        if (!data_to_flush.empty()) {
            // Generate a filename for the new run in Level 1 (acquires id_mutex_)
            std::string new_sstable_file = generate_sstable_filename(1);

            // Write the flushed data to the new SSTable file and get its info (including filter)
            SSTableInfo new_run_info = write_sstable(data_to_flush, new_sstable_file, data_to_flush.size());

            if (!new_run_info.filename.empty()) { // Check if write was successful
                // Add the new run (info) to Level 1
                if (levels_.size() > 1 && levels_[1]) {
                    std::lock_guard<std::mutex> l1_lock(levels_[1]->level_mutex_);
                    levels_[1]->add_run(std::move(new_run_info)); // add_run assumes lock held
                    // std::cerr << "DEBUG INSERT: Added new run " << new_run_info.filename << " to L1." << std::endl; // Debug
                } else {
                    std::cerr << "Critical Error: Level 1 is null or levels_ vector too small. Cannot add flushed run." << std::endl;
                    // Data loss: file exists but isn't tracked.
                }

                // Check if Level 1 needs merging now
                // This call will acquire its own locks internally.
                check_and_trigger_merge(1);

            } else {
                std::cerr << "Error: Failed to write flushed memtable to disk. Data potentially lost." << std::endl;
                return false; // Indicate failure
            }
         } else {
             // Memtable was full but flush yielded no data. This is unexpected.
             std::cerr << "Warning: Memtable was full, but flush returned empty data. Re-attempting insert." << std::endl;
             // The memtable is now supposedly empty or smaller. Retry the insert.
             return memtable_ptr_->insert(kv_pair); // insert acquires memtable_mutex_
         }

        // The memtable was flushed, now insert the original kv_pair which previously failed due to fullness.
        // The memtable is now (or should be) empty.
        return memtable_ptr_->insert(kv_pair); // insert acquires memtable_mutex_
    }
    // Insert successful (directly into memtable on first try)
    return true;
}

// Public get function using the internal parallel search helper
int lsm_tree::get(int key, std::ostream& os) {
    // Use the internal helper to get the key's value
    std::optional<key_value> result = getValueForKey(key); // Performs parallel search

    // Lock the output stream to print the result
    std::lock_guard<std::mutex> cout_lock(cout_mutex_);

    if (result.has_value()) {
        const auto& kv = result.value();
        // getValueForKey only returns non-tombstone entries if found
        os << kv.value << std::endl;
        return kv.value;
    } else {
        // Key not found or found as a tombstone
        os << std::endl;
        return -1;
    }
}

// Range function using parallel get calls (via getValueForKey)
void lsm_tree::range(int start, int end, std::ostream& os) {
    // Launch parallel tasks for each key in the range
    std::vector<std::pair<int, std::future<std::optional<key_value>>>> futures;
    futures.reserve(end - start + 1);

    for (int k = start; k <= end; ++k) {
         // Launch async task to get the value for key 'k'
        futures.push_back({k, std::async(std::launch::async, &lsm_tree::getValueForKey, this, k)});
    }

    // Collect results into a map to keep them sorted by key
    std::map<int, key_value> found_pairs;
    for (auto& pair : futures) {
        int key = pair.first;
        // .get() waits for the future and retrieves the result
        std::optional<key_value> result = pair.second.get();
        if (result.has_value()) {
            // getValueForKey only returns non-tombstone valid entries
            found_pairs[key] = result.value();
        }
    }

    // Lock the output stream for printing the entire range result
    std::lock_guard<std::mutex> cout_lock(cout_mutex_);

    os << "Range (" << start << " to " << end << "): ";
    // Iterate through the sorted map and print the found key-value pairs
    for (const auto& pair : found_pairs) {
        os << pair.first << ":" << pair.second.value << " ";
    }
    os << std::endl;
}

void lsm_tree::delete_key(int key) {
    // Insert a tombstone entry for the key.
    // This relies on the insert function's locking and flushing/merging logic.
    insert({key, 0, true});
}

// printStats remains largely the same, using sequential locking for consistency snapshot (of the metadata lists)
void lsm_tree::printStats(std::ostream& os) const {
    std::lock_guard<std::mutex> cout_lock(cout_mutex_); // Lock for the entire stats output
    os << "--- LSM Tree Stats ---" << std::endl;

    // Data structures to hold intermediate results
    std::map<int, std::pair<int, std::string>> logical_data; // Map<key, Pair<value, location>>
    std::set<int> deleted_keys;                             // Keep track of keys confirmed deleted
    std::vector<long long> physical_key_counts(MAX_LEVELS + 1, 0); // Count all keys per level file

    // --- Stage 1: Process data from newest to oldest to find logical state ---

    // 1.a Process Memtable
    {
        std::lock_guard<std::mutex> memtable_lock(memtable_ptr_->memtable_mutex_);
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
    } // memtable_lock goes out of scope

    // 1.b Process Levels (from L1 down to MAX_LEVELS)
    for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
        level* current_level = levels_[level_num];
        if (!current_level) continue; // Skip if level doesn't exist

        // Lock the level while iterating its run list (sstable_runs_)
        std::lock_guard<std::mutex> level_lock(current_level->level_mutex_);

        // Process runs within the level (newest run first - reverse iteration of sstable_runs_)
        for (auto it = current_level->sstable_runs_.rbegin(); it != current_level->sstable_runs_.rend(); ++it) {
            const SSTableInfo& run_info = *it;
            const std::string& filename = run_info.filename;

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
    } // level_lock goes out of scope

    // --- Stage 2: Print the statistics based on collected data ---

    // (1) Logical Pair Count
    os << "Logical Pairs: " << logical_data.size() << std::endl;

    // (2) Keys Per Level (Physical count including tombstones/stale data in files)
    os << "LVL1: " << physical_key_counts[1];
    for (int i = 2; i <= MAX_LEVELS; ++i) {
        os << ", LVL" << i << ": " << physical_key_counts[i];
    }
    os << std::endl;

    // (3) Dump Tree (Logical view: Key:Value:Level)
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
              os << kv_pair.first << ":" << kv_pair.second << ":M ";
         }
          os << std::endl; // Newline after memtable entries
    }

    // Print Level entries ("L1" to "LMAX_LEVELS") in order
     for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
         std::string location_str = "L" + std::to_string(level_num);
         if (entries_by_location.count(location_str)) {
             for (const auto& kv_pair : entries_by_location[location_str]) {
                 os << kv_pair.first << ":" << kv_pair.second << ":" << location_str << " ";
             }
             os << std::endl; // Newline after each level's entries
         }
     }

    os << "----------------------" << std::endl;
}

// Explicit function to delete all SSTable files and directories
void lsm_tree::cleanup_files() {
    std::cout << "Cleaning up ALL SSTable files and directories..." << std::endl;
    // Need to lock levels to get filenames and clear runs.
    // File deletion is handled by delete_sst_files's internal lock.
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
            std::vector<std::string> files_to_delete;
            { // Scoped lock for this level
                std::lock_guard<std::mutex> level_lock(levels_[i]->level_mutex_);
                files_to_delete = levels_[i]->get_run_filenames(); // Now calls the added definition
                levels_[i]->clear_runs(); // Now calls the added definition
            } // level_lock releases mutex

            // Delete the physical files after releasing the level lock
            delete_sst_files(files_to_delete); // delete_sst_files uses file_delete_mutex_ internally

            // Remove the level directory
            std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
             if (rmdir(level_dir.c_str()) != 0) {
                 if (errno != ENOTEMPTY && errno != ENOENT) { // ENOENT means it was already gone
                      std::cerr << "Warning: Could not remove directory " << level_dir << ": " << strerror(errno) << std::endl;
                 } else if (errno == ENOTEMPTY) {
                     // std::cerr << "Info: Directory not empty, not removed: " << level_dir << std::endl;
                 }
             } else {
                 // std::cout << "Removed directory: " << level_dir << std::endl;
             }
        }
    }
    // Remove the root data directory
    if (rmdir(DATA_DIR.c_str()) != 0) {
         if (errno != ENOTEMPTY && errno != ENOENT) { // ENOENT means it was already gone
             std::cerr << "Warning: Could not remove root data directory " << DATA_DIR << ": " << strerror(errno) << std::endl;
         } else if (errno == ENOTEMPTY) {
              // std::cerr << "Info: Root data directory not empty, not removed: " << DATA_DIR << std::endl;
         }
    } else {
        // std::cout << "Removed directory: " << DATA_DIR << std::endl;
    }

     // Reset run ID generator
     std::lock_guard<std::mutex> id_lock(id_mutex_);
     next_run_id_ = 0;
     // std::cout << "DEBUG: Reset next_run_id_ to 0." << std::endl; // Debug
}

// Public wrapper for load_file (loads a list of commands)
void lsm_tree::load(const std::string& fileName) {
    // Note: This function parses a file containing commands and calls the LSM tree
    // methods. The parallelization discussed applies to 'get' and 'range' calls *from*
    // this load function, not the parsing/loading itself.
    // The implementation of this function is not provided in the original snippet,
    // but the parallel `get` and `range` will be used when commands like GET or RANGE
    // are encountered in the loaded file.
    // If load_file was meant to load SSTables, it would need to handle filenames
    // correctly and potentially rebuild the tree state, similar to the constructor.
    // Assuming it's loading commands for now.

    // Example basic implementation assuming command format like "PUT 1 10", "GET 1", "RANGE 1 5":
    std::ifstream infile(fileName);
    if (!infile) {
        std::cerr << "Error: Could not open load file: " << fileName << std::endl;
        return;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(infile, line)) {
        lineNumber++;
        std::stringstream ss(line);
        std::string command;
        ss >> command;

        try {
            if (command == "PUT") {
                int key, value;
                ss >> key >> value;
                insert({key, value, false});
            } else if (command == "GET") {
                int key;
                ss >> key;
                get(key, std::cout); // Use the parallel get
            } else if (command == "DELETE") {
                int key;
                ss >> key;
                delete_key(key);
            } else if (command == "RANGE") {
                 int start, end;
                 ss >> start >> end;
                 range(start, end, std::cout); // Use the parallel range
            } else if (command.empty() || command[0] == '#') {
                 // Ignore empty lines or comments
            }
            else {
                std::cerr << "Warning: Unknown command '" << command << "' on line " << lineNumber << " in " << fileName << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing command '" << command << "' on line " << lineNumber << " in " << fileName << ": " << e.what() << std::endl;
        } catch (...) {
             std::cerr << "Unknown error processing command '" << command << "' on line " << lineNumber << " in " << fileName << std::endl;
        }
    }
    infile.close();
}