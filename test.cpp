#include "lsm_tree.hh"
#include <iostream>
#include <vector>
#include <string>
#include <cstdlib> // For exit() if needed

// Helper function to run a test case and report results
// Returns true if the test passes, false otherwise.
bool run_test_case(const std::string& name, bool condition, bool fatal = false) {
    std::cout << "  Test: " << name << " ... " << std::flush;
    if (condition) {
        std::cout << "PASS" << std::endl;
        return true;
    } else {
        std::cout << "FAIL" << std::endl;
        if (fatal) {
            std::cerr << "    FATAL ERROR: Cannot continue test." << std::endl;
            // Consider exiting if a fundamental persistence step fails
            // exit(1);
        }
        return false;
    }
}

// --- Keep original test suites (modified slightly for context) ---

// Test suite for Put and Get operations (Used for initial population)
bool populate_for_persistence(lsm_tree& db) {
    std::cout << "--- Populating DB Instance for Persistence Test ---" << std::endl;
    bool success = true;

    // Insert some initial data
    success &= run_test_case("Insert (10, 100)", db.insert({10, 100}));
    success &= run_test_case("Insert (20, 200)", db.insert({20, 200}));
    success &= run_test_case("Insert (5, 50)", db.insert({5, 50}));
    success &= run_test_case("Get key 5", db.get(5) == 50);

    // Update a key
    success &= run_test_case("Update key 10 -> 101", db.insert({10, 101}));
    success &= run_test_case("Get updated key 10", db.get(10) == 101);

    // Delete a key
    success &= run_test_case("Insert (50, 500)", db.insert({50, 500}));
    db.delete_key(50);
    success &= run_test_case("Get deleted key 50", db.get(50) == -1);

    // Insert enough to cause flushes/merges (adjust count based on constants)
    std::cout << "  Info: Inserting more data to trigger potential flush/merge..." << std::endl;
    for (int i = 0; i < (MEMTABLE_CAPACITY * SIZE_RATIO) + 5; ++i) {
         if (!db.insert({1000 + i, 10000 + i})) {
             success = false; // Check insert success
             std::cerr << "    ERROR during bulk insert!" << std::endl;
             break;
         }
    }
     // Verify a flushed key and the updated key after flush
    success &= run_test_case("Get flushed key 1000", db.get(1000) == 10000);
    success &= run_test_case("Get updated key 10 (after flush)", db.get(10) == 101);
     // Verify the deleted key persists after flush
     success &= run_test_case("Get deleted key 50 (after flush)", db.get(50) == -1);
     std::cout << "  Info: Population phase complete." << std::endl;

    // Optional: Print stats before closing this instance
    std::cout << "  Stats before closing instance:" << std::endl;
    db.printStats();


    return success;
}

// Test suite specifically for verifying data loaded from disk
bool verify_reloaded_data(lsm_tree& db) {
    std::cout << "\n--- Verifying Reloaded Data ---" << std::endl;
    bool success = true;
    int val;

    // Check keys that should have been persisted from the previous instance
    val = db.get(5);
    success &= run_test_case("Reloaded Get key 5", val == 50, true); // Fatal if basic reload fails

    val = db.get(10);
    success &= run_test_case("Reloaded Get updated key 10", val == 101, true);

    val = db.get(20);
    success &= run_test_case("Reloaded Get key 20", val == 200);

    val = db.get(50);
    success &= run_test_case("Reloaded Get deleted key 50", val == -1);

    val = db.get(1000);
    success &= run_test_case("Reloaded Get flushed key 1000", val == 10000);

    int last_bulk_key = 1000 + (MEMTABLE_CAPACITY * SIZE_RATIO) + 5 - 1;
    val = db.get(last_bulk_key);
     success &= run_test_case("Reloaded Get last bulk key", val == 10000 + (last_bulk_key - 1000));

    val = db.get(999); // Should still not exist
    success &= run_test_case("Reloaded Get non-existent key 999", val == -1);

    // Optional: Print stats after loading
    std::cout << "  Stats after reloading instance:" << std::endl;
    db.printStats();

    return success;
}

