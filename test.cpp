// test_lsm.cpp
#include "lsm_tree.hh" // Include your LSM tree header
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <mutex>
#include <atomic>
#include <sstream>

// Use a mutex for controlling access to std::cout from multiple threads
std::mutex cout_mutex;

// Writer thread function
void writer_task(lsm_tree* tree, int start_key, int num_keys_to_insert, std::atomic<int>& inserts_completed) {
    // std::cerr << "DEBUG: Writer thread started, inserting " << num_keys_to_insert << " keys starting from " << start_key << std::endl; // Debug
    for (int i = 0; i < num_keys_to_insert; ++i) {
        int key = start_key + i;
        int value = key * 100; // Simple value relation
        key_value kv(key, value, false);

        // The insert call will be synchronous until memtable is full,
        // then it will launch an async flush and immediately return.
        // This is exactly what we want to test concurrency against.
        if (tree->insert(kv)) {
            inserts_completed++;
            // std::lock_guard<std::mutex> lock(cout_mutex);
            // std::cout << "Writer: Inserted " << key << std::endl;
        } else {
             // This else block should ideally not be hit with the async flush logic.
             // If insert returns false after async flush is launched, it indicates
             // a problem with freeing memtable space or immediate retry logic.
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Writer: Failed to insert key " << key << " even after flush attempt." << std::endl;
        }

        // Add a small delay to simulate real-world operations and avoid pegging CPU
        // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // std::cerr << "DEBUG: Writer thread finished." << std::endl; // Debug
}

// Reader thread function
void reader_task(lsm_tree* tree, int key_range_start, int key_range_end, std::atomic<int>& gets_completed) {
    std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count()); // Seed random number generator
    std::uniform_int_distribution<int> dist(key_range_start, key_range_end); // Distribution within the key range

    // std::cerr << "DEBUG: Reader thread started, querying keys between " << key_range_start << " and " << key_range_end << std::endl; // Debug

    auto start_time = std::chrono::high_resolution_clock::now();
    const auto run_duration = std::chrono::seconds(10); // Run reader for 10 seconds

    while (std::chrono::high_resolution_clock::now() - start_time < run_duration) {
        int key_to_get = dist(rng); // Get a random key within the range

        // Use a temporary stringstream to capture output to avoid locking cout for too long
        std::stringstream ss;
        ss << "Reader: Getting key " << key_to_get << " -> ";

        // The get call performs parallel search across levels
        // This tests if search is responsive while flushes/merges happen
        int value = tree->get(key_to_get, ss); // get function locks cout_mutex internally

        ss << "Value: " << (value != -1 ? std::to_string(value) : "Not Found") << std::endl;

        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << ss.str(); // Print captured output
        }

        gets_completed++;

        // Small delay
        // std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
     // std::cerr << "DEBUG: Reader thread finished." << std::endl; // Debug
}


int main() {
    std::cout << "Starting LSM Tree Concurrency Test" << std::endl;

    // Ensure the data directory is clean before starting
    // Note: If you are using `cleanup_files` in the destructor, this might be redundant,
    // but explicit cleanup is good for tests.
    {
         lsm_tree temp_tree; // Create a temporary instance just to call cleanup
         temp_tree.cleanup_files(); // Deletes existing files
    }


    lsm_tree tree; // The main LSM tree instance

    const int total_keys_to_insert = 200; // Enough to trigger multiple flushes (MEMTABLE_CAPACITY=50 -> 4 flushes)
    const int writer_key_start = 1000;
    const int reader_key_range_start = 1000;
    const int reader_key_range_end = writer_key_start + total_keys_to_insert - 1; // Reader queries within the writer's range

    std::atomic<int> inserts_completed(0);
    std::atomic<int> gets_completed(0);


    std::cout << "Launching writer thread..." << std::endl;
    std::thread writer_t(writer_task, &tree, writer_key_start, total_keys_to_insert, std::ref(inserts_completed));

    // Give the writer a moment to start inserting some data before readers begin
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "Launching reader thread..." << std::endl;
    std::thread reader_t(reader_task, &tree, reader_key_range_start, reader_key_range_end, std::ref(gets_completed));

    // You could add more reader/writer/deleter threads here if desired

    // Wait for threads to complete
    writer_t.join();
    reader_t.join(); // Reader runs for a fixed duration

    std::cout << "\nAll test threads finished." << std::endl;
    std::cout << "Total inserts attempted by writer: " << total_keys_to_insert << std::endl;
    std::cout << "Total inserts completed (reported by writer): " << inserts_completed << std::endl; // Should match total_keys_to_insert if memtable accepts them
    std::cout << "Total get operations attempted by reader: " << gets_completed << std::endl;


    // Wait a bit longer for any *remaining* background flush/merge tasks to complete
    // This is important because `check_and_trigger_merge` might launch subsequent merges
    // *after* the threads join. The destructor *will* wait, but an explicit wait here
    // before printing final stats might be helpful if you want to see the tree's state
    // *after* all known merges triggered by the test have finished.
    std::cout << "Waiting for final background tasks (e.g., merges) to complete..." << std::endl;
    // A heuristic wait time, or add a mechanism to track *all* async tasks
    // The destructor handles the proper waiting via background_tasks_ vector.
    // std::this_thread::sleep_for(std::chrono::seconds(5)); // Optional manual wait


    std::cout << "\nFinal LSM Tree Stats:" << std::endl;
    tree.printStats(std::cout); // printStats locks cout_mutex internally

    std::cout << "Test finished." << std::endl;

    // The LSM tree destructor will implicitly wait for background_tasks_ and then delete files.
    // If you prefer explicit cleanup via the command handler, you could call `tree.cleanup_files()` here
    // instead of using the temporary tree at the start.

    return 0;
}