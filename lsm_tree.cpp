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
        return {};
    }

    SSTableInfo run_info; 
    run_info.filename = filename;

    long long current_block_byte_count = 0;
    std::vector<int> keys_in_file;
    int file_min_key = std::numeric_limits<int>::max();
    int file_max_key = std::numeric_limits<int>::min();
    bool found_any_key = false;

    // First pass: Collect keys, build fence pointers, find min/max keys
    infile.clear();
    infile.seekg(0, std::ios::beg); 

    while (true) {
        long long line_start_offset = infile.tellg();

        if (line_start_offset == -1 && !infile.eof()) { // check !infile.eof() otherwise tellg() at eof might be -1
             std::cerr << "Warning: tellg() failed during run info rebuild pass 1 in " << filename << std::endl;
        }

        std::string current_line;
        if (!std::getline(infile, current_line)) {
            if (!infile.eof() && infile.fail()) {
                 std::cerr << "Warning: Read error during run info rebuild pass 1 in " << filename << std::endl;
            }
            break; 
        }

        if (current_line.empty()) {
             // If tellg worked, use precise size. Otherwise, add estimated newline size.
             if (line_start_offset != -1 && infile.tellg() != -1) {
                 current_block_byte_count += (infile.tellg() - line_start_offset);
             } else {
                 current_block_byte_count += 1; 
             }
            continue;
        }

        std::stringstream ss(current_line);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
             keys_in_file.push_back(current_key);
             found_any_key = true;
             file_min_key = std::min(file_min_key, current_key);
             file_max_key = std::max(file_max_key, current_key);

             long long line_byte_size = 0;
             long long line_end_offset = infile.tellg();
             if (line_end_offset != -1 && line_start_offset != -1) {
                 line_byte_size = line_end_offset - line_start_offset;
             } else if (line_end_offset == -1) {
                  line_byte_size = current_line.length() + 1;
             }

             if (current_block_byte_count >= BLOCK_SIZE || fence_pointers.empty()) {
                 if (line_start_offset != -1) {
                    fence_pointers.push_back({current_key, line_start_offset});
                 } else {
                      fence_pointers.push_back({current_key, 0});
                      std::cerr << "Warning: Using fallback offset 0 for fence pointer in " << filename << " due to tellg failure." << std::endl;
                 }
                 current_block_byte_count = line_byte_size;
             } else {
                 current_block_byte_count += line_byte_size;
             }
        } else {
             std::cerr << "Warning: Failed to parse line during run info rebuild (pass 1): '" << current_line << "' in file: " << filename << std::endl;
             // Even if parse fails, estimate size to contribute to block count
             long long line_byte_size = 0;
             long long line_end_offset = infile.tellg();
             if (line_end_offset != -1 && line_start_offset != -1) {
                 line_byte_size = line_end_offset - line_start_offset;
             } else if (line_end_offset == -1) {
                  line_byte_size = current_line.length() + 1; 
             }
             current_block_byte_count += line_byte_size;
        }
    } 

    infile.close();

    // Set min/max keys *only if* keys were found
    if (found_any_key) {
        run_info.min_key = file_min_key;
        run_info.max_key = file_max_key;
    } else {
         run_info.min_key = std::numeric_limits<int>::max();
         run_info.max_key = std::numeric_limits<int>::min();
         run_info.fence_pointers.clear(); 
    }

    // Now initialize Bloom Filter with the actual count of keys found
    run_info.filter = BloomFilter(keys_in_file.size(), BLOOM_FILTER_FALSE_POSITIVE_RATE);

    // Re-open file for second pass to populate the Bloom Filter
    std::ifstream infile_pass2(filename);
    if (!infile_pass2) {
         std::cerr << "Error: Could not re-open SSTable TXT file for rebuild pass 2: " << filename << std::endl;
         return run_info; 
    }

    std::string line_pass2;
    size_t keys_added_to_filter = 0;

    while (std::getline(infile_pass2, line_pass2)) {
        if (line_pass2.empty()) continue;

        std::stringstream ss(line_pass2);
        int current_key, value, tombstone_flag;

        if (ss >> current_key >> value >> tombstone_flag) {
            run_info.filter.add(current_key);
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

    return run_info; 
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
    return stream_index < other.stream_index; 
}
 
};
// --- Level Class Implementation ---
level::level(int capacity, int curr_level) : capacity_(capacity), curr_level_(curr_level) {
}

level::~level() {
}

