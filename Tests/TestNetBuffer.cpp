// TestNetBuffer.cpp - Round-trip tests for NetBuffer serialization
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// Standalone reimplementation of NetBuffer
// (mirrors Spark::Net::NetBuffer from NetworkManager.h/cpp)
// ============================================================================

namespace
{

    struct Float3
    {
        float x, y, z;
    };

    class NetBuffer
    {
      public:
        void WriteUint8(uint8_t val) { m_data.push_back(val); }

        void WriteUint16(uint16_t val)
        {
            m_data.push_back(static_cast<uint8_t>(val & 0xFF));
            m_data.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        }

        void WriteUint32(uint32_t val)
        {
            for (int i = 0; i < 4; ++i)
                m_data.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }

        void WriteFloat(float val)
        {
            uint32_t bits;
            std::memcpy(&bits, &val, sizeof(float));
            WriteUint32(bits);
        }

        void WriteString(const std::string& val)
        {
            WriteUint16(static_cast<uint16_t>(val.size()));
            for (char c : val)
                m_data.push_back(static_cast<uint8_t>(c));
        }

        void WriteVector3(const Float3& val)
        {
            WriteFloat(val.x);
            WriteFloat(val.y);
            WriteFloat(val.z);
        }

        void WriteBytes(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            m_data.insert(m_data.end(), bytes, bytes + size);
        }

        uint8_t ReadUint8()
        {
            if (m_error || m_readPos >= m_data.size())
            {
                m_error = true;
                return 0;
            }
            return m_data[m_readPos++];
        }

        uint16_t ReadUint16()
        {
            if (m_error || m_readPos + 2 > m_data.size())
            {
                m_error = true;
                return 0;
            }
            uint16_t val = 0;
            for (int i = 0; i < 2; ++i)
                val |= static_cast<uint16_t>(m_data[m_readPos++]) << (i * 8);
            return val;
        }

        uint32_t ReadUint32()
        {
            if (m_error || m_readPos + 4 > m_data.size())
            {
                m_error = true;
                return 0;
            }
            uint32_t val = 0;
            for (int i = 0; i < 4; ++i)
                val |= static_cast<uint32_t>(m_data[m_readPos++]) << (i * 8);
            return val;
        }

        float ReadFloat()
        {
            if (m_error)
                return 0.0f;
            uint32_t bits = ReadUint32();
            float val;
            std::memcpy(&val, &bits, sizeof(float));
            return val;
        }

        std::string ReadString()
        {
            uint16_t len = ReadUint16();
            if (m_error)
                return {};
            if (m_readPos + len > m_data.size())
            {
                m_error = true;
                return {};
            }
            std::string result(len, '\0');
            for (uint16_t i = 0; i < len; ++i)
                result[i] = static_cast<char>(m_data[m_readPos++]);
            return result;
        }

        Float3 ReadVector3()
        {
            if (m_error)
                return {0.0f, 0.0f, 0.0f};
            float x = ReadFloat(), y = ReadFloat(), z = ReadFloat();
            return {x, y, z};
        }

        void ReadBytes(void* data, size_t size)
        {
            if (m_error || m_readPos + size > m_data.size())
            {
                m_error = true;
                return;
            }
            auto* bytes = static_cast<uint8_t*>(data);
            for (size_t i = 0; i < size; ++i)
                bytes[i] = m_data[m_readPos++];
        }

        const std::vector<uint8_t>& GetData() const { return m_data; }
        size_t GetSize() const { return m_data.size(); }
        size_t GetReadPosition() const { return m_readPos; }
        size_t RemainingBytes() const { return m_readPos <= m_data.size() ? m_data.size() - m_readPos : 0; }
        bool HasError() const { return m_error; }
        bool IsValid() const { return !m_error; }
        bool CanRead(size_t bytes) const { return !m_error && (m_readPos + bytes <= m_data.size()); }

