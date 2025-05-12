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
#include <mutex> // For std::mutex, std::lock_guard, std::unique_lock, std::scoped_lock

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

SSTableInfo lsm_tree::rebuild_run_info(const std::string& filename) { // Removed estimated_n_for_filter parameter
    std::vector<std::pair<int, long long>> fence_pointers;

    std::ifstream infile(filename);
    if (!infile) {
        std::cerr << "Warning: Could not open SSTable TXT file to rebuild run info: " << filename << std::endl;
        // Return empty SSTableInfo with an empty/invalid BloomFilter
        return {filename, {}, BloomFilter()};
    }

    std::string line;
    long long current_block_byte_count = 0;
    std::vector<int> keys_in_file; // To collect keys for Bloom Filter and count

    // First pass: Collect keys and build fence pointers
    // Need to seek back to beginning after this pass
    infile.clear(); // Clear EOF flags etc.
    infile.seekg(0); // Rewind to the start

    while (true) {
        long long line_start_offset = infile.tellg();

        if (line_start_offset == -1) {
             std::cerr << "Warning: tellg() failed during run info rebuild pass 1 in " << filename << std::endl;
             // Attempt cleanup and return error
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

        long long line_end_offset = infile.tellg();
        long long line_byte_size = line_end_offset - line_start_offset;

        if (current_line.empty()) {
            current_block_byte_count += line_byte_size;
            continue;
        }

        std::stringstream ss(current_line);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
             // This is a valid key-value line
             keys_in_file.push_back(current_key); // Collect the key

             // Logic for adding a fence pointer:
             if (current_block_byte_count >= BLOCK_SIZE || fence_pointers.empty()) {
                 if (line_start_offset != -1) {
                    fence_pointers.push_back({current_key, line_start_offset});
                 } else {
                     std::cerr << "Warning: Skipped adding fence pointer during rebuild (pass 1) due to failed tellg()." << std::endl;
                 }
                 current_block_byte_count = line_byte_size;
             } else {
                 current_block_byte_count += line_byte_size;
             }
        } else {
             std::cerr << "Warning: Failed to parse line during run info rebuild (pass 1): '" << current_line << "' in file: " << filename << std::endl;
             current_block_byte_count += line_byte_size;
        }
    } // End while(true) loop pass 1

    infile.close(); // Close after first pass

    // Now initialize Bloom Filter with the actual count of keys
    BloomFilter filter(keys_in_file.size(), BLOOM_FILTER_FALSE_POSITIVE_RATE);

    // Re-open file for second pass to populate the Bloom Filter
    // This is necessary because the stream was read to EOF in the first pass
    std::ifstream infile_pass2(filename);
    if (!infile_pass2) {
         std::cerr << "Error: Could not re-open SSTable TXT file for rebuild pass 2: " << filename << std::endl;
         // Return SSTableInfo with collected fence pointers but potentially empty filter
         return {filename, fence_pointers, BloomFilter()};
    }

    std::string line_pass2;
    size_t keys_added_to_filter = 0; // Sanity check

    while (std::getline(infile_pass2, line_pass2)) {
        if (line_pass2.empty()) continue;

        std::stringstream ss(line_pass2);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
            filter.add(current_key); // Add key to the filter
            keys_added_to_filter++;
        } else {
             std::cerr << "Warning: Failed to parse line during run info rebuild (pass 2): '" << line_pass2 << "' in file: " << filename << std::endl;
        }
    }
     if (!infile_pass2.eof() && infile_pass2.fail()) {
          std::cerr << "Warning: Read error or parsing issue near EOF during rebuild (pass 2) in SSTable TXT file: " << filename << std::endl;
     }


    infile_pass2.close(); // Close after second pass

    // Optional sanity check
    if (keys_added_to_filter != keys_in_file.size()) {
         std::cerr << "Error: Key count mismatch during rebuild for " << filename << ". Pass 1 found " << keys_in_file.size() << ", Pass 2 added " << keys_added_to_filter << " to filter." << std::endl;
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
        return stream_index < other.stream_index;
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
    // std::lock_guard<std::mutex> lock(level_mutex_); // REMOVED internal lock
    // Add the new run (info including filename, fence pointers, and filter)
    sstable_runs_.push_back(std::move(info));
    // The find_key logic searches runs within a level newest-first (reverse iterator).
    // The LSM tree constructor ENSURES runs are added oldest-first, so rbegin() gives newest-first.
}