// Optional: Test modifying data *after* reloading
bool test_modification_after_reload(lsm_tree& db) {
    std::cout << "\n--- Testing Modifications After Reload ---" << std::endl;
    bool success = true;
    int val;

    // Add a new key
    success &= run_test_case("Insert NEW key 777", db.insert({777, 7770}));
    val = db.get(777);
    success &= run_test_case("Get NEW key 777", val == 7770);

    // Update a previously loaded key
    success &= run_test_case("Update loaded key 20 -> 202", db.insert({20, 202}));
    val = db.get(20);
    success &= run_test_case("Get updated loaded key 20", val == 202);

    // Delete another previously loaded key
    db.delete_key(5);
    val = db.get(5);
    success &= run_test_case("Delete loaded key 5 & Get", val == -1);

    // Check that unrelated keys are still correct
    val = db.get(10);
    success &= run_test_case("Check unrelated key 10", val == 101);

    std::cout << "  Stats after modification:" << std::endl;
    db.printStats();

    return success;
}


int main() {
    lsm_tree* db_handle = nullptr; // Use one handle, create/delete as needed
    bool overall_success = true;

    std::cout << "=======================================" << std::endl;
    std::cout << "Starting LSM Tree Persistence Tests" << std::endl;
    std::cout << "=======================================" << std::endl;

    // --- Initial Cleanup ---
    // Create a temporary instance just to call cleanup_files()
    std::cout << "--- Phase 0: Initial Cleanup ---" << std::endl;
    try {
        db_handle = new lsm_tree();
        std::cout << "  Running cleanup_files()..." << std::endl;
        db_handle->cleanup_files();
        delete db_handle;
        db_handle = nullptr;
        std::cout << "  Cleanup finished." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "  ERROR during initial cleanup: " << e.what() << std::endl;
        // Decide if fatal. If directories couldn't be made/deleted, maybe stop.
        overall_success = false;
        // return 1; // Exit if cleanup is critical for test start
    }


    // --- Phase 1: Populate and Save ---
    if (overall_success) {
        std::cout << "\n--- Phase 1: Populate and Save ---" << std::endl;
        try {
            db_handle = new lsm_tree(); // Creates directories if needed
            std::cout << "  Created db instance 1." << std::endl;
            overall_success &= populate_for_persistence(*db_handle);
            std::cout << "  Deleting db instance 1 (simulating close)..." << std::endl;
            delete db_handle;
            db_handle = nullptr;
            std::cout << "  Instance 1 deleted." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "  ERROR during Phase 1: " << e.what() << std::endl;
            overall_success = false;
            if (db_handle) delete db_handle; // Ensure cleanup if error occurred mid-phase
            db_handle = nullptr;
        }
    }

    // --- Phase 2: Reload and Verify ---
    if (overall_success) {
        std::cout << "\n--- Phase 2: Reload and Verify ---" << std::endl;
        try {
             // Creating this instance SHOULD trigger loading from disk files
            db_handle = new lsm_tree();
            std::cout << "  Created db instance 2 (loading from disk...)" << std::endl;
            overall_success &= verify_reloaded_data(*db_handle);

            // Optional: Test modifications after reload
            overall_success &= test_modification_after_reload(*db_handle);

            std::cout << "  Deleting db instance 2..." << std::endl;
            delete db_handle;
            db_handle = nullptr;
            std::cout << "  Instance 2 deleted." << std::endl;

        } catch (const std::exception& e) {
            std::cerr << "  ERROR during Phase 2: " << e.what() << std::endl;
            overall_success = false;
            if (db_handle) delete db_handle;
            db_handle = nullptr;
        }
    } else {
         std::cout << "\n--- Phase 2: SKIPPED due to previous errors ---" << std::endl;
    }

    // --- Final Cleanup ---
    // Always attempt cleanup, even if tests failed, unless cleanup itself failed initially
    std::cout << "\n--- Phase 3: Final Cleanup ---" << std::endl;
     try {
        db_handle = new lsm_tree(); // Create one last time for cleanup
        std::cout << "  Running final cleanup_files()..." << std::endl;
        db_handle->cleanup_files();
        delete db_handle;
        db_handle = nullptr;
        std::cout << "  Final cleanup finished." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "  ERROR during final cleanup: " << e.what() << std::endl;
        // Don't change overall_success here, just report
    }


    // --- Report Final Result ---
    std::cout << "\n=======================================" << std::endl;
    if (overall_success) {
        std::cout << "Overall Result: ALL PERSISTENCE TESTS PASSED" << std::endl;
         std::cout << "=======================================" << std::endl;
        return 0; // Exit code 0 indicates success
    } else {
        std::cout << "Overall Result: SOME PERSISTENCE TESTS FAILED" << std::endl;
         std::cout << "=======================================" << std::endl;
        return 1; // Exit code non-zero indicates failure
    }
}