        void Reset()
        {
            m_data.clear();
            m_readPos = 0;
            m_error = false;
        }

      private:
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
        bool m_error = false;
    };

} // namespace

// ============================================================================
// Basic Round-Trip Tests
// ============================================================================

TEST(NetBuffer_Uint8RoundTrip)
{
    NetBuffer buf;
    buf.WriteUint8(0);
    buf.WriteUint8(127);
    buf.WriteUint8(255);

    EXPECT_EQ(buf.ReadUint8(), (uint8_t)0);
    EXPECT_EQ(buf.ReadUint8(), (uint8_t)127);
    EXPECT_EQ(buf.ReadUint8(), (uint8_t)255);
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_Uint16RoundTrip)
{
    NetBuffer buf;
    buf.WriteUint16(0);
    buf.WriteUint16(1000);
    buf.WriteUint16(65535);

    EXPECT_EQ(buf.ReadUint16(), (uint16_t)0);
    EXPECT_EQ(buf.ReadUint16(), (uint16_t)1000);
    EXPECT_EQ(buf.ReadUint16(), (uint16_t)65535);
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_Uint32RoundTrip)
{
    NetBuffer buf;
    buf.WriteUint32(0);
    buf.WriteUint32(123456789);
    buf.WriteUint32(0xFFFFFFFF);

    EXPECT_EQ(buf.ReadUint32(), (uint32_t)0);
    EXPECT_EQ(buf.ReadUint32(), (uint32_t)123456789);
    EXPECT_EQ(buf.ReadUint32(), (uint32_t)0xFFFFFFFF);
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_FloatRoundTrip)
{
    NetBuffer buf;
    buf.WriteFloat(0.0f);
    buf.WriteFloat(3.14159f);
    buf.WriteFloat(-1.0f);

    EXPECT_NEAR(buf.ReadFloat(), 0.0f, 0.0001f);
    EXPECT_NEAR(buf.ReadFloat(), 3.14159f, 0.0001f);
    EXPECT_NEAR(buf.ReadFloat(), -1.0f, 0.0001f);
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_StringRoundTrip)
{
    NetBuffer buf;
    buf.WriteString("hello");
    buf.WriteString("");
    buf.WriteString("world!");

    EXPECT_EQ(buf.ReadString(), std::string("hello"));
    EXPECT_EQ(buf.ReadString(), std::string(""));
    EXPECT_EQ(buf.ReadString(), std::string("world!"));
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_Vector3RoundTrip)
{
    NetBuffer buf;
    Float3 v{1.0f, 2.0f, 3.0f};
    buf.WriteVector3(v);

    auto result = buf.ReadVector3();
    EXPECT_NEAR(result.x, 1.0f, 0.0001f);
    EXPECT_NEAR(result.y, 2.0f, 0.0001f);
    EXPECT_NEAR(result.z, 3.0f, 0.0001f);
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_BytesRoundTrip)
{
    NetBuffer buf;
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    buf.WriteBytes(data, 4);

    uint8_t result[4] = {};
    buf.ReadBytes(result, 4);
    EXPECT_EQ(result[0], (uint8_t)0xDE);
    EXPECT_EQ(result[1], (uint8_t)0xAD);
    EXPECT_EQ(result[2], (uint8_t)0xBE);
    EXPECT_EQ(result[3], (uint8_t)0xEF);
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// Mixed Types
// ============================================================================

TEST(NetBuffer_MixedTypes)
{
    NetBuffer buf;
    buf.WriteUint8(42);
    buf.WriteFloat(2.718f);
    buf.WriteString("test");
    buf.WriteUint32(999);

    EXPECT_EQ(buf.ReadUint8(), (uint8_t)42);
    EXPECT_NEAR(buf.ReadFloat(), 2.718f, 0.001f);
    EXPECT_EQ(buf.ReadString(), std::string("test"));
    EXPECT_EQ(buf.ReadUint32(), (uint32_t)999);
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// Boundary Conditions
// ============================================================================

TEST(NetBuffer_EmptyBuffer)
{
    NetBuffer buf;
    EXPECT_EQ(buf.GetSize(), (size_t)0);
    EXPECT_EQ(buf.GetReadPosition(), (size_t)0);
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBuffer_ExactCapacity)
{
    NetBuffer buf;
    buf.WriteUint8(1);
    EXPECT_EQ(buf.GetSize(), (size_t)1);

    auto val = buf.ReadUint8();
    EXPECT_EQ(val, (uint8_t)1);
    EXPECT_FALSE(buf.HasError());
    EXPECT_EQ(buf.GetReadPosition(), (size_t)1);
}

TEST(NetBuffer_Reset)
{
    NetBuffer buf;
    buf.WriteUint32(12345);
    buf.ReadUint32();

    buf.Reset();
    EXPECT_EQ(buf.GetSize(), (size_t)0);
    EXPECT_EQ(buf.GetReadPosition(), (size_t)0);
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// Error Handling (Overrun Detection)
// ============================================================================

TEST(NetBuffer_ReadUint8Overrun)
{
    NetBuffer buf;
    EXPECT_FALSE(buf.HasError());

    auto val = buf.ReadUint8();
    EXPECT_EQ(val, (uint8_t)0);
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ReadUint16Overrun)
{
    NetBuffer buf;
    buf.WriteUint8(0xFF); // Only 1 byte, need 2

    auto val = buf.ReadUint16();
    EXPECT_EQ(val, (uint16_t)0);
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ReadUint32Overrun)
{
    NetBuffer buf;
    buf.WriteUint16(0xFFFF); // Only 2 bytes, need 4

    auto val = buf.ReadUint32();
    EXPECT_EQ(val, (uint32_t)0);
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ReadFloatOverrun)
{
    NetBuffer buf;
    // Empty buffer — reading float should fail
    auto val = buf.ReadFloat();
    EXPECT_NEAR(val, 0.0f, 0.0001f);
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ReadStringOverrun)
{
    NetBuffer buf;
    // Write a length prefix claiming 100 chars but don't write the chars
    buf.WriteUint16(100);

    auto val = buf.ReadString();
    EXPECT_TRUE(buf.HasError());
    EXPECT_TRUE(val.empty());
}

TEST(NetBuffer_ReadBytesOverrun)
{
    NetBuffer buf;
    buf.WriteUint8(0xAA);

    uint8_t result[4] = {};
    buf.ReadBytes(result, 4); // Request 4 bytes, only 1 available
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ErrorPersistsAfterOverrun)
{
    NetBuffer buf;
    buf.WriteUint8(42);

    // Read successfully
    auto val = buf.ReadUint8();
    EXPECT_EQ(val, (uint8_t)42);
    EXPECT_FALSE(buf.HasError());

    // Overrun
    buf.ReadUint8();
    EXPECT_TRUE(buf.HasError());

    // Error persists
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ResetClearsError)
{
    NetBuffer buf;
    buf.ReadUint8(); // Overrun on empty buffer
    EXPECT_TRUE(buf.HasError());

    buf.Reset();
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// Error Propagation (once error is set, all subsequent reads fail immediately)
// ============================================================================

TEST(NetBuffer_ErrorPropagationUint16)
{
    NetBuffer buf;
    buf.WriteUint8(42);
    buf.WriteUint16(1000);

    // Cause an overrun on a uint32 read (only 3 bytes available)
    buf.ReadUint32();
    EXPECT_TRUE(buf.HasError());

    // Subsequent reads should fail immediately without advancing position
    size_t posBeforeRead = buf.GetReadPosition();
    auto val = buf.ReadUint16();
    EXPECT_EQ(val, (uint16_t)0);
    EXPECT_TRUE(buf.HasError());
    EXPECT_EQ(buf.GetReadPosition(), posBeforeRead);
}

TEST(NetBuffer_ErrorPropagationFloat)
{
    NetBuffer buf;
    // Cause error on empty buffer
    buf.ReadUint8();
    EXPECT_TRUE(buf.HasError());

    // Float read should immediately return 0 without trying to read
    auto val = buf.ReadFloat();
    EXPECT_NEAR(val, 0.0f, 0.0001f);
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ErrorPropagationString)
{
    NetBuffer buf;
    buf.ReadUint8(); // Error on empty buffer
    EXPECT_TRUE(buf.HasError());

    // String read should immediately return empty
    auto val = buf.ReadString();
    EXPECT_TRUE(val.empty());
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ErrorPropagationVector3)
{
    NetBuffer buf;
    buf.ReadUint8(); // Error on empty buffer
    EXPECT_TRUE(buf.HasError());

    auto val = buf.ReadVector3();
    EXPECT_NEAR(val.x, 0.0f, 0.0001f);
    EXPECT_NEAR(val.y, 0.0f, 0.0001f);
    EXPECT_NEAR(val.z, 0.0f, 0.0001f);
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBuffer_ErrorPropagationBytes)
{
    NetBuffer buf;
    buf.ReadUint8(); // Error on empty buffer
    EXPECT_TRUE(buf.HasError());

    uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    buf.ReadBytes(data, 4);
    EXPECT_TRUE(buf.HasError());
    // Data should be unchanged since read was skipped
    EXPECT_EQ(data[0], (uint8_t)0xAA);
}

// ============================================================================
// New API: RemainingBytes, CanRead, IsValid
// ============================================================================

TEST(NetBuffer_RemainingBytes)
{
    NetBuffer buf;
    buf.WriteUint32(42);
    buf.WriteUint8(7);

    EXPECT_EQ(buf.RemainingBytes(), (size_t)5);
    buf.ReadUint32();
    EXPECT_EQ(buf.RemainingBytes(), (size_t)1);
    buf.ReadUint8();
    EXPECT_EQ(buf.RemainingBytes(), (size_t)0);
}

TEST(NetBuffer_CanRead)
{
    NetBuffer buf;
    buf.WriteUint16(100);

    EXPECT_TRUE(buf.CanRead(1));
    EXPECT_TRUE(buf.CanRead(2));
    EXPECT_FALSE(buf.CanRead(3));
    EXPECT_FALSE(buf.CanRead(100));
}

TEST(NetBuffer_CanReadAfterError)
{
    NetBuffer buf;
    buf.ReadUint8(); // Error
    EXPECT_TRUE(buf.HasError());
    EXPECT_FALSE(buf.CanRead(0));
    EXPECT_FALSE(buf.CanRead(1));
}

TEST(NetBuffer_IsValid)
{
    NetBuffer buf;
    EXPECT_TRUE(buf.IsValid());
    buf.WriteUint8(42);
    buf.ReadUint8();
    EXPECT_TRUE(buf.IsValid());

    buf.ReadUint8(); // Overrun
    EXPECT_FALSE(buf.IsValid());
}

TEST(NetBuffer_ChainedErrorPropagation)
{
    // Simulate a malformed packet: first read succeeds, then error cascades
    NetBuffer buf;
    buf.WriteUint8(1);   // marker byte
    buf.WriteUint16(50); // claimed string length of 50

    auto marker = buf.ReadUint8();
    EXPECT_EQ(marker, (uint8_t)1);
    EXPECT_FALSE(buf.HasError());

    // ReadString reads len=50 but only 0 bytes remain after the length prefix
    auto str = buf.ReadString();
    EXPECT_TRUE(buf.HasError());
    EXPECT_TRUE(str.empty());

    // All further reads should also fail
    auto val = buf.ReadUint32();
    EXPECT_EQ(val, (uint32_t)0);
    EXPECT_TRUE(buf.HasError());
}