// Assumes caller holds level_mutex_
// Search key in this level's SSTables (files) using Bloom filters and fence pointers
bool level::find_key(int key, int& value, bool& is_tombstone) const { // Changed to const
    // std::lock_guard<std::mutex> lock(level_mutex_); // REMOVED internal lock
    // std::cerr << "DEBUG FIND: Searching level " << curr_level_ << " runs for key " << key << std::endl; // Debug
    // Search runs in reverse order (newest first)
    for (auto it = sstable_runs_.rbegin(); it != sstable_runs_.rend(); ++it) {
        const SSTableInfo& run_info = *it;
        const std::string& filename = run_info.filename;
        const auto& fence_pointers = run_info.fence_pointers;
        const auto& filter = run_info.filter;

        // --- Bloom Filter Check ---
        // Bloom filter contains() is const
        if (!filter.contains(key)) {
             // std::cerr << "DEBUG FIND:   BF excluded key " << key << " from file " << filename << std::endl; // Debug
            continue; // Key definitely not in this file, skip reading it
        }
        // std::cerr << "DEBUG FIND:   BF might contain key " << key << " in file " << filename << ". Checking file." << std::endl; // Debug
        // --- End Bloom Filter Check ---

        // --- Use Fence Pointers to find potential block ---
        long long search_offset = 0;

        if (!fence_pointers.empty()) {
            // fence_pointers is const&, lower_bound is const
            auto fp_it = std::lower_bound(fence_pointers.begin(), fence_pointers.end(), key,
                                         [](const std::pair<int, long long>& fp, int target_key){
                                             return fp.first < target_key;
                                         });

            if (fp_it != fence_pointers.begin()) {
                --fp_it;
                search_offset = fp_it->second;
            } else { // key is >= fence_pointers.front().first, search from beginning or first pointer
                search_offset = 0;
            }
             // // std::cerr << "DEBUG FIND:     Using fence pointers, seeking to offset " << search_offset << " in " << filename << std::endl; // Debug
        } else {
             // // std::cerr << "DEBUG FIND:     No fence pointers, seeking to offset 0 in " << filename << std::endl; // Debug
             search_offset = 0;
        }
        // --- End Fence Pointer Logic ---


        // Open the file and seek to the calculated offset
        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Warning: Could not open SSTable TXT file for reading: " << filename << std::endl;
            continue;
        }

        infile.seekg(search_offset);
        if (infile.fail()) {
            std::cerr << "Warning: Failed to seek to offset " << search_offset << " in file: " << filename << std::endl;
            infile.close();
            continue;
        }

        std::string line;
        int current_key;
        int current_value;
        int tombstone_flag;

        // Read line by line from the seeked position
        // // std::cerr << "DEBUG FIND:     Scanning from offset " << search_offset << " in " << filename << std::endl; // Debug
        while (std::getline(infile, line)) {
            if (line.empty()) continue;

            std::stringstream ss(line);

            if (ss >> current_key >> current_value >> tombstone_flag) {
                // std::cerr << "DEBUG FIND:       Read key " << current_key << " (Val: " << current_value << ", Tombstone: " << tombstone_flag << ")" << std::endl; // Debug
                if (current_key == key) {
                    value = current_value;
                    is_tombstone = (tombstone_flag == 1);
                    infile.close();
                    // std::cerr << "DEBUG FIND:     Key " << key << " FOUND in " << filename << std::endl; // Debug
                    return true;
                }
                 // Optimization: If we passed the target key, it means the key isn't in this block or later in this file.
                 if (current_key > key) {
                      // std::cerr << "DEBUG FIND:     Passed key " << key << " in " << filename << ". Stopping scan." << std::endl; // Debug
                     break;
                 }

            } else {
                 std::cerr << "Warning: Parsing error during find_key in file: " << filename << ", line: " << line << std::endl;
            }
        }

        if (!infile.eof() && infile.fail()) {
             std::cerr << "Warning: Read error or parsing issue near EOF in SSTable TXT file: " << filename << std::endl;
        }

        infile.close();
        // std::cerr << "DEBUG FIND:   Key " << key << " NOT found in file " << filename << std::endl; // Debug

    }

    // std::cerr << "DEBUG FIND: Key " << key << " NOT found in level " << curr_level_ << std::endl; // Debug
    return false;
}

// Assumes caller holds level_mutex_
std::vector<std::string> level::get_run_filenames() const { // Changed to const
    // std::lock_guard<std::mutex> lock(level_mutex_); // REMOVED internal lock
    std::vector<std::string> filenames;
    filenames.reserve(sstable_runs_.size());
    for(const auto& info : sstable_runs_) {
        filenames.push_back(info.filename);
    }
    return filenames;
}

