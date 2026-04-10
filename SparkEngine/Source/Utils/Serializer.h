/**
 * @file Serializer.h
 * @brief Lightweight binary serialization and deserialization utilities
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides `BinaryWriter` and `BinaryReader` for reading and writing
 * strongly-typed binary data into / from a `std::vector<uint8_t>` buffer.
 * Designed for save files, network payloads, and asset metadata — situations
 * where compact, portable binary representation is preferable to text formats.
 *
 * ## Format guarantees
 * - **Little-endian**: all integers are written in little-endian byte order so
 *   serialized data is portable across host-endian platforms.
 * - **Strings**: stored as a `uint32_t` byte-length prefix followed by UTF-8 bytes
 *   (no null terminator). Maximum string length is `UINT32_MAX` bytes.
 * - **POD types**: memcpy'd directly; the layout must match on reader and writer.
 * - No versioning or schema is embedded — callers are responsible for versioning.
 *
 * ## Thread safety
 * `BinaryWriter` and `BinaryReader` are **not thread-safe**. Each instance should
 * be used from a single thread.
 *
 * ## Usage
 * @code
 *   // Writing
 *   Spark::BinaryWriter writer;
 *   writer.Write<uint32_t>(42);
 *   writer.Write<float>(3.14f);
 *   writer.WriteString("SparkEngine");
 *   std::vector<uint8_t> bytes = writer.GetBuffer();
 *
 *   // Reading
 *   Spark::BinaryReader reader(bytes);
 *   auto version = reader.Read<uint32_t>();      // 42
 *   auto pi      = reader.Read<float>();          // 3.14f
 *   auto name    = reader.ReadString();           // "SparkEngine"
 *   bool ok      = !reader.HasError();            // true if all reads succeeded
 * @endcode
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "TypeTraits.h"

namespace Spark
{

    // =========================================================================
    // Endian helpers
    // =========================================================================

    namespace Detail
    {
        [[nodiscard]] inline constexpr bool IsLittleEndian() noexcept
        {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
            return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
            return true; // Assume little-endian (x86/x64/ARM-LE)
#endif
        }

        template <Spark::TriviallyCopyable T> [[nodiscard]] inline T ByteSwap(T value) noexcept
        {
            uint8_t bytes[sizeof(T)];
            std::memcpy(bytes, &value, sizeof(T));
            for (size_t i = 0; i < sizeof(T) / 2; ++i)
                std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
            T result;
            std::memcpy(&result, bytes, sizeof(T));
            return result;
        }

        template <typename T> [[nodiscard]] inline T ToLittleEndian(T value) noexcept
        {
            if constexpr (sizeof(T) == 1)
                return value;
            if (IsLittleEndian())
                return value;
            return ByteSwap(value);
        }

        template <typename T> [[nodiscard]] inline T FromLittleEndian(T value) noexcept
        {
            return ToLittleEndian(value); // Swap is its own inverse
        }
    } // namespace Detail

    // =========================================================================
    // BinaryWriter
    // =========================================================================

    /**
     * @class BinaryWriter
     * @brief Append-only binary serializer that writes into a growable byte buffer.
     *
     * Data is always written in little-endian byte order. The internal buffer can
     * be retrieved with `GetBuffer()` for transmission or saving.
     */
    class BinaryWriter
    {
      public:
        /**
         * @brief Construct an empty writer with optional reserved capacity.
         * @param reserveBytes  Pre-reserve this many bytes to avoid reallocation.
         */
        explicit BinaryWriter(size_t reserveBytes = 256) { m_buffer.reserve(reserveBytes); }

        /**
         * @brief Write a single value of a trivially-copyable type.
         *
         * Multi-byte integers are converted to little-endian before writing.
         *
         * @tparam T    A trivially-copyable, non-pointer type.
         * @param value  Value to serialise.
         */
        template <Spark::TriviallyCopyable T>
            requires(!std::is_pointer_v<T>)
        void Write(const T& value)
        {

            T leValue = Detail::ToLittleEndian(value);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&leValue);
            m_buffer.insert(m_buffer.end(), bytes, bytes + sizeof(T));
        }

        /**
         * @brief Write a std::string as a length-prefixed UTF-8 blob.
         *
         * Format: `uint32_t` byte-count (LE) followed by raw bytes (no null terminator).
         *
         * @param str  String to serialise.
         * @throws std::length_error if `str.size()` exceeds `UINT32_MAX`.
         */
        void WriteString(const std::string& str)
        {
            if (str.size() > UINT32_MAX)
                throw std::length_error("BinaryWriter::WriteString: string too long");

            Write<uint32_t>(static_cast<uint32_t>(str.size()));
            const auto* bytes = reinterpret_cast<const uint8_t*>(str.data());
            m_buffer.insert(m_buffer.end(), bytes, bytes + str.size());
        }

        /**
         * @brief Write raw bytes directly into the buffer (no endian conversion).
         *
         * @param data  Pointer to the bytes to write. Must not be null if `size > 0`.
         * @param size  Number of bytes to copy.
         */
        void WriteBytes(const void* data, size_t size)
        {
            if (size == 0)
                return;
            const auto* bytes = static_cast<const uint8_t*>(data);
            m_buffer.insert(m_buffer.end(), bytes, bytes + size);
        }

        /**
         * @brief Get a const reference to the internal byte buffer.
         * @return  Reference to the written bytes (valid until the next write).
         */
        [[nodiscard]] const std::vector<uint8_t>& GetBuffer() const noexcept { return m_buffer; }

        /**
         * @brief Move the internal buffer out of the writer.
         *
         * The writer is left in an empty-but-valid state after this call.
         *
         * @return  Owned byte vector.
         */
        [[nodiscard]] std::vector<uint8_t> TakeBuffer() noexcept { return std::move(m_buffer); }

        /**
         * @brief Number of bytes written so far.
         * @return  Total byte count.
         */
        [[nodiscard]] size_t Size() const noexcept { return m_buffer.size(); }

        /**
         * @brief Reset the writer to its initial empty state.
         */
        void Clear() noexcept { m_buffer.clear(); }

      private:
        std::vector<uint8_t> m_buffer;
    };

    // =========================================================================
    // BinaryReader
    // =========================================================================

    /**
     * @class BinaryReader
     * @brief Sequential binary deserializer that reads from a byte buffer.
     *
     * Maintains a cursor that advances with each read. All reads are bounds-checked;
     * on overflow the reader enters an error state and subsequent reads return
     * zero-initialised values. Check `HasError()` after reading to detect truncation.
     */
    class BinaryReader
    {
      public:
        /**
         * @brief Construct a reader over an externally-owned byte buffer.
         *
         * The buffer must outlive this reader.
         *
         * @param data  Pointer to the first byte.
         * @param size  Total number of bytes available.
         */
        BinaryReader(const uint8_t* data, size_t size) noexcept : m_data(data), m_size(size), m_cursor(0) {}

        /**
         * @brief Construct a reader from a `std::vector<uint8_t>`.
         *
         * The vector must outlive this reader.
         *
         * @param buffer  Reference to the byte vector.
         */
        explicit BinaryReader(const std::vector<uint8_t>& buffer) noexcept
            : m_data(buffer.data()), m_size(buffer.size()), m_cursor(0)
        {
        }

        /**
         * @brief Read a single value of a trivially-copyable type.
         *
         * Values are read from the buffer in little-endian byte order and
         * converted to the host byte order before returning.
         *
         * @tparam T  A trivially-copyable, non-pointer type.
         * @return    The deserialised value, or a zero-initialised T on error.
         */
        template <Spark::TriviallyCopyable T>
            requires(!std::is_pointer_v<T>)
        T Read()
        {

            T value{};
            if (!Consume(sizeof(T)))
                return value;

            std::memcpy(&value, m_data + m_cursor - sizeof(T), sizeof(T));
            return Detail::FromLittleEndian(value);
        }

        /**
         * @brief Read a length-prefixed UTF-8 string.
         *
         * Reads a `uint32_t` byte count then that many raw bytes.
         * Returns an empty string on error.
         *
         * @return  The deserialised string, or an empty string on error.
         */
        [[nodiscard]] std::string ReadString()
        {
            auto length = Read<uint32_t>();
            if (m_error || !Consume(length))
                return {};

            return std::string(reinterpret_cast<const char*>(m_data + m_cursor - length), length);
        }

        /**
         * @brief Read raw bytes into a caller-provided buffer.
         *
         * @param dest  Destination buffer. Must not be null if `size > 0`.
         * @param size  Number of bytes to copy.
         * @return      true if all bytes were successfully read.
         */
        bool ReadBytes(void* dest, size_t size)
        {
            if (size == 0)
                return true;
            if (dest == nullptr)
            {
                m_error = true;
                return false;
            }
            if (!Consume(size))
                return false;
            std::memcpy(dest, m_data + m_cursor - size, size);
            return true;
        }

        /**
         * @brief Skip `count` bytes without reading them.
         * @param count  Number of bytes to skip.
         * @return       true if the skip succeeded, false on overflow.
         */
        bool Skip(size_t count) { return Consume(count); }

        /**
         * @brief Current read position within the buffer.
         * @return  Byte offset from the start of the buffer.
         */
        [[nodiscard]] size_t Tell() const noexcept { return m_cursor; }

        /**
         * @brief Total number of bytes in the buffer.
         * @return  Buffer size in bytes.
         */
        [[nodiscard]] size_t Size() const noexcept { return m_size; }

        /**
         * @brief Number of bytes remaining to be read.
         * @return  Bytes between the current cursor and the end of the buffer.
         */
        [[nodiscard]] size_t Remaining() const noexcept { return m_error ? 0 : m_size - m_cursor; }

        /**
         * @brief Return whether the reader has encountered an error (e.g. overflow).
         * @return  true if any read or skip operation failed.
         */
        [[nodiscard]] bool HasError() const noexcept { return m_error; }

        /**
         * @brief Return true if there are no more bytes to read (and no error).
         * @return  true at end-of-buffer without error.
         */
        [[nodiscard]] bool IsEOF() const noexcept { return !m_error && m_cursor >= m_size; }

      private:
        bool Consume(size_t count) noexcept
        {
            if (m_error || m_cursor > m_size || count > (m_size - m_cursor))
            {
                m_error = true;
                return false;
            }
            m_cursor += count;
            return true;
        }

        const uint8_t* m_data;
        size_t m_size;
        size_t m_cursor;
        bool m_error = false;
    };

} // namespace Spark
