/**
 * @file TelemetrySpoolFormat.cpp
 * @brief Binary serialization and deserialization for the telemetry spool format.
 */

#include "Utils/TelemetrySpoolInternal.h"

#include "Utils/Telemetry.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace Spark::TelemetryDetail
{
    namespace
    {
        constexpr std::array<uint8_t, 8> kMagic{'S', 'P', 'A', 'R', 'K', 'T', 'L', 'M'};
        constexpr uint32_t kFormatVersion = 1;
        constexpr size_t kMinimumEventBytes = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3;
        constexpr uint32_t kMaxPropertiesPerEvent = 256;
        constexpr uint32_t kMaxStringBytes = 1024 * 1024;

        bool CheckedAdd(size_t& total, size_t amount)
        {
            if (amount > (std::numeric_limits<size_t>::max)() - total)
                return false;
            total += amount;
            return true;
        }

        void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
        }

        void AppendU64(std::vector<uint8_t>& bytes, uint64_t value)
        {
            for (unsigned shift = 0; shift < 64; shift += 8)
                bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
        }

        void AppendString(std::vector<uint8_t>& bytes, std::string_view value)
        {
            AppendU32(bytes, static_cast<uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        }

        class BinaryReader final
        {
          public:
            explicit BinaryReader(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}

            bool ReadBytes(void* output, size_t count)
            {
                if (count > m_bytes.size() - m_offset)
                    return false;
                if (count > 0)
                    std::memcpy(output, m_bytes.data() + m_offset, count);
                m_offset += count;
                return true;
            }

            bool ReadU32(uint32_t& value)
            {
                std::array<uint8_t, 4> encoded{};
                if (!ReadBytes(encoded.data(), encoded.size()))
                    return false;
                value = 0;
                for (unsigned index = 0; index < encoded.size(); ++index)
                    value |= static_cast<uint32_t>(encoded[index]) << (index * 8);
                return true;
            }

            bool ReadU64(uint64_t& value)
            {
                std::array<uint8_t, 8> encoded{};
                if (!ReadBytes(encoded.data(), encoded.size()))
                    return false;
                value = 0;
                for (unsigned index = 0; index < encoded.size(); ++index)
                    value |= static_cast<uint64_t>(encoded[index]) << (index * 8);
                return true;
            }

            bool ReadString(std::string& value)
            {
                uint32_t length = 0;
                if (!ReadU32(length) || length > kMaxStringBytes || length > m_bytes.size() - m_offset)
                    return false;
                value.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), length);
                m_offset += length;
                return true;
            }

            [[nodiscard]] bool AtEnd() const { return m_offset == m_bytes.size(); }

          private:
            const std::vector<uint8_t>& m_bytes;
            size_t m_offset = 0;
        };
    } // namespace

    bool TryResize(std::vector<uint8_t>& bytes, size_t size)
    {
        try
        {
            bytes.resize(size);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool EventSerializedSize(const TelemetryEvent& event, size_t& size)
    {
        if (event.sequence == 0 || event.sequence == (std::numeric_limits<uint64_t>::max)() ||
            event.name.size() > kMaxStringBytes || event.sessionId.size() > kMaxStringBytes ||
            event.properties.size() > kMaxPropertiesPerEvent)
        {
            return false;
        }

        size = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3;
        if (!CheckedAdd(size, event.name.size()) || !CheckedAdd(size, event.sessionId.size()))
            return false;

        for (const auto& [key, value] : event.properties)
        {
            if (key.size() > kMaxStringBytes || value.size() > kMaxStringBytes ||
                !CheckedAdd(size, sizeof(uint32_t) * 2) || !CheckedAdd(size, key.size()) ||
                !CheckedAdd(size, value.size()))
            {
                return false;
            }
        }
        return true;
    }

    bool Serialize(const std::vector<TelemetryEvent>& events, uint64_t maximumBytes, std::vector<uint8_t>& bytes)
    {
        if (events.empty() || events.size() > (std::numeric_limits<uint32_t>::max)())
            return false;

        size_t expectedSize = kHeaderBytes;
        uint64_t previousSequence = 0;
        for (const auto& event : events)
        {
            size_t eventSize = 0;
            if (!EventSerializedSize(event, eventSize) || event.sequence <= previousSequence ||
                !CheckedAdd(expectedSize, eventSize))
            {
                return false;
            }
            previousSequence = event.sequence;
        }
        if (expectedSize > maximumBytes)
            return false;

        bytes.clear();
        bytes.reserve(expectedSize);
        bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
        AppendU32(bytes, kFormatVersion);
        AppendU32(bytes, static_cast<uint32_t>(events.size()));

        for (const auto& event : events)
        {
            AppendU64(bytes, event.sequence);
            AppendU64(bytes, event.timestamp);
            AppendString(bytes, event.name);
            AppendString(bytes, event.sessionId);

            std::vector<std::pair<std::string_view, std::string_view>> properties;
            properties.reserve(event.properties.size());
            for (const auto& [key, value] : event.properties)
                properties.emplace_back(key, value);
            std::sort(properties.begin(), properties.end());

            AppendU32(bytes, static_cast<uint32_t>(properties.size()));
            for (const auto& [key, value] : properties)
            {
                AppendString(bytes, key);
                AppendString(bytes, value);
            }
        }
        return bytes.size() == expectedSize;
    }

    bool Parse(const std::vector<uint8_t>& bytes, uint32_t maximumEvents, std::vector<TelemetryEvent>& events)
    {
        BinaryReader reader(bytes);
        std::array<uint8_t, kMagic.size()> magic{};
        uint32_t version = 0;
        uint32_t eventCount = 0;
        if (!reader.ReadBytes(magic.data(), magic.size()) || magic != kMagic || !reader.ReadU32(version) ||
            version != kFormatVersion || !reader.ReadU32(eventCount) || eventCount == 0 || eventCount > maximumEvents ||
            eventCount > kAbsoluteMaxEvents || bytes.size() < kHeaderBytes ||
            eventCount > (bytes.size() - kHeaderBytes) / kMinimumEventBytes)
        {
            return false;
        }

        std::vector<TelemetryEvent> parsed;
        try
        {
            parsed.reserve(eventCount);
        }
        catch (...)
        {
            return false;
        }
        uint64_t previousSequence = 0;
        for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex)
        {
            TelemetryEvent event;
            uint32_t propertyCount = 0;
            if (!reader.ReadU64(event.sequence) || event.sequence == 0 ||
                event.sequence == (std::numeric_limits<uint64_t>::max)() || event.sequence <= previousSequence ||
                !reader.ReadU64(event.timestamp) || !reader.ReadString(event.name) ||
                !reader.ReadString(event.sessionId) || !reader.ReadU32(propertyCount) ||
                propertyCount > kMaxPropertiesPerEvent)
            {
                return false;
            }

            for (uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex)
            {
                std::string key;
                std::string value;
                if (!reader.ReadString(key) || !reader.ReadString(value) ||
                    !event.properties.emplace(std::move(key), std::move(value)).second)
                {
                    return false;
                }
            }

            previousSequence = event.sequence;
            parsed.push_back(std::move(event));
        }

        if (!reader.AtEnd())
            return false;
        events = std::move(parsed);
        return true;
    }

} // namespace Spark::TelemetryDetail
