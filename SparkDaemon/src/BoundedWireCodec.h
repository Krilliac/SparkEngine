/**
 * @file BoundedWireCodec.h
 * @brief Strict little-endian codec used by stateful daemon services.
 */

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Spark::Daemon::Wire
{
    template <typename T, bool = std::is_enum_v<T>> struct RawInteger
    {
        using Type = T;
    };

    template <typename T> struct RawInteger<T, true>
    {
        using Type = std::underlying_type_t<T>;
    };

    class Writer
    {
      public:
        template <typename T> void Write(T value)
        {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
            using Raw = typename RawInteger<T>::Type;
            using Unsigned = std::make_unsigned_t<Raw>;
            const auto bits = static_cast<Unsigned>(value);
            for (size_t i = 0; i < sizeof(Raw); ++i)
                m_bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffu));
        }

        bool WriteString(std::string_view value, size_t maximum)
        {
            if (value.size() > maximum || value.size() > std::numeric_limits<uint32_t>::max())
                return false;
            Write<uint32_t>(static_cast<uint32_t>(value.size()));
            m_bytes.insert(m_bytes.end(), value.begin(), value.end());
            return true;
        }

        bool WriteBytes(const std::vector<uint8_t>& value, size_t maximum)
        {
            if (value.size() > maximum || value.size() > std::numeric_limits<uint32_t>::max())
                return false;
            Write<uint32_t>(static_cast<uint32_t>(value.size()));
            m_bytes.insert(m_bytes.end(), value.begin(), value.end());
            return true;
        }

        [[nodiscard]] std::vector<uint8_t> Take() { return std::move(m_bytes); }

      private:
        std::vector<uint8_t> m_bytes;
    };

    class Reader
    {
      public:
        explicit Reader(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}

        template <typename T> bool Read(T& value)
        {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
            using Raw = typename RawInteger<T>::Type;
            using Unsigned = std::make_unsigned_t<Raw>;
            if (Remaining() < sizeof(Raw))
                return false;
            Unsigned bits = 0;
            for (size_t i = 0; i < sizeof(Raw); ++i)
                bits |= static_cast<Unsigned>(m_bytes[m_position++]) << (i * 8);
            value = static_cast<T>(bits);
            return true;
        }

        bool ReadString(std::string& value, size_t maximum)
        {
            uint32_t size = 0;
            if (!Read(size) || size > maximum || Remaining() < size)
                return false;
            value.assign(reinterpret_cast<const char*>(m_bytes.data() + m_position), size);
            m_position += size;
            return true;
        }

        bool ReadBytes(std::vector<uint8_t>& value, size_t maximum)
        {
            uint32_t size = 0;
            if (!Read(size) || size > maximum || Remaining() < size)
                return false;
            value.assign(m_bytes.begin() + static_cast<std::ptrdiff_t>(m_position),
                         m_bytes.begin() + static_cast<std::ptrdiff_t>(m_position + size));
            m_position += size;
            return true;
        }

        [[nodiscard]] size_t Remaining() const noexcept { return m_bytes.size() - m_position; }
        [[nodiscard]] bool Finished() const noexcept { return m_position == m_bytes.size(); }

      private:
        const std::vector<uint8_t>& m_bytes;
        size_t m_position = 0;
    };

    inline constexpr uint16_t kSchemaVersion = 1;

    inline bool ReadVersion(Reader& reader)
    {
        uint16_t version = 0;
        return reader.Read(version) && version == kSchemaVersion;
    }

    inline void WriteVersion(Writer& writer)
    {
        writer.Write<uint16_t>(kSchemaVersion);
    }
} // namespace Spark::Daemon::Wire
