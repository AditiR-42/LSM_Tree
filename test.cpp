#include "lsm_tree.hh"
#include <iostream>
#include <vector>
#include <string>
#include <cassert> // Use assert for critical internal checks if desired, but relying on return values here

// Helper function to run a test case and report results
// Returns true if the test passes, false otherwise.
bool run_test_case(const std::string& name, bool condition) {
    std::cout << "  Test: " << name << " ... " << std::flush;
    if (condition) {
        std::cout << "PASS" << std::endl;
        return true;
    } else {
        std::cout << "FAIL" << std::endl;
        return false;
    }
}

// Test suite for Put and Get operations
bool test_put_get(lsm_tree& db) {
    std::cout << "--- Testing Put/Get ---" << std::endl;
    bool success = true;
    int val;
    bool test_result;

    // 1. Simple insert and get
    db.insert({10, 100});
    val = db.get(10); // Note: get() also prints, we check return value
    test_result = run_test_case("Insert (10, 100) & Get", val == 100);
    success &= test_result;

    // 2. Get non-existent key
    val = db.get(999);
    test_result = run_test_case("Get non-existent key (999)", val == -1);
    success &= test_result;

    // 3. Insert multiple keys
    db.insert({20, 200});
    db.insert({5, 50});
    db.insert({15, 150});

    val = db.get(20);
    test_result = run_test_case("Get key 20 after multiple inserts", val == 200);
    success &= test_result;

    val = db.get(5);
    test_result = run_test_case("Get key 5 after multiple inserts", val == 50);
    success &= test_result;

    val = db.get(15);
    test_result = run_test_case("Get key 15 after multiple inserts", val == 150);
    success &= test_result;

    // 4. Update existing key
    db.insert({10, 101}); // Update key 10
    val = db.get(10);
    test_result = run_test_case("Update key 10 to 101 & Get", val == 101);
    success &= test_result;

    // 5. Insert enough keys to likely trigger memtable flush (adjust based on MEMTABLE_CAPACITY)
    std::cout << "  Test: Triggering potential memtable flush..." << std::endl;
    // Assuming MEMTABLE_CAPACITY is 10. Insert 10 more unique keys.
    for (int i = 0; i < MEMTABLE_CAPACITY + 2; ++i) {
         db.insert({1000 + i, 10000 + i});
    }
    std::cout << "      (Flush potentially occurred)" << std::endl;

    // Check a key that should have been flushed
    val = db.get(1000);
    test_result = run_test_case("Get key 1000 (potentially flushed)", val == 10000);
    success &= test_result;

    // Check a key likely still in memtable
    val = db.get(1000 + MEMTABLE_CAPACITY + 1);
     test_result = run_test_case("Get key 1011 (likely in memtable)", val == 10000 + MEMTABLE_CAPACITY + 1);
    success &= test_result;

    // Check original updated key after flush
     val = db.get(10);
    test_result = run_test_case("Get key 10 after potential flush", val == 101);
    success &= test_result;

    return success;
}

// Test suite for Delete operations
bool test_delete(lsm_tree& db) {
    std::cout << "\n--- Testing Delete ---" << std::endl;
    bool success = true;
    int val;
    bool test_result;

    // 1. Insert a key then delete it
    db.insert({50, 500});
    val = db.get(50);
    test_result = run_test_case("Setup: Insert (50, 500) & Get", val == 500);
    success &= test_result;

    db.delete_key(50);
    val = db.get(50); // Should now be tombstoned
    test_result = run_test_case("Delete key 50 & Get", val == -1);
    success &= test_result;

    // 2. Delete non-existent key
    db.delete_key(888);
    val = db.get(888); // Should remain non-existent / tombstoned if implemented that way
    test_result = run_test_case("Delete non-existent key 888 & Get", val == -1);
    success &= test_result;

    // 3. Insert, Delete, then Re-insert
    db.insert({60, 600});
    db.delete_key(60);
    val = db.get(60); // Confirm deleted
    test_result = run_test_case("Setup: Insert (60, 600), Delete & Get", val == -1);
    success &= test_result;

    db.insert({60, 601}); // Re-insert with new value
    val = db.get(60);
    test_result = run_test_case("Re-insert key 60 with value 601 & Get", val == 601);
    success &= test_result;

    // 4. Ensure delete persists after potential flush (add more data)
    db.insert({70, 700});
    db.delete_key(70);
    std::cout << "  Test: Triggering potential flush after delete..." << std::endl;
    for (int i = 0; i < MEMTABLE_CAPACITY + 2; ++i) {
         db.insert({2000 + i, 20000 + i});
    }
    std::cout << "      (Flush potentially occurred)" << std::endl;

    val = db.get(70); // Check the deleted key again
    test_result = run_test_case("Get deleted key 70 after potential flush", val == -1);
    success &= test_result;

    return success;
}

