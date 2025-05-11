// test.cpp
#include "lsm_tree.hh"
#include <iostream>
#include <cassert> // For simple checks (optional, you can just print messages)
#include <vector>
#include <string>
#include <functional> // For std::function if you want a test helper wrapper

// Helper to print a test section header
void print_test_header(const std::string& name) {
    std::cout << "\n--- " << name << " ---" << std::endl;
}

// Simple assertion helper (prints messages instead of crashing by default)
void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "[PASS] " << message << std::endl;
    } else {
        std::cerr << "[FAIL] " << message << std::endl;
    }
}

int main() {
    std::cout << "Starting LSM Tree Bloom Filter & Fence Pointer Tests" << std::endl;

    // Ensure a clean slate by removing previous data directories
    print_test_header("Cleanup previous data");
    {
        // Create a temporary tree object just to call cleanup_files
        lsm_tree temp_tree;
        temp_tree.cleanup_files();
    }
    std::cout << "Cleanup complete." << std::endl;


    // --- Test 1: Basic Insert and Get (Memtable Only) ---
    print_test_header("Test 1: Basic Insert and Get (Memtable Only)");
    {
        lsm_tree tree;
        std::cout << "Inserting keys 1, 2, 3..." << std::endl;
        tree.insert({1, 10});
        tree.insert({2, 20});
        tree.insert({3, 30});

        std::cout << "Checking keys in Memtable:" << std::endl;
        check(tree.get(1) == 10, "Get(1) returns 10");
        check(tree.get(2) == 20, "Get(2) returns 20");
        check(tree.get(3) == 30, "Get(3) returns 30");
        check(tree.get(99) == -1, "Get(99) (non-existent) returns -1"); // Key not found

        // tree.printStats(); // Should show keys 1, 2, 3 in Memtable
    } // tree scope ends, destructor called, likely flushes memtable

    // --- Test 2: Flush to Level 1 and Get (SSTable & BF/FP) ---
    print_test_header("Test 2: Flush to Level 1 and Get (SSTable & BF/FP)");
    {
        lsm_tree tree; // Fresh tree. Previous memtable likely flushed by destructor.

        std::cout << "Inserting " << MEMTABLE_CAPACITY + 5 << " keys to force flush..." << std::endl;
        // Insert enough keys to guarantee a flush to Level 1
        for (int i = 100; i < 100 + MEMTABLE_CAPACITY; ++i) {
            tree.insert({i, i * 2});
        }
        // Insert a few more to definitely exceed capacity and trigger the flush logic
        for (int i = 100 + MEMTABLE_CAPACITY; i < 100 + MEMTABLE_CAPACITY + 5; ++i) {
             tree.insert({i, i * 2});
        }


        // tree.printStats(); // Should show a run in L1, Memtable should be (mostly) empty

        std::cout << "Checking keys that should be in the L1 SSTable:" << std::endl;
        // Test getting keys from the SSTable (should use BF/FP)
        check(tree.get(105) == 210, "Get(105) returns 210 (from L1 SSTable)");
        check(tree.get(100 + MEMTABLE_CAPACITY / 2) == (100 + MEMTABLE_CAPACITY / 2) * 2,
              "Get(mid-range key) returns correct value (from L1 SSTable)");
        check(tree.get(100 + MEMTABLE_CAPACITY + 4) == (100 + MEMTABLE_CAPACITY + 4) * 2,
              "Get(last key) returns correct value (from L1 SSTable)");

         // Test getting a key NOT in this SSTable (BF should filter)
         // This key is far outside the range [100, 100+MEMTABLE_CAPACITY+4]
        std::cout << "Checking a key definitely not in the L1 SSTable (BF negative test):" << std::endl;
        check(tree.get(5000) == -1, "Get(5000) (non-existent) returns -1 (BF should filter)");

        // Test a key from Test 1 that might have been flushed by the destructor.
        // The status depends on the order, but searching should still work.
        // printStats might show it in L1 or L2 depending on if Test 1's flush caused a merge.
        // We won't strictly check the value here, just that get doesn't crash and ideally returns -1.
        std::cout << "Checking a key from previous run (may or may not exist):" << std::endl;
        tree.get(1); // Just call it to see output, don't strictly assert
        tree.get(2);

    } // tree destroyed, flushing again

    // --- Test 3: Overwrite and Delete in Memtable and resulting SSTable ---
    print_test_header("Test 3: Overwrite and Delete");
    {
        lsm_tree tree; // Fresh tree, loads existing data

        std::cout << "Inserting key 5, then overwriting..." << std::endl;
        tree.insert({5, 50}); // Original value in memtable
        tree.insert({6, 60}); // Original value in memtable

        // Force flush 1: {5:50}, {6:60} go to L1
        std::cout << "Force flush 1..." << std::endl;
        for(int i = 200; i < 200 + MEMTABLE_CAPACITY; ++i) tree.insert({i, i});
        tree.insert({200 + MEMTABLE_CAPACITY, 999});

        tree.printStats(); // Should show {5:50}, {6:60} (or similar) in L1

        std::cout << "Overwriting key 5, deleting key 6 in memtable..." << std::endl;
        tree.insert({5, 55}); // Overwrite 5 in memtable
        tree.delete_key(6);   // Delete 6 in memtable (tombstone)

        std::cout << "Checking keys from memtable:" << std::endl;
        check(tree.get(5) == 55, "Get(5) returns 55 (newest from memtable)");
        check(tree.get(6) == -1, "Get(6) returns -1 (deleted in memtable)");

        // Force flush 2: {5:55}, {6:tombstone} go to L1 (potentially merging with previous L1)
        std::cout << "Force flush 2..." << std::endl;
        for(int i = 300; i < 300 + MEMTABLE_CAPACITY; ++i) tree.insert({i, i});
        tree.insert({300 + MEMTABLE_CAPACITY, 888});

        tree.printStats(); // Should show {5:55}, {6:tombstone} (or merged) in L1 or L2

        std::cout << "Checking keys after flush/merge:" << std::endl;
        // Get should find the newest versions in the SSTables
        check(tree.get(5) == 55, "Get(5) returns 55 (from SSTable)");
        check(tree.get(6) == -1, "Get(6) returns -1 (tombstone in SSTable)");

        // Check a key from the first flush SSTable (now potentially merged)
        check(tree.get(205) == 205, "Get(205) returns 205 (from older SSTable)");

    } // tree destroyed, flushing remaining

    // --- Test 4: Simple Merge to Level 2 ---
    print_test_header("Test 4: Simple Merge to Level 2");
    {
         lsm_tree tree; // Fresh tree, loads existing data

         std::cout << "Inserting keys to trigger merge from L1 to L2 (needs >=" << SIZE_RATIO << " runs in L1)..." << std::endl;

         // Create runs in L1 until the merge threshold (SIZE_RATIO) is met
         // Each loop iteration creates one run
         for (int r = 0; r < SIZE_RATIO; ++r) {
             std::cout << " Creating run " << r + 1 << "..." << std::endl;
             // Insert keys for this run (use distinct ranges for clarity, though overlap is fine)
             for (int i = r * 1000 + 1; i <= r * 1000 + 50; ++i) { // Insert 50 keys
                 tree.insert({i, i * 10});
             }
             // Trigger flush for this run (insert MEMTABLE_CAPACITY+1 garbage keys)
             for (int f = 0; f < MEMTABLE_CAPACITY + 1; ++f) {
                 tree.insert({80000 + r*1000 + f, 1}); // Use high keys to avoid conflicting with main test keys
             }
              // Give it a moment for potential background merge if implemented async (not the case here)
         }

         tree.printStats(); // Should show runs in L2 (the result of L1 merging)

         std::cout << "Checking keys from the merged L2 SSTable:" << std::endl;
         // Test getting keys that were merged into L2
         check(tree.get(1) == 10, "Get(1) returns 10 (from L2 merged run)");

         check(tree.get(1001) == 10010, "Get(1001) returns 10010 (from L2 merged run)");
         check(tree.get(4050) == 40500, "Get(4050) returns 40500 (from L2 merged run)");
         // Test a key not expected to be in the merged L2 (BF should filter)
         std::cout << "Checking a key definitely not in L2 (BF negative test):" << std::endl;
         check(tree.get(77777) == -1, "Get(77777) (non-existent) returns -1 (BF should filter)");

    } // tree destroyed

     // --- Test 5: Restart and Load from Disk (Verifies rebuild_run_info) ---
    print_test_header("Test 5: Restart and Load from Disk");
    {
        // Create a new tree. This will trigger the loading logic (rebuild_run_info)
        // for files created by previous tests, including their BFs and FPs.
        lsm_tree tree;

        // tree.printStats(); // Should show keys loaded from disk

        std::cout << "Checking keys loaded from disk (using rebuilt BF/FP):" << std::endl;
        // Test getting keys known to be in files from previous tests
        check(tree.get(1) == 10, "Get(1) returns 10 (loaded from disk)"); // From Test 4 merge
        check(tree.get(105) == 210, "Get(105) returns 210 (loaded from disk)"); // From Test 2 L1 flush
        std::cout << tree.get(5) << std::endl;
        check(tree.get(5) == 55, "Get(5) returns 55 (loaded from disk - overwritten)"); // From Test 3 overwrite
        std::cout << tree.get(6) << std::endl;
        check(tree.get(6) == -1, "Get(6) returns -1 (loaded from disk - deleted)"); // From Test 3 delete

        // Test a key not in the loaded data (BF should filter)
        std::cout << "Checking a key definitely not loaded (BF negative test):" << std::endl;
        check(tree.get(98765) == -1, "Get(98765) (non-existent) returns -1 (loaded BF should filter)");

    } // tree destroyed

    std::cout << "\nLSM Tree Bloom Filter & Fence Pointer Tests Complete." << std::endl;

    return 0;
}