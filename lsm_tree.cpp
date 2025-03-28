#include "lsm_tree.hh" 
#include <iostream>   
#include <algorithm>  
#include <vector>

using namespace std;

// Level class implementation
level::level(int capacity, int curr_level) : capacity_(capacity), curr_level_(curr_level) {
    sstable_.reserve(capacity_);
}

// Memtable class implementation
memtable::memtable() {
    memtable_.reserve(MEMTABLE_CAPACITY);
}

// LSM_Tree class implementation
lsm_tree::lsm_tree() {
    memtable_ptr_ = new memtable();

    memtable_ptr_->level1ptr_ = new level(INITIAL_LEVEL_CAPACITY, 1);
    levels_.resize(MAX_LEVELS + 1); //Initialize the Vector!!!!
    levels_[1] = memtable_ptr_->level1ptr_; // ptr to level 1 is stored in 1 index

    auto curr_level_ptr = levels_[1];
    for (int i = 2; i <= MAX_LEVELS; ++i) {
        levels_[i] = new level(curr_level_ptr->capacity_ * SIZE_RATIO, i);
        curr_level_ptr->next_ = levels_[i];
        curr_level_ptr = levels_[i];
    }
}

bool level::merge(vector<key_value>& child_data_ptr, int num_elements_to_merge) {
    // TODO: REPLACE WITH ACTUAL MERGE POLICY
    std::cout << "Placeholder merge function called. Will implement merge policy later." << std::endl;
    // For now, just clear the current level's SSTable and copy the child data.
    sstable_.clear(); // Clear the vector

    // Copy the content from child_data to sstable_
    for (int i = 0; i < num_elements_to_merge; ++i) {
          sstable_.push_back(child_data_ptr[i]);
    }
    // Update the current level size;
    curr_size_ = num_elements_to_merge;

    return true;
}

bool memtable::insert(key_value kv_pair) {
    // Search through memtable --> if it exists, update the key value struct directly to be the new value
    for (int i = 0; i < curr_size_; ++i) {
        if (memtable_[i].key == kv_pair.key) {
            memtable_[i].value = kv_pair.value;
            return true;
        }
    }

    if (curr_size_ == capacity_) {
        // Sort memtable before merging
        std::sort(memtable_.begin(), memtable_.end(), [](const key_value& a, const key_value& b) {
            return a.key < b.key;
        });

        // Merge the sorted buffer into the LSM tree
        level1ptr_->merge(memtable_, curr_size_);

        curr_size_ = 0;
        memtable_.clear(); // Clear the vector
        memtable_.reserve(MEMTABLE_CAPACITY);
    }

    memtable_.push_back(kv_pair);
    ++curr_size_;
    return true;
}

bool lsm_tree::insert(key_value kv_pair) {
    memtable_ptr_->insert(kv_pair);
    return true;
}

int lsm_tree::get(int key, bool called_from_range) {
    // Perform linear search on the memtable
    for (int i = 0; i < memtable_ptr_->curr_size_; ++i) {
        if (memtable_ptr_->memtable_[i].key == key) {
            if (memtable_ptr_->memtable_[i].tombstone) {
                if (!called_from_range) {
                    cout << endl;
                }
                return -1;
            }

            if (!called_from_range) {
                cout << memtable_ptr_->memtable_[i].value << endl;
            } else {
                cout << key << ":" << memtable_ptr_->memtable_[i].value << " ";
            }
            return memtable_ptr_->memtable_[i].value;
        }
    }

    // If key not found, perform binary search across each sorted string table level
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        auto curr_level_ptr = levels_[i];

        int l = 0;
        int r = curr_level_ptr->curr_size_ - 1;

        while (l <= r) {
            int midpoint = (l + r) / 2;
            if (curr_level_ptr->sstable_[midpoint].key == key) {

                // If not deleted, return key's value
                if (curr_level_ptr->sstable_[midpoint].tombstone) {
                    if (!called_from_range) {
                        cout << endl;
                    }
                    return -1;
                }

                if (!called_from_range) {
                    cout << curr_level_ptr->sstable_[midpoint].value << endl;
                } else {
                    cout << key << ":" << curr_level_ptr->sstable_[midpoint].value << " ";
                }
                return curr_level_ptr->sstable_[midpoint].value;

            } else if (curr_level_ptr->sstable_[midpoint].key < key) {
                l = midpoint + 1;
            } else {
                r = midpoint - 1;
            }
        }
    }

    if (!called_from_range) {
        cout << endl;
    }
    return -1;
}

void lsm_tree::range(int start, int end) {
    for (int i = start; i < end; ++i) {
        get(i, true);
    }
    cout << endl; 
}

void lsm_tree::delete_key(int key) {
    // Search through memtable --> if it exists, update the key value struct to be marked as deleted (tombstone = true)
    for (int i = 0; i < memtable_ptr_->curr_size_; ++i) {
        if (memtable_ptr_->memtable_[i].key == key) {
            memtable_ptr_->memtable_[i].tombstone = true;
            return;
        }
    }

    // If not found, insert a new key value struct with tombstoned marked as true
    insert({key, 0, true});
    return;
}

void lsm_tree::printStats() {
    // (1) Number of logical key value pairs (excluding deleted/stale entries)
    int logicalPairCount = 0;

    // First, count the logical pairs in the MemTable
    for (int i = 0; i < memtable_ptr_->curr_size_; ++i) {
        if (!memtable_ptr_->memtable_[i].tombstone) {
            ++logicalPairCount;
        }
    }

    // Count logical pairs in SSTables at each level
    vector<int> levelCounts(MAX_LEVELS + 1, 0);  // Initialize counts for each level
    for (int i = 1; i <= MAX_LEVELS; ++i) {
        auto curr_level_ptr = levels_[i];
        for (int j = 0; j < curr_level_ptr->curr_size_; ++j) {
            if (!curr_level_ptr->sstable_[j].tombstone) {
                ++logicalPairCount;
                ++levelCounts[i];
            }
        }
    }

    cout << "Logical Pairs: " << logicalPairCount << endl;

    // (2) Number of keys in each level
    cout << "LVL1: " << levelCounts[1];
    for (int i = 2; i <= MAX_LEVELS; ++i) {
        cout << ", LVL" << i << ": " << levelCounts[i];
    }
    cout << endl;

    // (3) Dump the tree (key, value, level)
    cout << "MemTable:" << endl;
    for (int i = 0; i < memtable_ptr_->curr_size_; ++i) {
         if (!memtable_ptr_->memtable_[i].tombstone)
            cout << memtable_ptr_->memtable_[i].key << ":" << memtable_ptr_->memtable_[i].value << ":M ";
    }

    cout << endl;

    for (int i = 1; i <= MAX_LEVELS; ++i) {
        cout << "Level " << i << ":" << endl;
        auto curr_level_ptr = levels_[i];
        for (int j = 0; j < curr_level_ptr->curr_size_; ++j) {
           if (!curr_level_ptr->sstable_[j].tombstone)
              cout << curr_level_ptr->sstable_[j].key << ":" << curr_level_ptr->sstable_[j].value << ":L" << i << " ";
        }
        cout << endl;
    }
}