// Assumes caller holds level_mutex_
void level::add_run(SSTableInfo&& info) {
    sstable_runs_.push_back(std::move(info));
}

// Assumes caller holds level_mutex_ 
// Search key in this level's SSTables (files) using Bloom filters and fence pointers
bool level::find_key(int key, int& value, bool& is_tombstone) const {
    // Iterate through runs in reverse order (newest first)
    for (auto it = sstable_runs_.rbegin(); it != sstable_runs_.rend(); ++it) {
        const SSTableInfo& run_info = *it;
        const std::string& filename = run_info.filename;
        const auto& fence_pointers = run_info.fence_pointers;
        const auto& filter = run_info.filter;

        // Bloom Filter Check
        if (!filter.contains(key)) {
            continue; 
        }

        // Use fence pointers to find potential block
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
                search_offset = 0;
            }
        } else {
            search_offset = 0; 
        }

        // End Fence Pointer Logic
        // Open the file and seek to the calculated offset
        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Warning: Could not open SSTable TXT file for reading in find_key: " << filename << std::endl;
            continue;
        }

        if (search_offset < 0) search_offset = 0; 

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
                    return true; 
                }
                // If we passed the target key, it means the key isn't in this block or later in this file.
                if (current_key > key) {
                    break;
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

    return false;
    
}

// Helper for parallel search to return optional<tuple<...>>
// Acquires and releases its own lock internally.
std::optional<std::tuple<int, int, bool, int>> level::find_key_parallel(int key) const {
    std::lock_guard<std::mutex> lock(level_mutex_);
    int value;
    bool is_tombstone;
    if (find_key(key, value, is_tombstone)) {
        return std::make_optional(std::make_tuple(key, value, is_tombstone, curr_level_));
    }
    return std::nullopt;
}

// Assumes caller holds level_mutex_
std::vector<std::string> level::get_run_filenames() const {
    std::vector<std::string> filenames;
    filenames.reserve(sstable_runs_.size());
    for(const auto& info : sstable_runs_) {
        filenames.push_back(info.filename);
    }
    return filenames;
}

// Assumes caller holds level_mutex_
void level::clear_runs() {
    sstable_runs_.clear();
}

// --- Memtable Class Implementation ---
memtable::memtable(size_t capacity) : capacity_(capacity), cur_size_(0) {
}

memtable::memtable() : capacity_(MEMTABLE_CAPACITY), cur_size_(0) {
}

void memtable::insert(key_value kv_pair, bool& trigger_flush) {
    std::lock_guard<std::mutex> lock(memtable_mutex_);
    bool is_update = memtable_.count(kv_pair.key) > 0;

    // Insert or update the key
    memtable_[kv_pair.key] = kv_pair;

    // If it was a new key, increment size
    if (!is_update) {
        cur_size_++;
    }

    trigger_flush = (cur_size_ >= capacity_);
}

std::vector<key_value> memtable::flush() {
    std::lock_guard<std::mutex> lock(memtable_mutex_);
    // Copy elements from the map to a vector
    std::vector<key_value> data_to_flush;
    data_to_flush.reserve(memtable_.size()); // Reserve space based on map size
    for(const auto& pair : memtable_) {
        data_to_flush.push_back(pair.second);
    }

    // Clear the map
    memtable_.clear();

    return data_to_flush; 
}

bool memtable::find_key(int key, int& value, bool& is_tombstone) {
    std::lock_guard<std::mutex> lock(memtable_mutex_);
    auto it = memtable_.find(key);

    if (it != memtable_.end()) {
        const key_value& kv = it->second; 
        value = kv.value;
        is_tombstone = kv.tombstone;
        return true;
    }

    return false; 
 
}

