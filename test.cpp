// test.cpp

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include "lsm_tree.hh"
#include <cassert>

using namespace std;

int main() {
    lsm_tree db;

    // 1. Insert and Get Test
    cout << "Starting insert and get test..." << endl;
    db.insert({10, 100});
    cout << "Inserted (10, 100)" << endl;
    int val = db.get(10);
    cout << "Got value: " << val << endl;
    assert(val == 100);
    cout << "Insert and get test passed." << endl;

    // 2. Delete and Get Test
    cout << "Starting delete and get test..." << endl;
    db.delete_key(10);
    cout << "Deleted key 10" << endl;
    val = db.get(10);
    cout << "Got value: " << val << endl;
    assert(val == -1); // Expecting not found
    cout << "Delete and get test passed." << endl;

    // 3. Basic Range Test
    cout << "Starting range test..." << endl;
    db.insert({20, 200});
    db.insert({30, 300});
    db.insert({40, 400});
    cout << "Inserted range values" << endl;
    cout << "Range test completed!" << endl;

    // 4. Print Stats Test
    cout << "Starting printStats test..." << endl;
    cout << "Calling printStats directly to console:" << endl;
    db.printStats();
    cout << "printStats test passed." << endl;
    cout << "All tests completed!" << endl;

    return 0;
}