// Test suite for Range operations
// NOTE: Verification relies on visual inspection of the output printed by db.range()
bool test_range(lsm_tree& db) {
    std::cout << "\n--- Testing Range (Visual Inspection Needed) ---" << std::endl;
    bool success = true;

    // Setup: Insert keys in and out of range [25, 35]
    db.insert({25, 250});
    db.insert({30, 300});
    db.insert({35, 350});
    db.insert({28, 280});
    db.insert({20, 209}); // Outside low
    db.insert({40, 409}); // Outside high
    db.insert({32, 320});
    db.delete_key(30);    // Delete one key in the range
    db.insert({35, 351}); // Update a key at the end of range

    std::cout << "  Test: Calling range(25, 35). Expecting:" << std::endl;
    std::cout << "        Range (25 to 35): 25:250 28:280 32:320 35:351 " << std::endl; // Expected output
    std::cout << "        Actual Output: ";
    db.range(25, 35); // Function prints directly
    std::cout << "      (Compare actual output above with expected)" << std::endl;

    // Add a basic check: does the function run without crashing?
    success &= run_test_case("Range function execution", true); // Simple check it ran

    return success; // Success here only means it ran; correctness is visual
}

// Test suite for PrintStats
// NOTE: Verification relies on visual inspection of the output printed by db.printStats()
bool test_stats(lsm_tree& db) {
    std::cout << "\n--- Testing printStats (Visual Inspection Needed) ---" << std::endl;
    bool success = true;

    std::cout << "  Test: Calling printStats(). Inspect output below:" << std::endl;
    std::cout << "vvvvvvvvvvvvvvvvvvvvvvvvvvvv" << std::endl;
    db.printStats(); // Function prints directly
    std::cout << "^^^^^^^^^^^^^^^^^^^^^^^^^^^^" << std::endl;
    std::cout << "      (Inspect stats output above for plausibility)" << std::endl;

    // Add a basic check: does the function run without crashing?
    success &= run_test_case("printStats function execution", true); // Simple check it ran

    return success; // Success here only means it ran; correctness is visual
}


int main() {
    lsm_tree* db = new lsm_tree();
    bool overall_success = true;

    std::cout << "===========================" << std::endl;
    std::cout << "Starting LSM Tree Tests" << std::endl;
    std::cout << "===========================" << std::endl;

    // Clean up any potential leftover files from previous runs BEFORE testing
    std::cout << "Initial cleanup..." << std::endl;
    db->cleanup_files();

    // Run test suites
    overall_success &= test_put_get(*db);
    overall_success &= test_delete(*db);
    overall_success &= test_range(*db);
    overall_success &= test_stats(*db); // Call stats again after more operations

    // Final cleanup AFTER testing
    std::cout << "\nFinal cleanup..." << std::endl;
    db->cleanup_files();

    delete db; // Clean up the LSM tree object itself

    std::cout << "===========================" << std::endl;
    if (overall_success) {
        std::cout << "Overall Result: ALL TESTS PASSED (Check visual output for Range/Stats)" << std::endl;
         std::cout << "===========================" << std::endl;
        return 0; // Exit code 0 indicates success
    } else {
        std::cout << "Overall Result: SOME TESTS FAILED" << std::endl;
         std::cout << "===========================" << std::endl;
        return 1; // Exit code non-zero indicates failure
    }
}