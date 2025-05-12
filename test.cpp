#include "lsm_tree.hh" // Include the LSM tree header
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <random>
#include <map> // To verify state
#include <set> // To track deleted keys

// Define constants from lsm_tree.hh if not included by it
// (Better to ensure lsm_tree.hh defines/includes necessary constants)
// #define MEMTABLE_CAPACITY 10
// #define INITIAL_LEVEL_CAPACITY 2
// #define SIZE_RATIO 4
// #define MAX_LEVELS 3
// const int BLOCK_SIZE = 100; // Approximate block size in bytes
// const std::string SST_FILE_PREFIX = "run_";
// const std::string SST_FILE_SUFFIX = ".sst";
// const double BLOOM_FILTER_FALSE_POSITIVE_RATE = 0.01;
// const size_t BLOOM_FILTER_ESTIMATED_N_FLUSH = MEMTABLE_CAPACITY; // Estimated keys in a flush
// const size_t BLOOM_FILTER_ESTIMATED_N_MERGE = 100; // Heuristic for merged runs

// Define test parameters
const int NUM_THREADS = 5;
const int OPS_PER_THREAD = 500;
const int KEY_RANGE_START = 1000;
const int KEY_RANGE_END = KEY_RANGE_START + NUM_THREADS * 200; // Overlapping key ranges

// --- Worker Thread Function ---
void worker_thread(lsm_tree* tree, int thread_id, int start_key, int end_key, int num_ops) {
    std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count() + thread_id);
    std::uniform_int_distribution<int> key_dist(start_key, end_key);
    std::uniform_int_distribution<int> op_dist(0, 100); // 0-50: Insert/Update, 51-70: Delete, 71-100: Get

    std::cout << "Thread " << thread_id << " starting operations in key range [" << start_key << ", " << end_key << "]" << std::endl;

    for (int i = 0; i < num_ops; ++i) {
        int key = key_dist(rng);
        int op_type = op_dist(rng);

        try {
            if (op_type <= 50) { // Insert/Update
                int value = thread_id * 1000 + key; // Value includes thread ID for tracking
                tree->insert({key, value, false});
                // std::cout << "Thread " << thread_id << ": Inserted/Updated key " << key << " with value " << value << std::endl; // Too verbose
            } else if (op_type <= 70) { // Delete
                tree->delete_key(key);
                // std::cout << "Thread " << thread_id << ": Deleted key " << key << std::endl; // Too verbose
            } else { // Get
                // Note: Get prints to cout in your current get implementation
                // For concurrent testing, it's better to capture output or modify get
                // For this simple test, we'll just call get and rely on printStats for verification
                tree->get(key, std::cout); // Output to cout, not range mode
                // Consider making `get` return a struct or optional<pair<int, bool>>
                // rather than printing, for easier test verification.
            }
        } catch (const std::exception& e) {
            std::cerr << "Thread " << thread_id << " ERROR: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Thread " << thread_id << " ERROR: Unknown exception." << std::endl;
        }

        // Small sleep to potentially increase context switching
        // std::this_thread::sleep_for(std::chrono::microseconds(10)); // Optional: can make races more likely
    }

    std::cout << "Thread " << thread_id << " finished." << std::endl;
}

int main() {
    std::cout << "LSM Tree Concurrency Test" << std::endl;

    // Clean up any previous test data
    lsm_tree cleanup_tree; // Create a temporary tree just for cleanup
    cleanup_tree.cleanup_files();
    // The temporary cleanup_tree goes out of scope and is destroyed

    // Create the main LSM tree instance
    lsm_tree db;

    // Create and launch worker threads
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        int start_key = KEY_RANGE_START + i * 100; // Overlapping ranges
        int end_key = start_key + 150;
        threads.emplace_back(worker_thread, &db, i + 1, start_key, end_key, OPS_PER_THREAD);
    }

    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "\nAll threads finished. Performing final verification." << std::endl;

    // --- Verification ---
    // The most reliable verification for concurrent writes is to check the final state.
    // We can do this by iterating through the expected key range and using get().
    // However, your get function prints directly.
    // A simpler verification is to print stats and spot-check.

    std::cout << "\n--- Final LSM Tree State ---" << std::endl;
    db.printStats(std::cout);
    std::cout << "--------------------------" << std::endl;

    // Basic spot checks using get
    std::cout << "\n--- Spot Checks ---" << std::endl;
    for (int key = KEY_RANGE_START; key <= KEY_RANGE_END; ++key) {
       // Check a few keys across the range
       if (key % 50 == 0) {
           std::cout << "Checking key " << key << ": ";
           db.get(key, std::cout); // get prints value or newline
       }
    }
    std::cout << "-------------------" << std::endl;

    // A more rigorous check would track the expected final state based on timestamps
    // or a logical sequence of operations, but that's complex.
    // For this simple test, we rely on printStats showing a consistent logical count
    // and spot checks not crashing or showing obvious garbage.

    std::cout << "\nTest complete." << std::endl;

    // The `db` object is destroyed when main exits, triggering the destructor.
    // cleanup_files() was called at the start for a clean state.

    return 0;
}