// Assumes caller holds level_mutex_
void level::clear_runs() {
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
        // Loading happens single-threaded during constructor, no need for level locks here yet.
        std::vector<std::string> found_sstable_paths; // Collect paths first

        DIR *dirp = opendir(level_dir.c_str());
        if (dirp) {
            struct dirent *dp;
            while ((dp = readdir(dirp)) != nullptr) {
                std::string filename = dp->d_name;
                std::string full_path = level_dir + "/" + filename;

                // Check if it's a regular file and potentially an SST file
                struct stat file_stat;
                if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode))
                {
                     if (filename.length() > SST_FILE_SUFFIX.length() &&
                         filename.substr(filename.length() - SST_FILE_SUFFIX.length()) == SST_FILE_SUFFIX &&
                         filename.rfind(SST_FILE_PREFIX) != std::string::npos)
                    {
                        // Found a potential SSTable file
                        found_sstable_paths.push_back(full_path); // Collect path

                        // Also update max_run_id_found while scanning filenames
                        size_t run_pos = filename.rfind(SST_FILE_PREFIX);
                        size_t sst_pos = filename.rfind(SST_FILE_SUFFIX);
                        if (run_pos != std::string::npos && sst_pos != std::string::npos && run_pos < sst_pos) {
                             try {
                                 long long run_id = std::stoll(filename.substr(run_pos + SST_FILE_PREFIX.length(), sst_pos - (run_pos + SST_FILE_PREFIX.length())));
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
        // This ensures levels_[i]->sstable_runs_ is populated in chronological order.
        std::sort(found_sstable_paths.begin(), found_sstable_paths.end(),
                  [](const std::string& a, const std::string& b) {
                      size_t run_pos_a = a.rfind(SST_FILE_PREFIX);
                      size_t sst_pos_a = a.rfind(SST_FILE_SUFFIX);
                      size_t run_pos_b = b.rfind(SST_FILE_PREFIX);
                      size_t sst_pos_b = b.rfind(SST_FILE_SUFFIX);

                      // Basic validation and fallback
                      if (run_pos_a == std::string::npos || sst_pos_a == std::string::npos || run_pos_a >= sst_pos_a ||
                          run_pos_b == std::string::npos || sst_pos_b == std::string::npos || run_pos_b >= sst_pos_b) {
                           std::cerr << "Warning: Could not parse run ID from filename for sorting (fallback to string compare): " << a << " or " << b << std::endl;
                           return a < b; // Fallback to string comparison (arbitrary but consistent)
                      }

                      try {
                          // Extract and convert run IDs
                          long long run_id_a = std::stoll(a.substr(run_pos_a + SST_FILE_PREFIX.length(), sst_pos_a - (run_pos_a + SST_FILE_PREFIX.length())));
                          long long run_id_b = std::stoll(b.substr(run_pos_b + SST_FILE_PREFIX.length(), sst_pos_b - (run_pos_b + SST_FILE_SUFFIX.length()))); // Fix substring for run_id_b

                          return run_id_a < run_id_b; // Sort by run ID ascending (oldest first)
                      } catch (...) {
                          std::cerr << "Warning: Exception parsing run ID for sorting (fallback to string compare): " << a << " or " << b << std::endl;
                          return a < b; // Fallback on exception
                      }
                  });

        // Now rebuild info (including BF and FP) and add runs to the level in sorted order
        // The add_run function will acquire the level_mutex_
        for(const auto& full_path : found_sstable_paths) {
             // Use BLOOM_FILTER_ESTIMATED_N_FLUSH as a heuristic for rebuilding
             // A more accurate rebuild might count keys, but this is simpler.
             SSTableInfo loaded_run_info = rebuild_run_info(full_path);

             if (!loaded_run_info.filename.empty()) { // Check if rebuild was successful
                 levels_[i]->add_run(std::move(loaded_run_info)); // Add to level (oldest first)
             } else {
                 std::cerr << "Error: Failed to rebuild run info for " << full_path << " during load. Skipping." << std::endl;
             }
        }
        // After this loop, levels_[i]->sstable_runs_ contains SSTableInfo objects sorted by run ID (oldest first).
        // Iterating levels_[i]->sstable_runs_.rbegin() will now correctly process newer runs first.
    }

     // Set the next run ID to be one greater than the highest found
    next_run_id_ = max_run_id_found + 1;
}

lsm_tree::~lsm_tree() {
    // Perform final flush on shutdown
    if (memtable_ptr_ && memtable_ptr_->curr_size_ > 0) {
        std::vector<key_value> data_to_flush = memtable_ptr_->flush(); // flush acquires memtable lock
        if (!data_to_flush.empty()) {
            std::string final_sstable_file = generate_sstable_filename(1); // acquires id_mutex_
            SSTableInfo final_run_info = write_sstable(data_to_flush, final_sstable_file, data_to_flush.size());
            if (!final_run_info.filename.empty()) { // Check if write was successful
                 if (levels_.size() > 1 && levels_[1]) {
                     // Need to lock L1 to add the run
                     std::lock_guard<std::mutex> l1_lock(levels_[1]->level_mutex_);
                      levels_[1]->add_run(std::move(final_run_info)); // add_run assumes lock held
                 }
            } else {
                 std::cerr << "Error: Failed to write final memtable flush to disk during shutdown!" << std::endl;
            }
        }
    }

    // Clean up level pointers
    delete memtable_ptr_;
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
             delete levels_[i]; // This deletes the level object, which destroys the sstable_runs_ vector and its SSTableInfo objects.
             // Note: The level destructor *does not* need to acquire level_mutex_
             // as the entire level object is being deleted, implying no other thread
             // should be accessing it.
        }
    }
    // Physical files remain unless cleanup_files is called.
}

std::string lsm_tree::generate_sstable_filename(int level_num) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    std::string level_dir = DATA_DIR + "/L" + std::to_string(level_num);
    return level_dir + "/" + SST_FILE_PREFIX + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

SSTableInfo lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename, size_t estimated_n_for_filter) {
    SSTableInfo run_info;
    run_info.filename = filename;
    run_info.filter = BloomFilter(estimated_n_for_filter, BLOOM_FILTER_FALSE_POSITIVE_RATE);

    std::ofstream outfile(filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open SSTable TXT file for writing: " << filename << std::endl;
        return {"", {}, {}};
    }

    long long current_block_byte_count = 0;

    for (size_t i = 0; i < data.size(); ++i) {
        const auto& kv = data[i];

        run_info.filter.add(kv.key); // BloomFilter add is thread-safe if underlying ops are (usually is, but depends on BF impl)

        long long entry_start_offset = outfile.tellp();
         if (entry_start_offset == -1) {
             std::cerr << "Warning: tellp() failed before writing entry to " << filename << ". Fence pointers might be inaccurate." << std::endl;
         }

        outfile << kv.key << " " << kv.value << " " << (kv.tombstone ? 1 : 0) << "\n";

        if (!outfile) {
             std::cerr << "Error: Failed to write to SSTable TXT file: " << filename << std::endl;
             outfile.close();
             std::remove(filename.c_str());
             return {"", {}, {}};
        }

        long long entry_end_offset = outfile.tellp();
        long long line_byte_size = 0;
        if (entry_end_offset != -1 && entry_start_offset != -1) {
             line_byte_size = entry_end_offset - entry_start_offset;
        } else if (entry_end_offset == -1) {
             std::cerr << "Warning: tellp() failed after writing entry to " << filename << ". Cannot calculate line size." << std::endl;
             line_byte_size = 0;
        }

        if (current_block_byte_count >= BLOCK_SIZE || run_info.fence_pointers.empty()) {
             if (entry_start_offset != -1) {
                 run_info.fence_pointers.push_back({kv.key, entry_start_offset});
             } else {
                 std::cerr << "Warning: Skipped adding fence pointer due to failed tellp()." << std::endl;
             }
             current_block_byte_count = line_byte_size;
        } else {
            current_block_byte_count += line_byte_size;
        }
    }

    outfile.close();
    if (!outfile) {
        std::cerr << "Error: Failed to close SSTable TXT file properly: " << filename << std::endl;
         return {"", {}, {}};
    }
    return run_info;
}

