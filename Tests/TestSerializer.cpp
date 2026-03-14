// TestSerializer.cpp - Tests for Spark::BinaryWriter and BinaryReader
#include "TestFramework.h"
#include "Utils/Serializer.h"

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// BinaryWriter basic writes
// ============================================================================

TEST(Serializer_Writer_Empty)
{
    Spark::BinaryWriter writer;
    EXPECT_EQ(writer.Size(), 0u);
    EXPECT_TRUE(writer.GetBuffer().empty());
}

TEST(Serializer_Writer_WriteUint32)
{
    Spark::BinaryWriter writer;
    writer.Write<uint32_t>(0xDEADBEEFu);
    EXPECT_EQ(writer.Size(), 4u);
}

TEST(Serializer_Writer_WriteFloat)
{
    Spark::BinaryWriter writer;
    writer.Write<float>(3.14f);
    EXPECT_EQ(writer.Size(), 4u);
}

TEST(Serializer_Writer_WriteString)
{
    Spark::BinaryWriter writer;
    writer.WriteString("Hello");
    // 4 bytes length prefix + 5 bytes data
    EXPECT_EQ(writer.Size(), 9u);
}

TEST(Serializer_Writer_WriteEmptyString)
{
    Spark::BinaryWriter writer;
    writer.WriteString("");
    // 4 bytes length prefix + 0 bytes data
    EXPECT_EQ(writer.Size(), 4u);
}

TEST(Serializer_Writer_Clear)
{
    Spark::BinaryWriter writer;
    writer.Write<int>(42);
    writer.Clear();
    EXPECT_EQ(writer.Size(), 0u);
}

TEST(Serializer_Writer_TakeBuffer)
{
    Spark::BinaryWriter writer;
    writer.Write<uint8_t>(0xAB);
    auto buf = writer.TakeBuffer();
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0], 0xABu);
    EXPECT_EQ(writer.Size(), 0u); // Writer is now empty
}

// ============================================================================
// BinaryReader basic reads
// ============================================================================

TEST(Serializer_Reader_Uint32_RoundTrip)
{
    Spark::BinaryWriter writer;
    writer.Write<uint32_t>(12345u);
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    auto val = reader.Read<uint32_t>();
    EXPECT_EQ(val, 12345u);
    EXPECT_FALSE(reader.HasError());
    EXPECT_TRUE(reader.IsEOF());
}

TEST(Serializer_Reader_Float_RoundTrip)
{
    Spark::BinaryWriter writer;
    writer.Write<float>(2.718f);
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    auto val = reader.Read<float>();
    EXPECT_NEAR(val, 2.718f, 0.0001f);
    EXPECT_FALSE(reader.HasError());
}

TEST(Serializer_Reader_String_RoundTrip)
{
    Spark::BinaryWriter writer;
    writer.WriteString("SparkEngine");
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    auto str = reader.ReadString();
    EXPECT_TRUE(str == "SparkEngine");
    EXPECT_FALSE(reader.HasError());
}

TEST(Serializer_Reader_MultipleValues)
{
    Spark::BinaryWriter writer;
    writer.Write<uint8_t>(1u);
    writer.Write<uint16_t>(2u);
    writer.Write<uint32_t>(3u);
    writer.WriteString("four");
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    EXPECT_EQ(reader.Read<uint8_t>(), 1u);
    EXPECT_EQ(reader.Read<uint16_t>(), 2u);
    EXPECT_EQ(reader.Read<uint32_t>(), 3u);
    EXPECT_TRUE(reader.ReadString() == "four");
    EXPECT_FALSE(reader.HasError());
    EXPECT_TRUE(reader.IsEOF());
}

TEST(Serializer_Reader_Bool_RoundTrip)
{
    Spark::BinaryWriter writer;
    writer.Write<bool>(true);
    writer.Write<bool>(false);
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    EXPECT_TRUE(reader.Read<bool>());
    EXPECT_FALSE(reader.Read<bool>());
    EXPECT_FALSE(reader.HasError());
}

// ============================================================================
// Error handling
// ============================================================================

TEST(Serializer_Reader_Overflow_SetsError)
{
    std::vector<uint8_t> twoBytes = {0x01, 0x02};
    Spark::BinaryReader reader(twoBytes);
    reader.Read<uint32_t>(); // Attempts to read 4 bytes from 2 — overflow
    EXPECT_TRUE(reader.HasError());
}

TEST(Serializer_Reader_Overflow_ReturnsDefault)
{
    std::vector<uint8_t> empty;
    Spark::BinaryReader reader(empty);
    auto val = reader.Read<int>();
    EXPECT_EQ(val, 0);
    EXPECT_TRUE(reader.HasError());
}

TEST(Serializer_Reader_Tell_And_Remaining)
{
    Spark::BinaryWriter writer;
    writer.Write<uint32_t>(0u);
    writer.Write<uint32_t>(0u);
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    EXPECT_EQ(reader.Tell(), 0u);
    EXPECT_EQ(reader.Remaining(), 8u);
    reader.Read<uint32_t>();
    EXPECT_EQ(reader.Tell(), 4u);
    EXPECT_EQ(reader.Remaining(), 4u);
}

TEST(Serializer_Reader_Skip)
{
    Spark::BinaryWriter writer;
    writer.Write<uint32_t>(0xDEAD0000u);
    writer.Write<uint32_t>(42u);
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    reader.Skip(4); // Skip first uint32
    auto val = reader.Read<uint32_t>();
    EXPECT_EQ(val, 42u);
    EXPECT_FALSE(reader.HasError());
}

TEST(Serializer_Reader_EmptyString_RoundTrip)
{
    Spark::BinaryWriter writer;
    writer.WriteString("");
    auto buf = writer.TakeBuffer();

    Spark::BinaryReader reader(buf);
    auto str = reader.ReadString();
    EXPECT_TRUE(str.empty());
    EXPECT_FALSE(reader.HasError());
}
