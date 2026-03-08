// TestSaveSystem.cpp - Tests for save/load serialization logic
// Standalone tests for serialization utilities

#include "TestFramework.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <unordered_map>

namespace TestSave
{

    // Minimal NetBuffer-like serialization for testing
    class TestBuffer
    {
      public:
        void WriteUInt32(uint32_t val)
        {
            const char* p = reinterpret_cast<const char*>(&val);
            m_data.insert(m_data.end(), p, p + sizeof(val));
        }

        void WriteFloat(float val)
        {
            const char* p = reinterpret_cast<const char*>(&val);
            m_data.insert(m_data.end(), p, p + sizeof(val));
        }

        void WriteString(const std::string& str)
        {
            WriteUInt32(static_cast<uint32_t>(str.size()));
            m_data.insert(m_data.end(), str.begin(), str.end());
        }

        void WriteBool(bool val) { m_data.push_back(val ? 1 : 0); }

        uint32_t ReadUInt32()
        {
            uint32_t val;
            std::memcpy(&val, m_data.data() + m_readPos, sizeof(val));
            m_readPos += sizeof(val);
            return val;
        }

        float ReadFloat()
        {
            float val;
            std::memcpy(&val, m_data.data() + m_readPos, sizeof(val));
            m_readPos += sizeof(val);
            return val;
        }

        std::string ReadString()
        {
            uint32_t len = ReadUInt32();
            std::string str(m_data.data() + m_readPos, m_data.data() + m_readPos + len);
            m_readPos += len;
            return str;
        }

        bool ReadBool() { return m_data[m_readPos++] != 0; }

        size_t Size() const { return m_data.size(); }
        void ResetRead() { m_readPos = 0; }

      private:
        std::vector<char> m_data;
        size_t m_readPos = 0;
    };

    // Minimal component serializer
    struct SerializedComponent
    {
        std::string typeName;
        std::unordered_map<std::string, std::string> properties;
    };

    struct SerializedEntity
    {
        uint32_t id;
        std::string name;
        std::vector<SerializedComponent> components;
    };

} // namespace TestSave

TEST(Buffer_WriteReadUInt32)
{
    TestSave::TestBuffer buf;
    buf.WriteUInt32(42);
    buf.WriteUInt32(0);
    buf.WriteUInt32(4294967295u);
    buf.ResetRead();
    EXPECT_EQ(buf.ReadUInt32(), 42u);
    EXPECT_EQ(buf.ReadUInt32(), 0u);
    EXPECT_EQ(buf.ReadUInt32(), 4294967295u);
}

TEST(Buffer_WriteReadFloat)
{
    TestSave::TestBuffer buf;
    buf.WriteFloat(3.14f);
    buf.WriteFloat(-1.0f);
    buf.WriteFloat(0.0f);
    buf.ResetRead();
    EXPECT_NEAR(buf.ReadFloat(), 3.14f, 0.001f);
    EXPECT_NEAR(buf.ReadFloat(), -1.0f, 0.001f);
    EXPECT_NEAR(buf.ReadFloat(), 0.0f, 0.001f);
}

TEST(Buffer_WriteReadString)
{
    TestSave::TestBuffer buf;
    buf.WriteString("Hello World");
    buf.WriteString("");
    buf.WriteString("SparkEngine");
    buf.ResetRead();
    EXPECT_EQ(buf.ReadString(), std::string("Hello World"));
    EXPECT_EQ(buf.ReadString(), std::string(""));
    EXPECT_EQ(buf.ReadString(), std::string("SparkEngine"));
}

TEST(Buffer_WriteReadBool)
{
    TestSave::TestBuffer buf;
    buf.WriteBool(true);
    buf.WriteBool(false);
    buf.WriteBool(true);
    buf.ResetRead();
    EXPECT_TRUE(buf.ReadBool());
    EXPECT_FALSE(buf.ReadBool());
    EXPECT_TRUE(buf.ReadBool());
}

TEST(Buffer_MixedTypes)
{
    TestSave::TestBuffer buf;
    buf.WriteUInt32(1);
    buf.WriteString("test_entity");
    buf.WriteFloat(3.0f);
    buf.WriteBool(true);
    buf.ResetRead();
    EXPECT_EQ(buf.ReadUInt32(), 1u);
    EXPECT_EQ(buf.ReadString(), std::string("test_entity"));
    EXPECT_NEAR(buf.ReadFloat(), 3.0f, 0.001f);
    EXPECT_TRUE(buf.ReadBool());
}

TEST(SerializedEntity_BasicStructure)
{
    TestSave::SerializedEntity entity;
    entity.id = 1;
    entity.name = "TestEntity";

    TestSave::SerializedComponent comp;
    comp.typeName = "Transform";
    comp.properties["position_x"] = "1.0";
    comp.properties["position_y"] = "2.0";
    comp.properties["position_z"] = "3.0";
    entity.components.push_back(comp);

    EXPECT_EQ(entity.id, 1u);
    EXPECT_EQ(entity.name, std::string("TestEntity"));
    EXPECT_EQ(entity.components.size(), 1u);
    EXPECT_EQ(entity.components[0].typeName, std::string("Transform"));
    EXPECT_EQ(entity.components[0].properties.size(), 3u);
}

TEST(SerializedEntity_MultipleComponents)
{
    TestSave::SerializedEntity entity;
    entity.id = 42;
    entity.name = "Player";

    TestSave::SerializedComponent transform;
    transform.typeName = "Transform";
    entity.components.push_back(transform);

    TestSave::SerializedComponent health;
    health.typeName = "HealthComponent";
    health.properties["health"] = "100.0";
    health.properties["maxHealth"] = "100.0";
    entity.components.push_back(health);

    TestSave::SerializedComponent mesh;
    mesh.typeName = "MeshRenderer";
    mesh.properties["meshPath"] = "models/player.obj";
    entity.components.push_back(mesh);

    EXPECT_EQ(entity.components.size(), 3u);
    EXPECT_EQ(entity.components[1].properties["health"], std::string("100.0"));
    EXPECT_EQ(entity.components[2].properties["meshPath"], std::string("models/player.obj"));
}
