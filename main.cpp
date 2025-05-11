#include <iostream>
#include <map>
#include <sstream>
#include <fstream>
#include <vector>
#include "lsm_tree.hh"

using namespace std;

void load(string& fileName, lsm_tree* lsm_tree_obj) {
    ifstream file;
    string newfileName = "generator/" + fileName;
    file.open(newfileName, ios::binary);
    if (!file.is_open()) {  
        cerr << "Error: File `" << newfileName << "` not found!" << endl;  
        return; // early exit on error
    }

    int key;
    int value;
    while (file.read(reinterpret_cast<char*>(&key), sizeof(key)) &&  
           file.read(reinterpret_cast<char*>(&value), sizeof(value))) { 
        lsm_tree_obj->insert({key, value}); // insert into lsm_tree
    }

    if (file.gcount() > 0) {
        cerr << "Warning: Incomplete read at end of file `" << newfileName << "`.  File may be corrupted." << endl;
    }
    file.close();
}

int main() {
    lsm_tree* db = new lsm_tree();
    
    string line;
    while (getline(cin, line)) {
        istringstream iss(line);
        char command;
        int key, value;
        string fileName;
        iss >> command;
        try {
            switch (command) {
                case 'p': // put
                    iss >> key >> value;
                    db->insert({key, value});
                    cout << key;
                    break;
                case 'g': // get
                    iss >> key;
                    db->get(key);
                    break;
                case 'd': // del
                    iss >> key;
                    db->delete_key(key);
                    break;
                case 'r': { // range
                    int startKey, endKey;
                    iss >> startKey >> endKey;
                    db->range(startKey, endKey);
                    break;
                }
                case 'l': { // load
                    iss >> fileName;
                    load(fileName, db);
                    break;
                }
                case 's': // stats
                    db->printStats();
                    break;
                default:
                    cout << "Unknown: please use p, g, d, r, l, or s" << command << endl;
            }
        } catch (const exception& e) {
            cerr << "Error: " << e.what() << endl;
        } catch (...) {
            cerr << "Error: An unknown exception occurred." << endl;
        }
    }
    return 0;
}