/**
 * @file UUID.h
 * @brief Lightweight unique identifier generation
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a compact 64-bit UUID suitable for entity IDs, asset handles,
 * and resource tracking. Uses a seeded mt19937_64 for generation.
 *
 * ## Usage
 * @code
 *   Spark::UUID id = Spark::UUID::Generate();
 *   std::string hex = id.ToString();     // "a3f7b2c1d4e5f6a7"
 *   Spark::UUID parsed = Spark::UUID::FromString(hex);
 *
 *   std::unordered_map<Spark::UUID, Entity> entities;
 *   entities[id] = myEntity;
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>
#include <random>
#include <functional>
#include <chrono>
#include <mutex>
#include <cstdio>

namespace Spark {

class UUID {
public:
    UUID() : m_value(0) {}
    explicit UUID(uint64_t value) : m_value(value) {}

    /// Generate a new unique identifier
    static UUID Generate() {
        static std::mutex s_mutex;
        static std::mt19937_64 s_rng(
            static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count()));

        std::lock_guard<std::mutex> lock(s_mutex);
        uint64_t val = 0;
        // Ensure non-zero
        while (val == 0) {
            val = s_rng();
        }
        return UUID(val);
    }

    /// Null/invalid UUID sentinel
    static UUID Null() { return UUID(0); }

    /// Check if this is a valid (non-null) UUID
    bool IsValid() const { return m_value != 0; }

    /// Get the raw 64-bit value
    uint64_t GetValue() const { return m_value; }

    /// Convert to hexadecimal string (16 characters)
    std::string ToString() const {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(m_value));
        return std::string(buf);
    }

    /// Parse a hex string back to UUID
    static UUID FromString(const std::string& str) {
        if (str.empty()) return Null();
        try {
            uint64_t val = std::stoull(str, nullptr, 16);
            return UUID(val);
        } catch (...) {
            return Null();
        }
    }

    // Comparison operators
    bool operator==(const UUID& other) const { return m_value == other.m_value; }
    bool operator!=(const UUID& other) const { return m_value != other.m_value; }
    bool operator<(const UUID& other) const { return m_value < other.m_value; }
    bool operator>(const UUID& other) const { return m_value > other.m_value; }
    bool operator<=(const UUID& other) const { return m_value <= other.m_value; }
    bool operator>=(const UUID& other) const { return m_value >= other.m_value; }

    explicit operator bool() const { return IsValid(); }
    explicit operator uint64_t() const { return m_value; }

private:
    uint64_t m_value;
};

} // namespace Spark

// std::hash specialization for use in unordered containers
namespace std {
template<>
struct hash<Spark::UUID> {
    size_t operator()(const Spark::UUID& uuid) const noexcept {
        return hash<uint64_t>()(uuid.GetValue());
    }
};
} // namespace std