// --- LSM_Tree Class Implementation ---
lsm_tree::lsm_tree() : next_run_id_(0), shutdown_requested_(false) { 
    memtable_ptr_ = new memtable(MEMTABLE_CAPACITY);

    levels_.resize(MAX_LEVELS + 1, nullptr); 
    // 1. Create root data directory
    if (!create_directory(DATA_DIR)) {
        throw std::runtime_error("Failed to create or access data directory: " + DATA_DIR);
    }

    long long max_run_id_found = -1; 

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
                        filename.rfind(SST_FILE_PREFIX) == 0) 
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
        std::sort(found_sstable_paths.begin(), found_sstable_paths.end(),
                [](const std::string& a, const std::string& b) {
                    size_t prefix_len = SST_FILE_PREFIX.length();
                    size_t suffix_len = SST_FILE_SUFFIX.length();

                    if (a.rfind(SST_FILE_PREFIX) != 0 || a.length() < prefix_len + suffix_len ||
                        b.rfind(SST_FILE_PREFIX) != 0 || b.length() < prefix_len + suffix_len ||
                        a.substr(a.length() - suffix_len) != SST_FILE_SUFFIX ||
                        b.substr(b.length() - suffix_len) != SST_FILE_SUFFIX) {
                        return a < b;
                    }

                    try {
                        long long run_id_a = std::stoll(a.substr(prefix_len, a.length() - prefix_len - suffix_len));
                        long long run_id_b = std::stoll(b.substr(prefix_len, b.length() - prefix_len - suffix_len));

                        return run_id_a < run_id_b;
                    } catch (...) {
                        std::cerr << "Warning: Exception parsing run ID for sorting (fallback to string compare): " << a << " or " << b << std::endl;
                        return a < b; 
                    }
                });

        // Now rebuild info (including BF and FP) and add runs to the level in sorted order
        for(const auto& full_path : found_sstable_paths) {
            SSTableInfo loaded_run_info = rebuild_run_info(full_path);

            if (!loaded_run_info.filename.empty()) {
                // Need to lock L<i> to add the run during constructor load
                levels_[i]->add_run(std::move(loaded_run_info));
            } else {
                std::cerr << "Error: Failed to rebuild run info for " << full_path << " during load. Skipping." << std::endl;
            }
        }
    }

    // Set the next run ID to be one greater than the highest found
    next_run_id_ = max_run_id_found + 1;

    // Start the background flusher thread
    flusher_thread_ = std::thread(&lsm_tree::flushThreadLoop, this);
}

lsm_tree::~lsm_tree() {
    shutdown_requested_.store(true); 
    flush_request_cv_.notify_one(); 

    // Perform final flush on shutdown
    if (flusher_thread_.joinable()) {
    flusher_thread_.join();

    // Wait for any pending background tasks
    for(auto& future : background_tasks_) {
        if(future.valid()) { // Check if the future holds a shared state
            future.get(); 
        }
    }
    background_tasks_.clear(); 

    // Clean up level pointers
    delete memtable_ptr_;
    memtable_ptr_ = nullptr; 
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        if (levels_[i]) {
            delete levels_[i];
            levels_[i] = nullptr; 
        }
    }
    // Physical files remain unless cleanup_files is called separately before deletion.
    }
}

std::string lsm_tree::generate_sstable_filename(int level_num) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    std::string level_dir = DATA_DIR + "/L" + std::to_string(level_num);
    return level_dir + "/" + SST_FILE_PREFIX + std::to_string(next_run_id_++) + SST_FILE_SUFFIX;
}

