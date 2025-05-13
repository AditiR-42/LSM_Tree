// bloom_filter.cpp
#include "bloom_filter.hh"
#include <iostream> 

// Constructor: Calculates optimal m and k
BloomFilter::BloomFilter(size_t estimated_elements, double false_positive_rate) {
    if (estimated_elements == 0 || false_positive_rate <= 0.0 || false_positive_rate >= 1.0) {
        m_ = 0;
        k_ = 0;
        return;
    }

    // Calculate optimal m (size of bit array in bits)
    // m = -(n * log(p)) / (log(2)^2)
    double n = static_cast<double>(estimated_elements);
    double p = false_positive_rate;
    m_ = static_cast<size_t>(std::ceil(-(n * std::log(p)) / (std::log(2.0) * std::log(2.0))));

    // Calculate optimal k (number of hash functions)
    // k = (m/n) * log(2)
    // Ensure k is at least 1
    k_ = static_cast<size_t>(std::ceil((static_cast<double>(m_) / n) * std::log(2.0)));
    if (k_ == 0) k_ = 1; 

    // Initialize bit array (vector of bytes)
    // Need ceil(m / 8) bytes
    size_t num_bytes = (m_ + 7) / 8;
    bit_array_.assign(num_bytes, 0); 
}

// Default constructor (creates an empty filter)
BloomFilter::BloomFilter() : m_(0), k_(0) {
}

void BloomFilter::add(int key) {
    if (is_empty()) {
        // Can't add to an uninitialized filter
        // std::cerr << "Warning: Attempted to add key " << key << " to an empty Bloom filter." << std::endl;
        return;
    }

    for (size_t i = 0; i < k_; ++i) {
        size_t index = get_hash_index(key, i);

        // Ensure index is within bounds (should be guaranteed by modulo m_)
        if (index >= m_) {
             std::cerr << "Error: Bloom filter hash index out of bounds! Index: " << index << ", m: " << m_ << std::endl;
             continue;
        }

        // Set the bit at 'index'
        size_t byte_index = index / 8;
        size_t bit_index_in_byte = index % 8;

        // Ensure byte_index is within bounds (should be guaranteed by index < m_)
        if (byte_index >= bit_array_.size()) {
             std::cerr << "Error: Bloom filter byte index out of bounds! Byte Index: " << byte_index << ", bit_array_ size: " << bit_array_.size() << std::endl;
             continue; 
        }

        bit_array_[byte_index] |= (1 << bit_index_in_byte);
    }
}

bool BloomFilter::contains(int key) const {
     if (is_empty()) {
        return false;
    }

    for (size_t i = 0; i < k_; ++i) {
        size_t index = get_hash_index(key, i);

         // Ensure index is within bounds (should be guaranteed by modulo m_)
        if (index >= m_) {
             std::cerr << "Error: Bloom filter hash index out of bounds during contains! Index: " << index << ", m: " << m_ << std::endl;
             return false; // Cannot verify existence
        }

        // Check if the bit at 'index' is set
        size_t byte_index = index / 8;
        size_t bit_index_in_byte = index % 8;

         // Ensure byte_index is within bounds (should be guaranteed by index < m_)
        if (byte_index >= bit_array_.size()) {
             std::cerr << "Error: Bloom filter byte index out of bounds during contains! Byte Index: " << byte_index << ", bit_array_ size: " << bit_array_.size() << std::endl;
             return false; // Cannot verify existence
        }


        if (!((bit_array_[byte_index] >> bit_index_in_byte) & 1)) {
            // If even one bit is not set, the key is definitely not in the set
            return false;
        }
    }

    // If all bits are set, the key might be in the set (could be a false positive)
    return true;
}