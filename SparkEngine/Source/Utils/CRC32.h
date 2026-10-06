/**
 * @file CRC32.h
 * @brief Incremental standard CRC-32 for accidental-corruption detection.
 *
 * This is the reflected CRC-32/ISO-HDLC variant used by zlib: polynomial
 * `0xEDB88320`, initial state `0xFFFFFFFF`, and a final bitwise complement.
 * CRC-32 is not keyed and does not provide authenticity or tamper resistance.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Spark
{

    /// Incremental standard CRC-32 accumulator.
    class CRC32 final
    {
      public:
        /// Reset to the standard initial state.
        void Reset() noexcept { m_state = 0xFFFFFFFFu; }

        /// Add an arbitrary byte range. `data` may be null only when `size` is zero.
        void Update(const void* data, size_t size) noexcept
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                m_state ^= bytes[index];
                for (int bit = 0; bit < 8; ++bit)
                    m_state = (m_state & 1u) != 0u ? (m_state >> 1u) ^ 0xEDB88320u : m_state >> 1u;
            }
        }

        /// Return the standard finalized checksum without changing the accumulator.
        [[nodiscard]] uint32_t Finalize() const noexcept { return ~m_state; }

      private:
        uint32_t m_state = 0xFFFFFFFFu;
    };

    /// Compute standard CRC-32 over a complete byte range.
    [[nodiscard]] inline uint32_t ComputeCRC32(const void* data, size_t size) noexcept
    {
        CRC32 crc;
        crc.Update(data, size);
        return crc.Finalize();
    }

} // namespace Spark