SSTableInfo lsm_tree::write_sstable(const std::vector<key_value>& data, const std::string& filename, size_t estimated_n_for_filter) {
    SSTableInfo run_info;
    run_info.filename = filename;

    if (!data.empty()) {
        // Set min/max keys from the input data
        run_info.min_key = data.front().key;
        run_info.max_key = data.back().key;
    } else {
         // Empty SSTable has no keys, set range to indicate no overlap
         run_info.min_key = std::numeric_limits<int>::max();
         run_info.max_key = std::numeric_limits<int>::min();
    }

    // Initialize Bloom Filter using the passed estimated_n_for_filter
    size_t n_for_filter = (estimated_n_for_filter > 0) ? estimated_n_for_filter : data.size();
    if (n_for_filter == 0 && !data.empty()) {
         n_for_filter = data.size();
    } else if (n_for_filter == 0 && data.empty()) {
    }

    if (n_for_filter > 0) {
         run_info.filter = BloomFilter(n_for_filter, BLOOM_FILTER_FALSE_POSITIVE_RATE);
    } else {
         run_info.filter = BloomFilter(1, 1.0);
    }


    std::ofstream outfile(filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open SSTable TXT file for writing: " << filename << std::endl;
        return {};
    }

    long long current_block_byte_count = 0;

    for (size_t i = 0; i < data.size(); ++i) {
        const auto& kv = data[i];

        // Add key to filter *after* initializing it
        if (n_for_filter > 0) {
            run_info.filter.add(kv.key);
        }

        long long entry_start_offset = outfile.tellp(); 

        outfile << kv.key << " " << kv.value << " " << (kv.tombstone ? 1 : 0) << "\n";

        if (!outfile) {
             std::cerr << "Error: Failed to write entry (" << kv.key << ") to SSTable TXT file: " << filename << std::endl;
             outfile.close();
             std::remove(filename.c_str());
             return {};
        }

        long long entry_end_offset = outfile.tellp();
        long long line_byte_size = 0;
        if (entry_end_offset != -1 && entry_start_offset != -1) {
             line_byte_size = entry_end_offset - entry_start_offset;
        } else if (entry_end_offset == -1) {
             line_byte_size = std::to_string(kv.key).length() + std::to_string(kv.value).length() + std::to_string(kv.tombstone ? 1 : 0).length() + 2 + 1;
             std::cerr << "Warning: Using estimated line size for fence pointer calculation in " << filename << " due to tellp failure." << std::endl;
        }


        // Determine the offset for a potential fence pointer for THIS key
        long long offset_for_this_line = entry_start_offset;

        if (offset_for_this_line == -1) {
             std::cerr << "Warning: Failed to get start offset for key " << kv.key << " in " << filename << ". Using 0 as fallback offset for fence pointer." << std::endl;
             offset_for_this_line = 0;
        }

        // Add fence pointer if block condition met
        if (current_block_byte_count >= BLOCK_SIZE || run_info.fence_pointers.empty()) {
            // Use the calculated offset for the fence pointer
            run_info.fence_pointers.push_back({kv.key, offset_for_this_line});
            current_block_byte_count = line_byte_size;
        } else {
           // If not adding a fence pointer, just accumulate the line size
           current_block_byte_count += line_byte_size;
        }
    }

    outfile.close();
    if (!outfile) {
        std::cerr << "Error: Failed to close SSTable TXT file properly: " << filename << std::endl;
         std::remove(filename.c_str()); 
         return {}; 
    }
    return run_info; 
}

void lsm_tree::delete_sst_files(const std::vector<std::string>& filenames) {
    // Acquire mutex for file deletion
    std::lock_guard<std::mutex> delete_lock(file_delete_mutex_);
    for (const auto& filename : filenames) {
        if (std::remove(filename.c_str()) != 0) {
            if (errno != ENOENT) { // ENOENT is "No such file or directory"
                std::cerr << "ERROR DELETE: Could not delete SSTable file: " << filename << " (" << strerror(errno) << ")" << std::endl;
            } else {
            }
        }
    }
}

SSTableInfo lsm_tree::merge_runs(int target_level_num, const std::vector<SSTableInfo>& runs_to_merge_info, size_t estimated_n_for_filter) {
    if (runs_to_merge_info.empty()) {
        return {"", {}, {}};
    }

    // Input streams must be kept in the order of run_to_merge_info indices (oldest first)
    std::vector<std::ifstream> input_streams;
    input_streams.reserve(runs_to_merge_info.size());

    // Use min-heap to get smallest key, prioritizing newest version (larger stream_index) for duplicates
    priority_queue<merge_entry, vector<merge_entry>, greater<merge_entry>> min_heap;

    // Open all input TXT files and read the FIRST entry from EACH
    for (size_t i = 0; i < runs_to_merge_info.size(); ++i) {
        const std::string& filename = runs_to_merge_info[i].filename;
        input_streams.emplace_back(filename);

        if (!input_streams.back()) {
            std::cerr << "Error: Could not open TXT file for merge: " << filename << std::endl;
            for(auto& stream : input_streams) if(stream.is_open()) stream.close();
            return {"", {}, {}}; // Indicate failure
        }

        std::string line;
        if (std::getline(input_streams.back(), line)) {
            if (!line.empty()) {
                std::stringstream ss(line);
                int current_key, current_value, tombstone_flag;
                if (ss >> current_key >> current_value >> tombstone_flag) {
                    min_heap.push({{current_key, current_value, (tombstone_flag == 1)}, i}); 
                } else {
                    std::cerr << "Warning: Failed to parse initial line correctly in merge file: " << filename << ", line: '" << line << "'" << std::endl;
                }
            }
        } else {
            if (!input_streams.back().eof() && input_streams.back().fail()) {
                std::cerr << "Warning: Failed initial read from TXT file: " << filename << std::endl;
            }
        }
    }

    std::string output_filename = generate_sstable_filename(target_level_num); // acquires id_mutex_
    std::ofstream outfile(output_filename, std::ios::trunc);
    if (!outfile) {
        std::cerr << "Error: Could not open output TXT file for merge: " << output_filename << std::endl;
        for(auto& stream : input_streams) if(stream.is_open()) stream.close();
        return {"", {}, {}}; // Indicate failure
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

        // Discard any remaining elements at the top of the heap with the SAME KEY.
        // These are older duplicates. We need to advance their streams.
        std::vector<size_t> streams_to_advance;
        streams_to_advance.push_back(current_newest_for_key.stream_index); // The newest version's stream also needs advancing

        while (!min_heap.empty() && min_heap.top().kv.key == current_key) {
            merge_entry older_version = min_heap.top();
            min_heap.pop();
            streams_to_advance.push_back(older_version.stream_index); // Collect stream index
        }

        // Process the newest version: Write if not tombstone
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

            // Fence Pointer Logic
            long long offset_for_this_key_fp;
            if (entry_start_offset != -1) {
                offset_for_this_key_fp = entry_start_offset;
            } else {
                std::cerr << "Warning: Skipped adding precise fence pointer for key " << current_newest_for_key.kv.key << " in " << output_filename << " due to failed tellp() at start of line. Using offset 0 as fallback." << std::endl;
                offset_for_this_key_fp = 0;
            }

            if (current_block_byte_count >= BLOCK_SIZE || merged_run_info.fence_pointers.empty()) {
                merged_run_info.fence_pointers.push_back({current_newest_for_key.kv.key, offset_for_this_key_fp});
                current_block_byte_count = line_byte_size;
            } else {
                current_block_byte_count += line_byte_size;
            }
        }

        // Advance streams for all versions collected in this batch
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
                    }
                } else {
                    if (!input_streams[stream_idx].eof() && input_streams[stream_idx].fail()) {
                        std::cerr << "Warning: Failed read from TXT file : " << runs_to_merge_info[stream_idx].filename << std::endl;
                    }
                    input_streams[stream_idx].close();
                }
            }
        }
    } 

    // Close output file
    outfile.close();
    if (!outfile) {
        std::cerr << "Error closing merged output TXT file properly: " << output_filename << std::endl;
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

    return merged_run_info;
}

