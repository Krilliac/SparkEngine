/**
 * @file TestSceneSnapshotSerializer.cpp
 * @brief Tests for Spark::Editor::SceneSnapshotSerializer and binary IO helpers
 */

#include "TestFramework.h"
#include "Engine/Editor/SceneSnapshotSerializer.h"

// ============================================================================
// SnapshotWriter / SnapshotReader round-trip tests
// ============================================================================

TEST(SnapshotWriter_U32_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(0);
    writer.WriteU32(42);
    writer.WriteU32(0xDEADBEEF);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_EQ(reader.ReadU32(), static_cast<uint32_t>(0));
    EXPECT_EQ(reader.ReadU32(), static_cast<uint32_t>(42));
    EXPECT_EQ(reader.ReadU32(), static_cast<uint32_t>(0xDEADBEEF));
    EXPECT_TRUE(reader.IsValid());
}

TEST(SnapshotWriter_U64_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU64(0);
    writer.WriteU64(123456789ULL);
    writer.WriteU64(0xFFFFFFFFFFFFFFFFULL);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_EQ(reader.ReadU64(), static_cast<uint64_t>(0));
    EXPECT_EQ(reader.ReadU64(), static_cast<uint64_t>(123456789ULL));
    EXPECT_EQ(reader.ReadU64(), static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL));
}

TEST(SnapshotWriter_Float_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteFloat(0.0f);
    writer.WriteFloat(3.14159f);
    writer.WriteFloat(-1.0f);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_NEAR(reader.ReadFloat(), 0.0f, 0.0001f);
    EXPECT_NEAR(reader.ReadFloat(), 3.14159f, 0.0001f);
    EXPECT_NEAR(reader.ReadFloat(), -1.0f, 0.0001f);
}

TEST(SnapshotWriter_Bool_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteBool(true);
    writer.WriteBool(false);
    writer.WriteBool(true);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_TRUE(reader.ReadBool());
    EXPECT_FALSE(reader.ReadBool());
    EXPECT_TRUE(reader.ReadBool());
}

TEST(SnapshotWriter_String_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteString("");
    writer.WriteString("Hello");
    writer.WriteString("SparkEngine Scene Data 2025");

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_EQ(reader.ReadString(), std::string(""));
    EXPECT_EQ(reader.ReadString(), std::string("Hello"));
    EXPECT_EQ(reader.ReadString(), std::string("SparkEngine Scene Data 2025"));
}

TEST(SnapshotWriter_Float3_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteFloat3(1.0f, 2.0f, 3.0f);
    writer.WriteFloat3(-5.5f, 0.0f, 100.0f);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    float x, y, z;
    reader.ReadFloat3(x, y, z);
    EXPECT_NEAR(x, 1.0f, 0.0001f);
    EXPECT_NEAR(y, 2.0f, 0.0001f);
    EXPECT_NEAR(z, 3.0f, 0.0001f);

    reader.ReadFloat3(x, y, z);
    EXPECT_NEAR(x, -5.5f, 0.0001f);
    EXPECT_NEAR(y, 0.0f, 0.0001f);
    EXPECT_NEAR(z, 100.0f, 0.0001f);
}

TEST(SnapshotWriter_MixedTypes_RoundTrip)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(10);
    writer.WriteString("TestEntity");
    writer.WriteFloat(3.14f);
    writer.WriteBool(true);
    writer.WriteU64(999);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_EQ(reader.ReadU32(), static_cast<uint32_t>(10));
    EXPECT_EQ(reader.ReadString(), std::string("TestEntity"));
    EXPECT_NEAR(reader.ReadFloat(), 3.14f, 0.01f);
    EXPECT_TRUE(reader.ReadBool());
    EXPECT_EQ(reader.ReadU64(), static_cast<uint64_t>(999));
    EXPECT_TRUE(reader.IsValid());
}

TEST(SnapshotWriter_Size)
{
    Spark::Editor::SnapshotWriter writer;
    EXPECT_EQ(writer.Size(), static_cast<size_t>(0));

    writer.WriteU32(1);
    EXPECT_EQ(writer.Size(), static_cast<size_t>(4));

    writer.WriteFloat(1.0f);
    EXPECT_EQ(writer.Size(), static_cast<size_t>(8));

    writer.WriteBool(true);
    EXPECT_EQ(writer.Size(), static_cast<size_t>(9));
}

TEST(SnapshotReader_HasRemaining)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(42);

    Spark::Editor::SnapshotReader reader(writer.GetData());
    EXPECT_TRUE(reader.HasRemaining(4));
    EXPECT_FALSE(reader.HasRemaining(5));

    reader.ReadU32();
    EXPECT_FALSE(reader.HasRemaining(1));
}

