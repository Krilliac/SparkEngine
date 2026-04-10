/**
 * @file NetworkBuffer.cpp
 * @brief NetBuffer serialization/deserialization helpers
 *
 * Extracted from NetworkManager.cpp — implements all NetBuffer Read and Write methods.
 */

#include "NetworkManager.h"
#include "../../Utils/Assert.h"
#include "../../Utils/LogMacros.h"
#include <cstring>
#include <algorithm>

using namespace DirectX;
namespace Spark::Net
{

    // ============================================================================
    // NetBuffer — Write methods
    // ============================================================================

    void NetBuffer::WriteUint8(uint8_t val)
    {
        m_data.push_back(val);
    }

    void NetBuffer::WriteUint16(uint16_t val)
    {
        m_data.push_back(static_cast<uint8_t>(val & 0xFF));
        m_data.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    }

    void NetBuffer::WriteUint32(uint32_t val)
    {
        for (int i = 0; i < 4; ++i)
            m_data.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }

    void NetBuffer::WriteFloat(float val)
    {
        uint32_t bits;
        std::memcpy(&bits, &val, sizeof(float));
        WriteUint32(bits);
    }

    void NetBuffer::WriteString(const std::string& val)
    {
        // Clamp string length to uint16_t max to prevent truncation bugs
        uint16_t len = static_cast<uint16_t>((std::min)(val.size(), static_cast<size_t>(UINT16_MAX)));
        WriteUint16(len);
        for (uint16_t i = 0; i < len; ++i)
            m_data.push_back(static_cast<uint8_t>(val[i]));
    }

    void NetBuffer::WriteVector3(const XMFLOAT3& val)
    {
        WriteFloat(val.x);
        WriteFloat(val.y);
        WriteFloat(val.z);
    }

    void NetBuffer::WriteBytes(const void* data, size_t size)
    {
        if (!data || size == 0)
            return;
        const auto* bytes = static_cast<const uint8_t*>(data);
        m_data.insert(m_data.end(), bytes, bytes + size);
    }

    // ============================================================================
    // NetBuffer — Read methods
    // ============================================================================

    uint8_t NetBuffer::ReadUint8()
    {
        if (m_error || m_readPos >= m_data.size())
        {
            // Log only on the first overrun per buffer. Subsequent reads after
            // m_error is sticky would otherwise spam the log during fuzz / stress tests
            // that intentionally feed truncated packets.
            if (!m_error)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "NetBuffer::ReadUint8 — buffer overrun at pos %zu (size=%zu)", m_readPos, m_data.size());
                m_error = true;
            }
            return 0;
        }
        return m_data[m_readPos++];
    }

    uint16_t NetBuffer::ReadUint16()
    {
        if (m_error || m_readPos + 2 > m_data.size())
        {
            if (!m_error)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "NetBuffer::ReadUint16 — buffer overrun at pos %zu (need 2, size=%zu)", m_readPos,
                               m_data.size());
                m_error = true;
            }
            return 0;
        }
        uint16_t val = 0;
        for (int i = 0; i < 2; ++i)
            val |= static_cast<uint16_t>(m_data[m_readPos++]) << (i * 8);
        return val;
    }

    uint32_t NetBuffer::ReadUint32()
    {
        if (m_error || m_readPos + 4 > m_data.size())
        {
            if (!m_error)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "NetBuffer::ReadUint32 — buffer overrun at pos %zu (need 4, size=%zu)", m_readPos,
                               m_data.size());
                m_error = true;
            }
            return 0;
        }
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i)
            val |= static_cast<uint32_t>(m_data[m_readPos++]) << (i * 8);
        return val;
    }

    float NetBuffer::ReadFloat()
    {
        if (m_error)
            return 0.0f;
        uint32_t bits = ReadUint32();
        float val;
        std::memcpy(&val, &bits, sizeof(float));
        return val;
    }

    std::string NetBuffer::ReadString()
    {
        uint16_t len = ReadUint16();
        if (m_error)
            return {};
        if (m_readPos + len > m_data.size())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "NetBuffer::ReadString — buffer overrun: string len=%u, remaining=%zu", len,
                           m_data.size() - m_readPos);
            m_error = true;
            return {};
        }
        std::string result(len, '\0');
        for (uint16_t i = 0; i < len; ++i)
            result[i] = static_cast<char>(m_data[m_readPos++]);
        return result;
    }

    XMFLOAT3 NetBuffer::ReadVector3()
    {
        if (m_error)
            return {0.0f, 0.0f, 0.0f};
        float x = ReadFloat(), y = ReadFloat(), z = ReadFloat();
        return {x, y, z};
    }

    void NetBuffer::ReadBytes(void* data, size_t size)
    {
        if (!data || size == 0)
            return;
        if (m_error || m_readPos + size > m_data.size())
        {
            m_error = true;
            return;
        }
        auto* bytes = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
            bytes[i] = m_data[m_readPos++];
    }

} // namespace Spark::Net