// Check and trigger merges starting from a level
void lsm_tree::check_and_trigger_merge(int level_num) {
    if (level_num < 1 || level_num > MAX_LEVELS) {
        return;
    }

    level* current_level = levels_[level_num];
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
            return;
        }

        // Copy runs to merge info and filenames while holding the lock
        runs_to_merge_info.reserve(current_level->sstable_runs_.size());
        files_to_delete.reserve(current_level->sstable_runs_.size());
        for(const auto& info : current_level->sstable_runs_) {
            runs_to_merge_info.push_back(info);
            files_to_delete.push_back(info.filename);
        }

        // Clear the runs from the current level *now* while the lock is held.
        current_level->clear_runs();
    } // current_level_lock releases mutex. Files are still on disk at this point.

    // --- Phase 2: Launch the merge process (File I/O, adding to target, deleting old) asynchronously ---
    size_t estimated_n = BLOOM_FILTER_ESTIMATED_N_MERGE;

    // Capture necessary variables by value or const reference for the async lambda
    background_tasks_.push_back(std::async(std::launch::async,
        [this, level_num, target_level_num, runs_to_merge_info = std::move(runs_to_merge_info), files_to_delete = std::move(files_to_delete), estimated_n]() mutable {

        SSTableInfo merged_run_info = merge_runs(target_level_num, runs_to_merge_info, estimated_n);

        // --- Phase 3: Add the new merged run to the target level ---
        if (!merged_run_info.filename.empty()) {
            if (target_level_num > 0 && target_level_num <= MAX_LEVELS && levels_.size() > static_cast<size_t>(target_level_num) && levels_[target_level_num]) {
                { // Scoped lock for the target level
                    std::lock_guard<std::mutex> target_level_lock(levels_[target_level_num]->level_mutex_);
                    levels_[target_level_num]->add_run(std::move(merged_run_info));
                } // target_level_lock releases mutex

                // --- Phase 4: Recursively check if the *next* level now needs merging ---
                if (target_level_num < MAX_LEVELS) { 
                    check_and_trigger_merge(target_level_num); 
                }

            } else {
                std::cerr << "Critical Error: Target level " << target_level_num << " became invalid in async merge task. Cannot add merged run. File " << merged_run_info.filename << " created but not added to tree." << std::endl;
            }

            // --- Phase 5: Delete old files ---
            delete_sst_files(files_to_delete); // uses file_delete_mutex_

        } else {
            std::cerr << "Error: Merge failed into level " << target_level_num << " in async task. Output file not created. Files from L" << level_num << " were cleared from memory BUT NOT deleted from disk. Manual cleanup of: ";
            for(const auto& fname : files_to_delete) std::cerr << fname << " ";
            std::cerr << "may be required." << std::endl;
        }
    }));
}

