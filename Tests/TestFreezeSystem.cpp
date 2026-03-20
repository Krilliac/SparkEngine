/**
 * @file TestFreezeSystem.cpp
 * @brief Tests for tag-validated save state serialization
 */

#include "TestFramework.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

    class TestFreezeBuffer
    {
      public:
        enum class Mode
        {
            Write,
            Read
        };

        explicit TestFreezeBuffer(Mode mode) : m_mode(mode) {}
        TestFreezeBuffer(Mode mode, std::vector<uint8_t> data) : m_mode(mode), m_data(std::move(data)) {}

        template <typename T> bool Freeze(T& value)
        {
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
                return true;
            }
            if (m_readPos + sizeof(T) > m_data.size())
                return false;
            std::memcpy(&value, m_data.data() + m_readPos, sizeof(T));
            m_readPos += sizeof(T);
            return true;
        }

        template <typename T> bool FreezeLegacy(T& value, size_t savedSize)
        {
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
                return true;
            }
            size_t readAmount = std::min(savedSize, sizeof(T));
            if (m_readPos + savedSize > m_data.size())
                return false;
            std::memset(&value, 0, sizeof(T));
            std::memcpy(&value, m_data.data() + m_readPos, readAmount);
            m_readPos += savedSize;
            return true;
        }

        bool FreezeTag(const std::string& tag)
        {
            if (m_mode == Mode::Write)
            {
                uint32_t len = static_cast<uint32_t>(tag.size());
                Freeze(len);
                const auto* bytes = reinterpret_cast<const uint8_t*>(tag.data());
                m_data.insert(m_data.end(), bytes, bytes + len);
                return true;
            }
            uint32_t len = 0;
            if (!Freeze(len))
                return false;
            if (m_readPos + len > m_data.size())
                return false;
            std::string stored(reinterpret_cast<const char*>(m_data.data() + m_readPos), len);
            m_readPos += len;
            return stored == tag;
        }

        const std::vector<uint8_t>& GetData() const { return m_data; }
        size_t GetSize() const { return m_data.size(); }

      private:
        Mode m_mode;
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
    };

} // anonymous namespace

TEST(FreezeBuffer_RoundTrip)
{
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);

    int32_t intVal = 42;
    float floatVal = 3.14f;
    writer.Freeze(intVal);
    writer.Freeze(floatVal);

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    int32_t readInt = 0;
    float readFloat = 0.0f;
    EXPECT_TRUE(reader.Freeze(readInt));
    EXPECT_TRUE(reader.Freeze(readFloat));
    EXPECT_EQ(readInt, 42);
    EXPECT_NEAR(readFloat, 3.14f, 0.001f);
}

TEST(FreezeBuffer_TagValidation)
{
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    writer.FreezeTag("Physics");
    int32_t val = 100;
    writer.Freeze(val);
    writer.FreezeTag("Audio");

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    EXPECT_TRUE(reader.FreezeTag("Physics"));
    int32_t readVal = 0;
    reader.Freeze(readVal);
    EXPECT_EQ(readVal, 100);
    EXPECT_TRUE(reader.FreezeTag("Audio"));
}

TEST(FreezeBuffer_TagMismatch)
{
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    writer.FreezeTag("Physics");

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    EXPECT_FALSE(reader.FreezeTag("Audio"));
}

TEST(FreezeBuffer_LegacyCompat)
{
    struct OldStruct
    {
        int32_t x;
        int32_t y;
    };
    struct NewStruct
    {
        int32_t x;
        int32_t y;
        int32_t z;
        int32_t w;
    };

    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    OldStruct old = {10, 20};
    writer.Freeze(old);

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    NewStruct expanded = {};
    EXPECT_TRUE(reader.FreezeLegacy(expanded, sizeof(OldStruct)));
    EXPECT_EQ(expanded.x, 10);
    EXPECT_EQ(expanded.y, 20);
    EXPECT_EQ(expanded.z, 0); // Zero-filled new field
    EXPECT_EQ(expanded.w, 0);
}

TEST(FreezeSystem_SubsystemOrchestration)
{
    struct PhysicsState
    {
        float gravity = -9.81f;
        int subSteps = 4;
    };

    struct AudioState
    {
        float masterVolume = 0.8f;
        int activeVoices = 24;
    };

    PhysicsState physState = {-9.81f, 4};
    AudioState audioState = {0.8f, 24};

    // Write
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    uint32_t version = 1;
    writer.Freeze(version);
    writer.FreezeTag("Physics");
    writer.Freeze(physState);
    writer.FreezeTag("Audio");
    writer.Freeze(audioState);
    writer.FreezeTag("END");

    // Read back
    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    uint32_t readVersion = 0;
    reader.Freeze(readVersion);
    EXPECT_EQ(readVersion, 1u);
    EXPECT_TRUE(reader.FreezeTag("Physics"));
    PhysicsState readPhys = {};
    reader.Freeze(readPhys);
    EXPECT_NEAR(readPhys.gravity, -9.81f, 0.001f);
    EXPECT_EQ(readPhys.subSteps, 4);
    EXPECT_TRUE(reader.FreezeTag("Audio"));
    AudioState readAudio = {};
    reader.Freeze(readAudio);
    EXPECT_NEAR(readAudio.masterVolume, 0.8f, 0.001f);
    EXPECT_EQ(readAudio.activeVoices, 24);
    EXPECT_TRUE(reader.FreezeTag("END"));
}
