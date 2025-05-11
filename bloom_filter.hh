// bloom_filter.hh
#ifndef BLOOM_FILTER_HH
#define BLOOM_FILTER_HH

#include <vector>
#include <string>
#include <cmath>   // For log, ceil
#include <functional> // For std::hash
#include <limits>  // For numeric_limits

// --- Constants for Bloom Filter ---
// Desired false positive probability (a property of the filter itself)
const double BLOOM_FILTER_FALSE_POSITIVE_RATE = 0.01; // 1%

class BloomFilter {
private:
    std::vector<unsigned char> bit_array_; // Use unsigned char for 8 bits per byte
    size_t m_; // Size of the bit array in bits
    size_t k_; // Number of hash functions

    // --- Hash Functions ---
    // A simple approach combining two base hash functions
    // h_i(x) = (h1(x) + i * h2(x)) % m
    size_t hash1(int key) const {
        return std::hash<int>{}(key);
    }

    size_t hash2(int key) const {
        // Combine std::hash with a multiplier for a second hash
        // Using a prime multiplier related to the golden ratio
        return std::hash<int>{}(key * 0x9e3779b9);
    }

    size_t get_hash_index(int key, size_t i) const {
        // Compute the i-th hash index
        size_t h1_val = hash1(key);
        size_t h2_val = hash2(key);

        // Combined hash function: (h1 + i * h2) % m
        // Use unsigned arithmetic to handle potential large values correctly with modulo.
         size_t index = (h1_val + i * h2_val) % m_; // This relies on size_t wrapping for large intermediate i*h2_val before modulo m

        // A potentially safer way to combine if h2 is very large could involve bitwise operations,
        // but the additive approach is standard and usually sufficient if m is not excessively large compared to size_t max.

        return index;
    }


public:
    // Constructor: Calculates optimal m and k based on estimated elements and false positive rate
    BloomFilter(size_t estimated_elements, double false_positive_rate);

    // Default constructor (for empty or deserialized filters)
    // Creates an empty filter with m=0, k=0
    BloomFilter();

    // Copy constructor and assignment operator
    BloomFilter(const BloomFilter& other) = default;
    BloomFilter& operator=(const BloomFilter& other) = default;
    // Move constructor and assignment operator
    BloomFilter(BloomFilter&& other) noexcept = default; // Added noexcept for better move semantics
    BloomFilter& operator=(BloomFilter&& other) noexcept = default; // Added noexcept

    // Add a key to the bloom filter
    void add(int key);

    // Check if a key might be in the set (returns true) or is definitely not (returns false)
    bool contains(int key) const;

    // Check if the filter is empty (useful after default construction or moves)
    bool is_empty() const { return m_ == 0 || bit_array_.empty(); }

    // Get parameters (for debugging/serialization if needed later)
    size_t size_in_bits() const { return m_; }
    size_t num_hash_functions() const { return k_; }
    size_t size_in_bytes() const { return bit_array_.size(); }

    // Note: No explicit destructor needed if using std::vector
};

#endif // BLOOM_FILTER_HH