// Internal search function used by both public get and range
std::optional<key_value> lsm_tree::getValueForKey(int key) const {
    int value;
    bool is_tombstone;
    // 1. Check Memtable first (sequentially, fastest path)
    // memtable::find_key handles its own lock.
    if (memtable_ptr_->find_key(key, value, is_tombstone)) {
        if (is_tombstone) {
            return std::nullopt;
        }
        return std::make_optional<key_value>({key, value, false});
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
    int newest_level_num = MAX_LEVELS + 1;

    for (auto& pair : level_futures) {
        // .get() waits for the future to complete and retrieves its result
        std::optional<std::tuple<int, int, bool, int>> result = pair.second.get();

        if (result.has_value()) {
            auto [res_key, res_value, res_tombstone, res_level_num] = result.value();

            // The lower the level_num, the newer the data.
            if (res_level_num < newest_level_num) {
                newest_level_num = res_level_num;
                newest_found_kv = std::make_optional<key_value>({res_key, res_value, res_tombstone});
            }
        }
    }

    // Determine final result based on the newest entry found in levels
    if (newest_found_kv.has_value()) {
        if (newest_found_kv.value().tombstone) {
            return std::nullopt; 
        } else {
            return newest_found_kv; 
        }
    } else {
        return std::nullopt;
    }
}

// --- Public Interface Implementation ---
bool lsm_tree::insert(key_value kv_pair) {
    // Check if shutdown is requested before attempting insert
    if (shutdown_requested_.load()) {
        return false;
    }

    // Try inserting into memtable
    bool trigger_flush = false;
    // memtable::insert handles its own lock (memtable_mutex_) and updates cur_size_
    memtable_ptr_->insert(kv_pair, trigger_flush);

    // Check if the insertion triggered the need for a flush
    if (trigger_flush) {
        // Signal the background flusher thread
        { // Scoped lock for the flush signal flag
            std::lock_guard<std::mutex> lock(flush_mutex_);
            flush_needed_ = true;
        }

        // Notify the flusher thread's condition variable
        flush_request_cv_.notify_one();
    }
    return true;
}

int lsm_tree::get(int key, std::ostream& os) {
    std::optional<key_value> result = getValueForKey(key);
    std::lock_guard<std::mutex> cout_lock(cout_mutex_);

    if (result.has_value()) {
        const auto& kv = result.value();
        os << kv.value << std::endl;
        return kv.value;
    } else {
        os << std::endl;
        return -1;
    }
 
}

void lsm_tree::range(int start, int end, std::ostream& os) {
    // Use a map to collect the most recent values for each key within the range.
    std::map<int, key_value> results_map;

    // 1. Scan the in-memory memtable (newest data source)
    {
        std::lock_guard<std::mutex> memtable_lock(memtable_ptr_->memtable_mutex_);

        auto it_low = memtable_ptr_->memtable_.lower_bound(start);

        for (auto it = it_low; it != memtable_ptr_->memtable_.end() && it->first <= end; ++it) {
            results_map.emplace(it->first, it->second);
        }
    }

    // 2. Scan levels (from L1 upwards, newest levels first)
    for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
        level* current_level = levels_[level_num];
        if (!current_level) continue;

        std::vector<SSTableInfo> sstables_to_scan_info;
        {
            std::lock_guard<std::mutex> level_lock(current_level->level_mutex_);
            sstables_to_scan_info = current_level->sstable_runs_;
        }

        // Within a level, process newer SSTables first.
        std::reverse(sstables_to_scan_info.begin(), sstables_to_scan_info.end());

        for (const auto& run_info : sstables_to_scan_info) {
            // Skip this file if its key range does not overlap with the query range [start, end]
            if (run_info.max_key < start || run_info.min_key > end) {
                continue;
            }

            std::ifstream infile(run_info.filename);
            if (!infile) {
                std::cerr << "Warning: Could not open SSTable TXT file for range query: " << run_info.filename << std::endl;
                continue;
            }

            // Use Fence Pointers to find the approximate start offset
            long long search_offset = 0;
            if (!run_info.fence_pointers.empty()) {
                // Find the first fence pointer whose key is >= start
                auto fp_it = std::lower_bound(run_info.fence_pointers.begin(), run_info.fence_pointers.end(), start,
                                             [](const std::pair<int, long long>& fp, int target_key){
                                                 return fp.first < target_key;
                                             });

                // If lower_bound is not the first element, go back one to get the fence pointer *before* the block start.
                if (fp_it != run_info.fence_pointers.begin()) {
                    --fp_it; 
                    search_offset = fp_it->second;
                } else {
                    // If start key is less than or equal to the first FP key, start from the beginning of the file.
                    search_offset = 0;
                }
            } else {
                 search_offset = 0;
            }

            // Seek the file stream to the calculated offset
            infile.seekg(search_offset);
             if (infile.fail()) {
                 std::cerr << "Warning: Failed to seek to offset " << search_offset << " in file " << run_info.filename << " during range query. Scanning from start." << std::endl;
                 infile.clear();
                 infile.seekg(0);
                 if (infile.fail()) {
                     std::cerr << "Error: Could not seek to start in file " << run_info.filename << ". Skipping file." << std::endl;
                     infile.close();
                     continue; // Skip this file
                 }
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
                    // If we've read past the end of the desired range, stop scanning this file.
                    if (current_key > end) {
                        break;
                    }

                    // If the key is within the desired range [start, end]
                    if (current_key >= start) { 
                        results_map.emplace(current_key, key_value{current_key, current_value, (tombstone_flag == 1)});
                    }
                } else {
                     std::cerr << "Warning: Parsing error during range scan in file: " << run_info.filename << ", line: '" << line << "'" << std::endl;
                }
            }
             if (!infile.eof() && infile.fail()) {
                  if (infile.bad()) {
                      std::cerr << "Error: Serious read error in SSTable TXT file: " << run_info.filename << " during range scan, flags: " << infile.rdstate() << std::endl;
                  }
             }
            infile.close();
        }
    }

    // 3. Collect final results into a vector and filter out tombstones
    std::vector<key_value> final_sorted_results;
    // Estimate size to avoid reallocations
    final_sorted_results.reserve(results_map.size());

    for (const auto& pair_entry : results_map) {
        const key_value& kv = pair_entry.second;
        if (!kv.tombstone) {
            final_sorted_results.push_back(kv);
        }
    }

    // 4. Lock the output stream for printing the entire range result
    std::lock_guard<std::mutex> cout_lock(cout_mutex_);

    os << "Range (" << start << " to " << end << "): ";
    // Iterate through the sorted vector and print the found key-value pairs
    for (const auto& kv : final_sorted_results) {
        os << kv.key << ":" << kv.value << " ";
    }
    os << std::endl;

}