void lsm_tree::delete_sst_files(const std::vector<std::string>& filenames) {
    std::lock_guard<std::mutex> delete_lock(file_delete_mutex_); // Acquire mutex for file deletion
    // std::cerr << "DEBUG DELETE: Lock acquired." << std::endl; // Debug
    for (const auto& filename : filenames) {
        // std::cerr << "DEBUG DELETE: - Deleting: " << filename << std::endl; // Debug
        if (std::remove(filename.c_str()) != 0) {
            // It's OK if the file doesn't exist (EEXIST) - another thread might have deleted it.
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
    // for(size_t i=0; i < runs_to_merge_info.size(); ++i) {
    //      // std::cerr << "DEBUG MERGE:   Input run " << i << ": " << runs_to_merge_info[i].filename << std::endl; // Debug
    // }

    std::vector<std::ifstream> input_streams;
    input_streams.reserve(runs_to_merge_info.size());

    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input TEXT files and read the FIRST entry from EACH
    for (size_t i = 0; i < runs_to_merge_info.size(); ++i) {
        const std::string& filename = runs_to_merge_info[i].filename;
        input_streams.emplace_back(filename); // Open stream

        if (!input_streams.back()) {
            std::cerr << "Error: Could not open TXT file for merge: " << filename << std::endl;
            // Attempt to close already opened streams before returning
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
             // Clean up the attempted output file if it was created
            // std::remove(generate_sstable_filename(target_level_num).c_str()); // Don't generate filename just to remove it on failure
            return {"", {}, {}};
        }

        std::string line;
        if (std::getline(input_streams.back(), line)) {
             if (!line.empty()) {
                // std::cerr << "DEBUG MERGE READ INITIAL: Stream " << i << ", Read line: '" << line << "'" << std::endl; // Debug
                 std::stringstream ss(line);
                 int current_key, current_value, tombstone_flag;
                 if (ss >> current_key >> current_value >> tombstone_flag) {
                    // std::cerr << "DEBUG MERGE READ INITIAL: Stream " << i << ", Parsed: Key=" << current_key << ", Val=" << current_value << ", Tombstone=" << tombstone_flag << std::endl; // Debug
                    min_heap.push({{current_key, current_value, (tombstone_flag == 1)}, i});
                    // // std::cerr << "DEBUG MERGE: Pushed initial key " << current_key << " from stream " << i << std::endl; // Debug
                 } else {
                      std::cerr << "Warning: Failed to parse initial line correctly in merge file: " << filename << ", line: '" << line << "'" << std::endl;
                 }
             }
        } else {
             // If getline failed immediately, check if it's EOF or a real error
             if (!input_streams.back().eof() && input_streams.back().fail()) {
                 std::cerr << "Warning: Failed initial read from TXT file: " << filename << std::endl;
             }
        }
    }
    // // std::cerr << "DEBUG MERGE: Initial heap size: " << min_heap.size() << std::endl; // Debug


    // Generate output filename *after* opening inputs successfully
    std::string output_filename = generate_sstable_filename(target_level_num); // acquires id_mutex_
    std::ofstream outfile(output_filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open output TXT file for merge: " << output_filename << std::endl;
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return {"", {}, {}};
    }

    SSTableInfo merged_run_info;
    merged_run_info.filename = output_filename;
    merged_run_info.filter = BloomFilter(estimated_n_for_filter, BLOOM_FILTER_FALSE_POSITIVE_RATE);

    long long current_block_byte_count = 0;

    // Merge process
    while (!min_heap.empty()) {
        // Get the top element - this is the smallest key overall.
        // Due to the comparator, if duplicates exist for this key, this is the newest version.
        merge_entry current_newest_for_key = min_heap.top();
        min_heap.pop();

        int current_key = current_newest_for_key.kv.key;
        size_t newest_stream_idx = current_newest_for_key.stream_index;


        // Discard any remaining elements at the top of the heap with the SAME KEY.
        // These MUST be older duplicates based on the comparator.
        // We need to advance their streams *before* we process the newest version,
        // so the heap contains the correct next elements for the *next* iteration.
        std::vector<size_t> streams_to_advance;
        streams_to_advance.push_back(newest_stream_idx); // Add the newest stream first

        while (!min_heap.empty() && min_heap.top().kv.key == current_key) {
            merge_entry older_version = min_heap.top();
            min_heap.pop();
            streams_to_advance.push_back(older_version.stream_index); // Collect stream index
        }

        // At this point, all entries for `current_key` that were at the very top
        // of the heap when we started processing this key have been removed.
        // The `newest_entry_for_key` is still valid and is the newest among them.
        // `streams_to_advance` contains the indices of all streams that contributed
        // an entry for `current_key` in this batch.

        // std::cerr << "DEBUG MERGE: Processing key " << current_key << ". Collected " << streams_to_advance.size() << " versions from top batch." << std::endl; // Debug
        // std::cerr << "DEBUG MERGE:   Newest version to process: (Val: " << current_newest_for_key.kv.value << ", Tombstone: " << current_newest_for_key.kv.tombstone << ", Stream: " << runs_to_merge_info[current_newest_for_key.stream_index].filename << ")" << std::endl; // Debug


        // --- Process the newest version: Write if not tombstone. ---
        // This is the ONLY point where we write for `current_key`.
        if (!current_newest_for_key.kv.tombstone) {
             merged_run_info.filter.add(current_newest_for_key.kv.key); // BloomFilter add is thread-safe internally

             long long entry_start_offset = outfile.tellp();
              if (entry_start_offset == -1) { std::cerr << "Warning: tellp() failed before writing entry to merged file " << output_filename << ". Fence pointers might be inaccurate." << std::endl; }
             outfile << current_newest_for_key.kv.key << " " << current_newest_for_key.kv.value << " " << (current_newest_for_key.kv.tombstone ? 1 : 0) << "\n";

             // std::cerr << "DEBUG MERGE WRITE:   --> Wrote key " << current_key // Debug
                    //    << " (Val: " << current_newest_for_key.kv.value // Debug
                    //    << ", Tombstone: " << current_newest_for_key.kv.tombstone // Debug
                    //    << ")" << std::endl; // Debug)

             if (!outfile) {
                 std::cerr << "Error writing during merge to TXT file: " << output_filename << std::endl;
                  outfile.close(); for(auto& stream : input_streams) if(stream.is_open()) stream.close(); std::remove(output_filename.c_str()); return {"", {}, {}};
             }

             long long entry_end_offset = outfile.tellp();
             long long line_byte_size = 0;
             if (entry_end_offset != -1 && entry_start_offset != -1) { line_byte_size = entry_end_offset - entry_start_offset; }
             else if (entry_end_offset == -1) { std::cerr << "Warning: tellp() failed after writing entry to merged file " << output_filename << ". Cannot calculate line size." << std::endl; line_byte_size = 0; }

             if (current_block_byte_count >= BLOCK_SIZE || merged_run_info.fence_pointers.empty()) {
                  if (entry_start_offset != -1) { merged_run_info.fence_pointers.push_back({current_newest_for_key.kv.key, entry_start_offset}); }
                  else { std::cerr << "Warning: Skipped adding fence pointer during merge due to failed tellp()." << std::endl; }
                  current_block_byte_count = line_byte_size;
             } else {
                 current_block_byte_count += line_byte_size;
             }
        } else {
             // std::cerr << "DEBUG MERGE:   --> Discarding key " << current_key << " (Tombstone)" << std::endl; // Debug
        }

        // --- Advance streams for all versions collected in this batch ---
        // Now, read the next entry from EACH stream that contributed a version of `current_key`
        // in this batch and push it onto the heap.
        for (size_t stream_idx : streams_to_advance) {
             // std::cerr << "DEBUG MERGE:   Advancing stream " << stream_idx << " for key " << current_key << " (file: " << runs_to_merge_info[stream_idx].filename << ")" << std::endl; // Debug

            if (input_streams[stream_idx].is_open()) {
                 std::string next_line;
                 if (std::getline(input_streams[stream_idx], next_line)) {
                    // std::cerr << "DEBUG MERGE READ ADVANCE: Stream " << stream_idx << ", Read line: '" << next_line << "'" << std::endl; // Debug
                     if (!next_line.empty()) {
                         std::stringstream ss(next_line);
                         int next_key, next_value, next_tombstone_flag;
                         if (ss >> next_key >> next_value >> next_tombstone_flag) {
                            // std::cerr << "DEBUG MERGE READ ADVANCE: Stream " << stream_idx << ", Parsed: Key=" << next_key << ", Val=" << next_value << ", Tombstone=" << next_tombstone_flag << std::endl; // Debug
                             min_heap.push({{next_key, next_value, (next_tombstone_flag == 1)}, stream_idx});
                             // std::cerr << "DEBUG MERGE:     -> Pushed next key " << next_key << " from stream " << stream_idx << std::endl; // Debug
                         } else {
                              std::cerr << "Warning: Failed to parse line after advancing stream " << stream_idx << " for key " << current_key << ": '" << next_line << "'" << std::endl;
                         }
                     } else {
                         // Read an empty line. Stream is advanced, nothing to push.
                         // std::cerr << "DEBUG MERGE READ ADVANCE:     Read empty line from stream " << stream_idx << std::endl; // Debug
                     }
                 } else {
                      // getline failed (EOF or read error)
                     if (!input_streams[stream_idx].eof() && input_streams[stream_idx].fail()) {
                         std::cerr << "Warning: Failed read from TXT file : " << runs_to_merge_info[stream_idx].filename << std::endl;
                     }
                     input_streams[stream_idx].close(); // Close stream when exhausted or failed
                     // std::cerr << "DEBUG MERGE:     -> Stream " << stream_idx << " exhausted or failed." << std::endl; // Debug
                 }
            } else {
                 // std::cerr << "DEBUG MERGE:     Stream " << stream_idx << " was already closed." << std::endl; // Debug
            }
        }
    } // End while(!min_heap.empty())

    // Close output file
    outfile.close();
    if (!outfile) {
        std::cerr << "Error closing merged output TXT file properly: " << output_filename << std::endl;
         // If closing fails, the file might be incomplete or corrupted. Delete it.
         std::remove(output_filename.c_str());
         return {"", {}, {}};
    }

    // Close any remaining input streams (should all be closed by now if successful)
    for (auto& stream : input_streams) {
        if (stream.is_open()) {
             std::cerr << "Warning: Input stream was still open after merge loop. Closing." << std::endl;
             stream.close();
        }
    }

    // std::cerr << "DEBUG MERGE: == Finished merge into level " << target_level_num << ". Output: " << output_filename << " ==" << std::endl; // Debug
    return merged_run_info;
}

// Function to check and trigger merges starting from a level
void lsm_tree::check_and_trigger_merge(int level_num) {
    // Merge can be triggered from L1 up to L_MAX.
    // A merge from level_num produces a run for level_num + 1.
    // The new run is placed in target_level, capped at MAX_LEVELS.
    if (level_num < 1 || level_num > MAX_LEVELS) {
        // Invalid source level for merge.
        // std::cerr << "DEBUG MERGE TRIGGER: Invalid source level " << level_num << ". Returning." << std::endl; // Debug
        return;
    }

    level* current_level = levels_[level_num];
    int target_level_num = level_num + 1;
    level* target_level = nullptr;

    // Determine the actual target level pointer, capped at MAX_LEVELS
    if (target_level_num <= MAX_LEVELS) {
        target_level = levels_[target_level_num];
    } else {
        // If merging from MAX_LEVELS, the target is still MAX_LEVELS (compaction strategy varies)
        // In this simple model, data effectively "overflows" from MAX_LEVELS by merging into L_MAX.
        target_level_num = MAX_LEVELS;
        target_level = levels_[MAX_LEVELS]; // This handles L_MAX merging into L_MAX
    }

    // Defensive check: Ensure we have a valid target level pointer
    if (!target_level) {
        std::cerr << "Critical Error: Target level " << target_level_num << " for merge from L" << level_num << " is unexpectedly null. Cannot merge." << std::endl;
        return;
    }


    // --- Phase 1: Determine merge needs, identify files, clear source level ---
    // Acquire lock for the *source* level to read its runs and clear it.
    // This lock must be held while reading sstable_runs_ and calling clear_runs.
    std::vector<SSTableInfo> runs_to_merge_info;
    std::vector<std::string> files_to_delete;

    { // Scoped lock for the current (source) level
        std::lock_guard<std::mutex> current_level_lock(current_level->level_mutex_);

        // Check merge condition *while holding the lock*
        if (current_level->sstable_runs_.size() < SIZE_RATIO) {
             // std::cerr << "DEBUG MERGE TRIGGER: Level " << level_num << " doesn't need merge (" << current_level->sstable_runs_.size() << "/" << SIZE_RATIO << "). Returning." << std::endl; // Debug
            return; // Locks automatically released when lock_guard goes out of scope
        }
        // std::cerr << "DEBUG MERGE TRIGGER: Level " << level_num << " needs merge (" << current_level->sstable_runs_.size() << "/" << SIZE_RATIO << "). Runs in memory for L" << level_num << ":" << std::endl; // Debug

        // Copy runs to merge info and filenames while holding the lock
        runs_to_merge_info.reserve(current_level->sstable_runs_.size());
        files_to_delete.reserve(current_level->sstable_runs_.size());
        for(const auto& info : current_level->sstable_runs_) {
            runs_to_merge_info.push_back(info); // Copies SSTableInfo (incl. filename, fp, filter)
            files_to_delete.push_back(info.filename); // Copies filename for deletion
        }
         // std::cerr << "DEBUG MERGE TRIGGER: Files collected for merge/delete: " << files_to_delete.size() << " from L" << level_num << "." << std::endl; // Debug

        // Clear the runs from the current level *now* while the lock is held.
        current_level->clear_runs(); // Assumes clear_runs does not re-lock
         // std::cerr << "DEBUG MERGE TRIGGER: Level " << level_num << " runs cleared from memory." << std::endl; // Debug

    } // current_level_lock goes out of scope and releases the mutex

    // --- Phase 2: Perform the merge (File I/O) ---
    // This phase is computationally intensive and involves file reading/writing.
    // It happens *without* holding level locks.
    // The merge_runs function itself does not need level locks as it operates on file handles.
    // std::cerr << "DEBUG MERGE TRIGGER: Performing merge for L" << level_num << " into L" << target_level_num << "." << std::endl; // Debug

    SSTableInfo merged_run_info = merge_runs(target_level_num, runs_to_merge_info, BLOOM_FILTER_ESTIMATED_N_MERGE); // Pass SSTableInfo vector

    // --- Phase 3: Add the new merged run to the target level ---
    // Acquire lock for the *target* level to add the new run.
    // This lock must be held while adding the merged_run_info.
    if (!merged_run_info.filename.empty()) {
         // Merge output file was successfully created
         // std::cerr << "DEBUG MERGE TRIGGER: Merge output file created: " << merged_run_info.filename << "." << std::endl; // Debug

         { // Scoped lock for the target level
              std::lock_guard<std::mutex> target_level_lock(target_level->level_mutex_);
               // std::cerr << "DEBUG MERGE TRIGGER: Adding merged run to L" << target_level_num << "." << std::endl; // Debug
              levels_[target_level_num]->add_run(std::move(merged_run_info)); // Assumes add_run does not re-lock
         } // target_level_lock goes out of scope and releases the mutex

         // --- Phase 4: Recursively check if the *next* level now needs merging ---
         // This check happens *after* the current merge is complete and the new run is added.
         // It might trigger another merge *from* the target level.
         // Only recurse if the target level number is within the valid range to *start* a merge from.
         if (target_level_num <= MAX_LEVELS) { // Valid source level for the next potential merge
              // std::cerr << "DEBUG MERGE TRIGGER: Checking if L" << target_level_num << " now needs a merge." << std::endl; // Debug
              check_and_trigger_merge(target_level_num);
         } else {
             // std::cerr << "DEBUG MERGE TRIGGER: Target level " << target_level_num << " is beyond MAX_LEVELS. Stopping recursion." << std::endl; // Debug
         }

    } else {
         // Merge failed (output file not created)
         // std::cerr << "DEBUG MERGE TRIGGER: Merge failed for level " << level_num << ". State potentially inconsistent." << std::endl; // Debug
        std::cerr << "Error: Merge failed for level " << level_num << ". Output file not created. Files from L" << level_num << " were cleared from memory but NOT deleted from disk. Manual cleanup may be required." << std::endl;
        // Note: In a real system, you'd need robust recovery or rollback here.
    }

    // --- Phase 5: Delete old files ---
    // This happens *after* the merge completes (or fails) and target level state is updated.
    // It uses the dedicated file_delete_mutex_ internally to serialize file removals.
     // std::cerr << "DEBUG MERGE TRIGGER: Initiating file deletion for runs merged from L" << level_num << "." << std::endl; // Debug
    delete_sst_files(files_to_delete); // delete_sst_files handles its own locking

     // std::cerr << "DEBUG MERGE TRIGGER: Finished merge process for L" << level_num << "." << std::endl; // Debug
}

// --- Public Interface Implementation ---

bool lsm_tree::insert(key_value kv_pair) {
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
            // This is file I/O, doesn't need LSM tree/level locks.
            // Use the actual size of data being flushed as the estimated N for the filter
            SSTableInfo new_run_info = write_sstable(data_to_flush, new_sstable_file, data_to_flush.size());

            if (!new_run_info.filename.empty()) { // Check if write was successful
                // Add the new run (info) to Level 1
                // Need to lock Level 1 to modify its sstable_runs_ vector.
                if (levels_.size() > 1 && levels_[1]) { // Defensive check
                     // std::cerr << "DEBUG INSERT: Adding new run " << new_run_info.filename << " to L1." << std::endl; // Debug
                    std::lock_guard<std::mutex> l1_lock(levels_[1]->level_mutex_);
                    levels_[1]->add_run(std::move(new_run_info)); // add_run assumes lock held
                } else {
                    std::cerr << "Critical Error: Level 1 is null or levels_ vector too small. Cannot add flushed run." << std::endl;
                     // The run info and file exist, but are not tracked. Data loss.
                     // In a real system, this would require significant error handling/recovery.
                }


                // Check if Level 1 needs merging now
                 // This call will acquire its own locks internally.
                 // It should be called *after* the flushed run has been successfully added to L1.
                check_and_trigger_merge(1);

            } else {
                std::cerr << "Error: Failed to write flushed memtable to disk. Data potentially lost." << std::endl;
                return false; // Indicate failure to insert (due to flush failure)
            }
         } else {
             // This case should ideally not happen if memtable was full but flush yielded empty data.
             // It means memtable_ptr_->flush() returned an empty vector unexpectedly.
             std::cerr << "Warning: Memtable was full, but flush returned empty data. This indicates an anomaly." << std::endl;
             // The original insert failed due to fullness. We should retry inserting now that memtable is supposedly clear.
             bool insert_after_anomalous_flush = memtable_ptr_->insert(kv_pair); // insert acquires memtable_mutex_
             if (!insert_after_anomalous_flush) {
                 std::cerr << "Critical Error: Could not insert element into memtable even after anomalous flush attempt." << std::endl;
                 return false; // Indicate critical failure
             }
             return true; // Insert successful after anomalous flush attempt
         }


        // If flush was successful (data_to_flush was not empty), the original kv_pair still needs to be inserted.
        // The initial memtable_ptr_->insert(kv_pair) returned false because it was full.
        // Memtable is now empty (or has space after the anomalous flush case above).
        // The insert below acquires memtable_mutex_ again.
        bool insert_after_flush_ok = memtable_ptr_->insert(kv_pair); // insert acquires memtable_mutex_
        if (!insert_after_flush_ok) {
            // This should be unreachable if flush cleared the memtable correctly.
            std::cerr << "Critical Error: Failed to insert element into empty memtable after successful flush." << std::endl;
            return false; // Indicate critical failure
        }
        return true; // Insert successful after flush and re-insert
    }
    // Insert successful (directly into memtable, first attempt)
    return true;
}

