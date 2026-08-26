/**
 * @file TestEngineInterfaceProtocol.cpp
 * @brief Adversarial coverage for the editor/engine event decoder.
 */

#include "TestFramework.h"
#include "Communication/EngineInterface.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace
{
    void PutU32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    }

    void PutU64(std::vector<uint8_t>& bytes, uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    }

    void PutString(std::vector<uint8_t>& bytes, const std::string& value)
    {
        PutU32(bytes, static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    std::vector<uint8_t> EventPrefix()
    {
        std::vector<uint8_t> bytes;
        PutU32(bytes, 0x53504B45u);
        PutU32(bytes, static_cast<uint32_t>(SparkEditor::EngineEventType::ENGINE_READY));
        PutU64(bytes, 42);
        PutU64(bytes, 1234);
        PutU32(bytes, 1);
        PutString(bytes, "engine");
        PutString(bytes, "ready");
        return bytes;
    }
}

TEST(EngineInterfaceProtocol_ValidEventRoundTripsAllFields)
{
    auto bytes = EventPrefix();
    PutU32(bytes, 1);
    PutString(bytes, "backend");
    PutString(bytes, "D3D11");
    PutU32(bytes, 3);
    bytes.insert(bytes.end(), {1, 2, 3});

    SparkEditor::EngineEvent event;
    ASSERT_TRUE(SparkEditor::EngineInterface::DeserializeEvent(bytes, event));
    EXPECT_TRUE(event.type == SparkEditor::EngineEventType::ENGINE_READY);
    EXPECT_EQ(event.eventID, uint64_t{42});
    EXPECT_EQ(event.data.at("backend"), std::string("D3D11"));
    EXPECT_EQ(event.binaryData.size(), size_t{3});
}

TEST(EngineInterfaceProtocol_RejectsImpossibleDataCountTransactionally)
{
    auto bytes = EventPrefix();
    PutU32(bytes, std::numeric_limits<uint32_t>::max());

    SparkEditor::EngineEvent event;
    event.message = "sentinel";
    EXPECT_FALSE(SparkEditor::EngineInterface::DeserializeEvent(bytes, event));
    EXPECT_EQ(event.message, std::string("sentinel"));
}

TEST(EngineInterfaceProtocol_RejectsTruncatedStringAndBinary)
{
    auto truncatedString = EventPrefix();
    PutU32(truncatedString, 1);
    PutU32(truncatedString, 1000);
    SparkEditor::EngineEvent event;
    EXPECT_FALSE(SparkEditor::EngineInterface::DeserializeEvent(truncatedString, event));

    auto truncatedBinary = EventPrefix();
    PutU32(truncatedBinary, 0);
    PutU32(truncatedBinary, 4096);
    truncatedBinary.push_back(1);
    EXPECT_FALSE(SparkEditor::EngineInterface::DeserializeEvent(truncatedBinary, event));
}

TEST(EngineInterfaceProtocol_RejectsTrailingOrOversizedMessages)
{
    auto bytes = EventPrefix();
    PutU32(bytes, 0);
    PutU32(bytes, 0);
    bytes.push_back(0xAA);
    SparkEditor::EngineEvent event;
    EXPECT_FALSE(SparkEditor::EngineInterface::DeserializeEvent(bytes, event));

    std::vector<uint8_t> oversized(SparkEditor::EngineInterface::kMaxEventMessageSize + 1u);
    EXPECT_FALSE(SparkEditor::EngineInterface::DeserializeEvent(oversized, event));
}