void lsm_tree::delete_key(int key) {
    // Insert a tombstone entry for the key.
    insert({key, 0, true});
}

void lsm_tree::printStats(std::ostream& os) const {
    std::lock_guard<std::mutex> cout_lock(cout_mutex_); // Lock for the entire stats output
    os << "--- LSM Tree Stats ---" << std::endl;
    // Data structures to hold intermediate results
    std::map<int, std::pair<int, std::string>> logical_data;
    std::set<int> deleted_keys;                             
    std::vector<long long> physical_key_counts(MAX_LEVELS + 1, 0);

    // --- Stage 1: Process data from newest to oldest to find logical state ---

    // 1.a Process Memtable
    {
        std::lock_guard<std::mutex> memtable_lock(memtable_ptr_->memtable_mutex_);
        for (const auto& pair : memtable_ptr_->memtable_) {
            const auto& kv = pair.second;

            // If key already processed (found newer version in map itself or already marked deleted in map processing), skip
            if (logical_data.count(kv.key) || deleted_keys.count(kv.key)) {
                continue;
            }

            if (kv.tombstone) {
                deleted_keys.insert(kv.key);
            } else {
                logical_data[kv.key] = {kv.value, "M"}; 
            }
        }
    } 

    // 1.b Process Levels (from L1 down to MAX_LEVELS)
    for (int level_num = 1; level_num <= MAX_LEVELS; ++level_num) {
        level* current_level = levels_[level_num];
        if (!current_level) continue;

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

            while (std::getline(infile, line)) {
                if (line.empty()) continue;

                std::stringstream ss(line);
                int current_key, current_value, tombstone_flag;

                if (ss >> current_key >> current_value >> tombstone_flag) {
                    current_file_key_count++;
                    bool current_tombstone = (tombstone_flag == 1);

                    // Check if key already has a newer version or is known to be deleted
                    if (logical_data.count(current_key) || deleted_keys.count(current_key)) {
                        continue;
                    }

                    // This is the newest version encountered so far for this key
                    if (current_tombstone) {
                        deleted_keys.insert(current_key);
                    } else {
                        logical_data[current_key] = {current_value, "L" + std::to_string(level_num)}; 
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
    os << "Logical Pairs: " << logical_data.size() << std::endl;

    // (2) Keys Per Level (Physical count including tombstones/stale data in files)
    os << "LVL1: " << physical_key_counts[1];
    for (int i = 2; i <= MAX_LEVELS; ++i) {
        os << ", LVL" << i << ": " << physical_key_counts[i];
    }
    os << std::endl;

    // (3) Dump Tree (Logical view: Key:Value:Level)
    std::map<std::string, std::vector<std::pair<int, int>>> entries_by_location;

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
            os << std::endl;
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
    files_to_delete = levels_[i]->get_run_filenames(); 
    levels_[i]->clear_runs(); 
    } 
    // Delete the physical files after releasing the level lock
            delete_sst_files(files_to_delete);
            // Remove the level directory
            std::string level_dir = DATA_DIR + "/L" + std::to_string(i);
            if (rmdir(level_dir.c_str()) != 0) {
                if (errno != ENOTEMPTY && errno != ENOENT) { // ENOENT means it was already gone
                    std::cerr << "Warning: Could not remove directory " << level_dir << ": " << strerror(errno) << std::endl;
                }
            }
        }
    }
    // Remove the root data directory
    if (rmdir(DATA_DIR.c_str()) != 0) {
        if (errno != ENOTEMPTY && errno != ENOENT) { 
            std::cerr << "Warning: Could not remove root data directory " << DATA_DIR << ": " << strerror(errno) << std::endl;
        }
    }
    // Reset run ID generator
    std::lock_guard<std::mutex> id_lock(id_mutex_);
    next_run_id_ = 0;
}

// Public wrapper for load_file (loads a list of commands)
void lsm_tree::load(const std::string& fileName) {
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
                get(key, std::cout);
            } else if (command == "DELETE") {
                int key;
                ss >> key;
                delete_key(key);
            } else if (command == "RANGE") {
                int start, end;
                ss >> start >> end;
                range(start, end, std::cout);
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
}

void lsm_tree::flushThreadLoop() {
    while (true) {
        // Wait for a flush request or shutdown signal
        std::unique_lock<std::mutex> lock(flush_mutex_);
        flush_request_cv_.wait(lock, [this]{
            // Wait if shutdown is NOT requested AND flush is NOT needed
            return shutdown_requested_.load() || flush_needed_;
        });

        // Check for shutdown request AFTER waking up
        if (shutdown_requested_.load() && !flush_needed_) {
            break;
        }

        // If we woke up because flush_needed_ is true (or shutdown was requested and flush_needed_ was true)
        if (flush_needed_) {
             flush_needed_ = false;

            // Unlock the mutex while performing the potentially blocking I/O and other work
            lock.unlock();

            // --- Flush the memtable (Logic copied from original async lambda in insert) ---
            std::vector<key_value> data_to_flush = memtable_ptr_->flush();

            if (!data_to_flush.empty()) {
                // Generate a filename for the new run in Level 1 (acquires id_mutex_)
                std::string new_sstable_file = generate_sstable_filename(1);

                // Write the flushed data to the new SSTable file and get its info (including filter)
                SSTableInfo new_run_info = write_sstable(data_to_flush, new_sstable_file, data_to_flush.size());

                if (!new_run_info.filename.empty()) {
                    // Add the new run (info) to Level 1
                    if (levels_.size() > 1 && levels_[1]) {
                        // Need to lock Level 1 to add the run
                        std::lock_guard<std::mutex> l1_lock(levels_[1]->level_mutex_);
                        levels_[1]->add_run(std::move(new_run_info)); // add_run assumes lock held
                    } else {
                         std::cerr << "Critical Error: Level 1 is null or levels_ vector too small in flusher task. Cannot add flushed run." << std::endl;
                    }

                    // Check if Level 1 needs merging now
                    check_and_trigger_merge(1);

                } else {
                    std::cerr << "Error: Failed to write flushed memtable to disk in flusher task. Data potentially lost." << std::endl;
                }
             }
        }
    }
}