// Note: get accesses multiple potentially shared structures (memtable, levels).
// It relies on find_key in memtable and level to handle their own locks.
// Output to os is protected by cout_mutex_.
int lsm_tree::get(int key, std::ostream& os, bool called_from_range) {
    // Acquire lock for output stream only when printing will occur
    // Defer locking so it's only locked right before actual output.
    std::unique_lock<std::mutex> cout_lock(cout_mutex_, std::defer_lock);

    int value = -1;
    bool is_tombstone = false;
    bool found = false;

    // 1. Check Memtable
    // // std::cerr << "DEBUG GET: Searching memtable for key " << key << std::endl; // Debug
    if (memtable_ptr_->find_key(key, value, is_tombstone)) { // find_key acquires memtable_mutex_
        found = true;
        // // std::cerr << "DEBUG GET: Found key " << key << " in memtable (Value: " << value << ", Tombstone: " << is_tombstone << ")" << std::endl; // Debug
        if (is_tombstone) {
            if (!called_from_range) {
                cout_lock.lock(); // Lock before printing
                os << endl;
            }
            return -1;
        }
    } else {
        // 2. Check Levels
        // // std::cerr << "DEBUG GET: Not in memtable. Searching levels for key " << key << std::endl; // Debug
        for (int i = 1; i <= MAX_LEVELS; ++i) {
            if (levels_[i]) {
                // // std::cerr << "DEBUG GET: Searching level " << i << " for key " << key << std::endl; // Debug
                // Need to lock the level *before* calling find_key on it
                std::lock_guard<std::mutex> level_lock(levels_[i]->level_mutex_); // Acquired lock
                if (levels_[i]->find_key(key, value, is_tombstone)) { // find_key assumes caller holds lock
                     found = true;
                     // // std::cerr << "DEBUG GET: Found key " << key << " in level " << i << " (Value: " << value << ", Tombstone: " << is_tombstone << ")" << std::endl; // Debug
                     if (is_tombstone) {
                        if (!called_from_range) {
                            cout_lock.lock(); // Lock before printing
                            os << endl;
                        }
                        return -1;
                     }
                     // Found valid entry in this level, stop searching lower levels
                     break; // Release level_lock due to break
                }
            } // level_lock goes out of scope here if break is not hit
        }
    }

    // Output based on findings
    if (found && !is_tombstone) {
         if (!called_from_range) {
             cout_lock.lock(); // Lock before printing
             os << value << endl;
         } else {
             // If called from range, the caller (range) is responsible for cout_mutex_
             // We print directly here assuming the caller holds the lock.
             os << key << ":" << value << " ";
         }
         return value;
    } else {
         // Not found or found and is tombstone
         if (!called_from_range) {
             cout_lock.lock(); // Lock before printing
             os << endl;
         } else {
             // If called from range, print nothing for not found/tombstone, caller handles spacing/newline.
             // Your previous code printed nothing, which is fine for range output format.
             // std::cerr << "DEBUG GET: Key " << key << " not found or is tombstone, skipping range output." << std::endl; // Debug
         }
         return -1;
    }
}