// ============================================================================
// SceneSnapshotSerializer tests
// ============================================================================

TEST(SceneSnapshotSerializer_Validate_EmptyData)
{
    std::vector<uint8_t> empty;
    EXPECT_FALSE(Spark::Editor::SceneSnapshotSerializer::Validate(empty));
}

TEST(SceneSnapshotSerializer_Validate_TooSmall)
{
    std::vector<uint8_t> small = {0x01, 0x02, 0x03};
    EXPECT_FALSE(Spark::Editor::SceneSnapshotSerializer::Validate(small));
}

TEST(SceneSnapshotSerializer_Validate_BadMagic)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(0xBADBAD); // Wrong magic
    writer.WriteU32(1);
    writer.WriteU32(0);
    writer.WriteU32(0);

    EXPECT_FALSE(Spark::Editor::SceneSnapshotSerializer::Validate(writer.GetData()));
}

TEST(SceneSnapshotSerializer_Validate_GoodHeader)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(Spark::Editor::SNAPSHOT_MAGIC);
    writer.WriteU32(Spark::Editor::SNAPSHOT_VERSION);
    writer.WriteU32(5); // entity count
    writer.WriteU32(0); // component type count

    EXPECT_TRUE(Spark::Editor::SceneSnapshotSerializer::Validate(writer.GetData()));
}

TEST(SceneSnapshotSerializer_SerializeNoComponents)
{
    // With no registered component serializers, serialize should still produce valid data
    auto& registry = Spark::Editor::ComponentSerializerRegistry::Instance();
    // Note: registry may have entries from other tests, but Serialize should still work
    auto data = Spark::Editor::SceneSnapshotSerializer::Serialize(nullptr, 10);

    EXPECT_TRUE(data.size() >= 16);
    EXPECT_TRUE(Spark::Editor::SceneSnapshotSerializer::Validate(data));
}

TEST(SceneSnapshotSerializer_GetSnapshotInfo_Invalid)
{
    std::vector<uint8_t> empty;
    auto info = Spark::Editor::SceneSnapshotSerializer::GetSnapshotInfo(empty);
    EXPECT_TRUE(info.contains("Invalid"));
}

TEST(SceneSnapshotSerializer_GetSnapshotInfo_BadMagic)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(0xBAD);
    writer.WriteU32(1);
    writer.WriteU32(0);
    writer.WriteU32(0);

    auto info = Spark::Editor::SceneSnapshotSerializer::GetSnapshotInfo(writer.GetData());
    EXPECT_TRUE(info.contains("bad magic"));
}

TEST(SceneSnapshotSerializer_GetSnapshotInfo_Valid)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(Spark::Editor::SNAPSHOT_MAGIC);
    writer.WriteU32(Spark::Editor::SNAPSHOT_VERSION);
    writer.WriteU32(42); // entities
    writer.WriteU32(3);  // component types

    auto info = Spark::Editor::SceneSnapshotSerializer::GetSnapshotInfo(writer.GetData());
    EXPECT_TRUE(info.contains("42 entities"));
    EXPECT_TRUE(info.contains("3 component types"));
}

// ============================================================================
// ComponentSerializerRegistry tests
// ============================================================================

TEST(ComponentSerializerRegistry_RegisterAndFind)
{
    auto& reg = Spark::Editor::ComponentSerializerRegistry::Instance();
    size_t countBefore = reg.Count();

    Spark::Editor::ComponentSerializerEntry entry;
    entry.typeName = "TestComponent_RegisterFind";
    entry.typeId = 99999; // Use a high ID to avoid conflicts
    entry.serialize = [](Spark::Editor::SnapshotWriter&, const void*) -> uint32_t { return 0; };
    entry.deserialize = [](Spark::Editor::SnapshotReader&, void*, uint32_t) -> bool { return true; };
    reg.Register(entry);

    EXPECT_EQ(reg.Count(), countBefore + 1);

    const auto* found = reg.Find(99999);
    EXPECT_TRUE(found != nullptr);
    EXPECT_EQ(found->typeName, std::string("TestComponent_RegisterFind"));

    const auto* notFound = reg.Find(88888);
    EXPECT_TRUE(notFound == nullptr);
}

TEST(SnapshotWriter_TakeData)
{
    Spark::Editor::SnapshotWriter writer;
    writer.WriteU32(42);
    writer.WriteString("test");

    size_t sizeBefore = writer.Size();
    auto data = writer.TakeData();
    EXPECT_EQ(data.size(), sizeBefore);
    EXPECT_EQ(writer.Size(), static_cast<size_t>(0)); // Should be empty after move
}