// Note: range calls get repeatedly and prints header/footer.
// It acquires cout_mutex_ for the entire operation.
void lsm_tree::range(int start, int end, std::ostream& os) {
    std::lock_guard<std::mutex> cout_lock(cout_mutex_); // Lock for the entire range output
    os << "Range (" << start << " to " << end << "): ";
    for (int k = start; k <= end; ++k) {
        // Call get in range mode. get will *not* acquire cout_mutex_ if called_from_range is true.
        // This function (range) holds the lock for get's printing.
        get(k, os, true);
    }
    os << endl;
}

void lsm_tree::delete_key(int key) {
    // Insert a tombstone entry for the key.
    // This relies on the insert function's locking.
    insert({key, 0, true});
}

// Note: printStats reads state across memtable and levels.
// It locks memtable_mutex_ and each level_mutex_ sequentially.
// Output to os is protected by cout_mutex_.
void lsm_tree::printStats(std::ostream& os) const { // Added const
    std::lock_guard<std::mutex> cout_lock(cout_mutex_); // Lock for the entire stats output
    os << "--- LSM Tree Stats ---" << std::endl;

    // Data structures to hold intermediate results
    // NOTE: Gathering stats across multiple levels atomically (a consistent snapshot)
    // requires more complex locking than simple mutexes. This implementation locks each part (memtable, then each level's run list) sequentially, meaning the state *could* change between reading different levels.
    std::map<int, std::pair<int, std::string>> logical_data; // Map<key, Pair<value, location>>
    std::set<int> deleted_keys;                             // Keep track of keys confirmed deleted
    std::vector<long long> physical_key_counts(MAX_LEVELS + 1, 0); // Count all keys per level file

    // --- Stage 1: Process data from newest to oldest to find logical state ---

    // 1.a Process Memtable
    // Lock the memtable while processing it
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

    // 1.b Process Levels (from L1 down to MAX_LEVELS)
    for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
        level* current_level = levels_[level_num];
        if (!current_level) continue; // Skip if level doesn't exist

        // Lock the level while iterating its run list (sstable_runs_)
        std::lock_guard<std::mutex> level_lock(current_level->level_mutex_); // Safe, mutable mutex

        // Process runs within the level (newest run first - reverse iteration of sstable_runs_)
        for (auto it = current_level->sstable_runs_.rbegin(); it != current_level->sstable_runs_.rend(); ++it) {
            const SSTableInfo& run_info = *it;
            const std::string& filename = run_info.filename;
            // // std::cerr << "DEBUG:   Reading file " << filename << " for stats..." << std::endl;

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
    os << "Logical Pairs: " << logical_data.size() << std::endl;

    // (2) Keys Per Level (Physical count including tombstones/stale data in files)
    os << "LVL1: " << physical_key_counts[1];
    for (int i = 2; i <= MAX_LEVELS; ++i) {
        os << ", LVL" << i << ": " << physical_key_counts[i];
    }
    os << std::endl;

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
// Note: Called during initialization and shutdown, should be thread-safe with other ops.
void lsm_tree::cleanup_files() {
    std::cout << "Cleaning up ALL SSTable files and directories..." << std::endl;
    // Need to lock levels to get filenames and clear runs.
    // File deletion is handled by delete_sst_files's internal lock.
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
            std::vector<std::string> files_to_delete;
            { // Scoped lock for this level
                std::lock_guard<std::mutex> level_lock(levels_[i]->level_mutex_); // Safe, mutable mutex
                // Get filenames from SSTableInfo objects while holding the lock
                files_to_delete = levels_[i]->get_run_filenames(); // get_run_filenames assumes lock held

                // Clear the list of SSTableInfo objects in memory while holding the lock
                levels_[i]->clear_runs(); // clear_runs assumes lock held
            } // level_lock goes out of scope, releases mutex

            // Delete the physical files after releasing the level lock
            delete_sst_files(files_to_delete); // delete_sst_files uses file_delete_mutex_ internally

            // Optionally remove the directory itself (this might fail if not empty)
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
    // Optionally remove the root data directory (this might fail if not empty)
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
     std::lock_guard<std::mutex> id_lock(id_mutex_);
     next_run_id_ = 0;
}