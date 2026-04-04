/**
 * @file TestPacketValidator.cpp
 * @brief Tests for the PacketValidator network security layer
 */

#include "TestFramework.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Inline minimal reproduction for unit testing
// ============================================================================

namespace TestPacket
{

    enum class MessageType : uint16_t
    {
        Connect = 1,
        ConnectAccepted = 2,
        Disconnect = 4,
        Heartbeat = 5,
        EntityStateUpdate = 9,
        ClientInput = 11,
        ChatMessage = 13,
        UserDefined = 1000
    };

    enum class PacketViolation : uint8_t
    {
        None,
        PayloadTooLarge,
        PayloadTooSmall,
        InvalidType,
        SchemaViolation,
        BadString,
        Unauthenticated,
        DirectionViolation
    };

    struct ValidationResult
    {
        bool valid = true;
        PacketViolation violation = PacketViolation::None;
        std::string reason;
    };

    struct MessageSchema
    {
        size_t minPayloadSize = 0;
        size_t maxPayloadSize = 4096;
        bool requiresAuth = false;
        bool allowedFromClient = true;
        bool allowedFromServer = true;
        bool sanitizeStrings = false;
    };

    struct NetworkMessage
    {
        MessageType type;
        uint32_t senderID = 0;
        std::vector<uint8_t> payload;
    };

    struct PacketValidationStats
    {
        uint64_t totalValidated = 0;
        uint64_t totalRejected = 0;
    };

    class PacketValidator
    {
      public:
        void SetMaxPayloadSize(size_t bytes) { m_maxPayloadSize = bytes; }
        void SetMaxStringLength(size_t chars) { m_maxStringLength = chars; }

        void RegisterSchema(MessageType type, const MessageSchema& schema) { m_schemas[type] = schema; }

        ValidationResult ValidatePacket(const NetworkMessage& msg, bool senderIsAuthenticated,
                                        bool senderIsClient) const
        {
            m_stats.totalValidated++;

            size_t payloadSize = msg.payload.size();

            if (payloadSize > m_maxPayloadSize)
            {
                m_stats.totalRejected++;
                return {false, PacketViolation::PayloadTooLarge, "Exceeds global max"};
            }

            auto schemaIt = m_schemas.find(msg.type);
            if (schemaIt == m_schemas.end())
            {
                if (static_cast<uint16_t>(msg.type) < static_cast<uint16_t>(MessageType::UserDefined))
                {
                    m_stats.totalRejected++;
                    return {false, PacketViolation::InvalidType, "Unknown type"};
                }
                return {true, PacketViolation::None, ""};
            }

            const auto& schema = schemaIt->second;

            if (payloadSize < schema.minPayloadSize)
            {
                m_stats.totalRejected++;
                return {false, PacketViolation::PayloadTooSmall, "Below minimum"};
            }

            if (payloadSize > schema.maxPayloadSize)
            {
                m_stats.totalRejected++;
                return {false, PacketViolation::PayloadTooLarge, "Exceeds type max"};
            }

            if (schema.requiresAuth && !senderIsAuthenticated)
            {
                m_stats.totalRejected++;
                return {false, PacketViolation::Unauthenticated, "Requires auth"};
            }

            if (senderIsClient && !schema.allowedFromClient)
            {
                m_stats.totalRejected++;
                return {false, PacketViolation::DirectionViolation, "Not allowed from client"};
            }

            if (!senderIsClient && !schema.allowedFromServer)
            {
                m_stats.totalRejected++;
                return {false, PacketViolation::DirectionViolation, "Not allowed from server"};
            }

            return {true, PacketViolation::None, ""};
        }

        bool ValidateString(const std::string& str) const
        {
            if (str.size() > m_maxStringLength)
                return false;

            for (char c : str)
            {
                if (c < 0x20 && c != '\n' && c != '\t' && c != '\r')
                    return false;
                if (c == 0x7F)
                    return false;
            }
            return true;
        }

        void SanitizeString(std::string& str) const
        {
            if (str.size() > m_maxStringLength)
                str.resize(m_maxStringLength);

            std::erase_if(str, [](char c) -> bool
                          { return (c < 0x20 && c != '\n' && c != '\t' && c != '\r') || c == 0x7F; });
        }

        PacketValidationStats GetStatistics() const { return m_stats; }

      private:
        size_t m_maxPayloadSize = 4096;
        size_t m_maxStringLength = 1024;
        std::unordered_map<MessageType, MessageSchema> m_schemas;
        mutable PacketValidationStats m_stats;
    };

} // namespace TestPacket

// ============================================================================
// Tests
// ============================================================================

TEST(PacketValidator_GlobalMaxPayloadSize)
{
    TestPacket::PacketValidator validator;
    validator.SetMaxPayloadSize(100);

    TestPacket::NetworkMessage msg;
    msg.type = TestPacket::MessageType::UserDefined;
    msg.payload.resize(50);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);

    msg.payload.resize(200);
    result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::PayloadTooLarge);
}

TEST(PacketValidator_PerTypePayloadBounds)
{
    TestPacket::PacketValidator validator;
    validator.RegisterSchema(TestPacket::MessageType::ClientInput, {.minPayloadSize = 8, .maxPayloadSize = 512});

    TestPacket::NetworkMessage msg;
    msg.type = TestPacket::MessageType::ClientInput;

    msg.payload.resize(4);
    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::PayloadTooSmall);

    msg.payload.resize(100);
    result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);

    msg.payload.resize(1000);
    result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::PayloadTooLarge);
}

TEST(PacketValidator_AuthenticationRequired)
{
    TestPacket::PacketValidator validator;
    validator.RegisterSchema(TestPacket::MessageType::Heartbeat, {.maxPayloadSize = 16, .requiresAuth = true});

    TestPacket::NetworkMessage msg;
    msg.type = TestPacket::MessageType::Heartbeat;
    msg.payload.resize(8);

    auto result = validator.ValidatePacket(msg, false, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::Unauthenticated);

    result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);
}

TEST(PacketValidator_DirectionEnforcement)
{
    TestPacket::PacketValidator validator;
    validator.RegisterSchema(
        TestPacket::MessageType::EntityStateUpdate,
        {.minPayloadSize = 8, .maxPayloadSize = 2048, .allowedFromClient = false, .allowedFromServer = true});

    TestPacket::NetworkMessage msg;
    msg.type = TestPacket::MessageType::EntityStateUpdate;
    msg.payload.resize(100);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::DirectionViolation);

    result = validator.ValidatePacket(msg, true, false);
    EXPECT_TRUE(result.valid);
}

TEST(PacketValidator_UnknownBuiltinTypeRejected)
{
    TestPacket::PacketValidator validator;

    TestPacket::NetworkMessage msg;
    msg.type = static_cast<TestPacket::MessageType>(999);
    msg.payload.resize(10);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::InvalidType);
}

TEST(PacketValidator_UserDefinedTypesPass)
{
    TestPacket::PacketValidator validator;

    TestPacket::NetworkMessage msg;
    msg.type = static_cast<TestPacket::MessageType>(1001);
    msg.payload.resize(100);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);
}

TEST(PacketValidator_StringValidation)
{
    TestPacket::PacketValidator validator;
    validator.SetMaxStringLength(50);

    EXPECT_TRUE(validator.ValidateString("Hello, world!"));
    EXPECT_TRUE(validator.ValidateString("Line 1\nLine 2\tTabbed"));

    std::string longStr(100, 'A');
    EXPECT_FALSE(validator.ValidateString(longStr));

    std::string withControl = "bad\x01string";
    EXPECT_FALSE(validator.ValidateString(withControl));

    std::string withDel = "bad\x7Fstring";
    EXPECT_FALSE(validator.ValidateString(withDel));
}

TEST(PacketValidator_StringSanitization)
{
    TestPacket::PacketValidator validator;
    validator.SetMaxStringLength(20);

    std::string str = "Hello\x01World\x7F!";
    validator.SanitizeString(str);
    EXPECT_EQ(str, std::string("HelloWorld!"));

    std::string normal = "Safe text";
    validator.SanitizeString(normal);
    EXPECT_EQ(normal, std::string("Safe text"));

    // Truncation test
    validator.SetMaxStringLength(5);
    std::string longStr = "Hello World";
    validator.SanitizeString(longStr);
    EXPECT_EQ(longStr, std::string("Hello"));
}

TEST(PacketValidator_StatisticsTracking)
{
    TestPacket::PacketValidator validator;
    validator.SetMaxPayloadSize(100);

    TestPacket::NetworkMessage good;
    good.type = TestPacket::MessageType::UserDefined;
    good.payload.resize(50);

    TestPacket::NetworkMessage bad;
    bad.type = TestPacket::MessageType::UserDefined;
    bad.payload.resize(200);

    validator.ValidatePacket(good, true, true);
    validator.ValidatePacket(good, true, true);
    validator.ValidatePacket(bad, true, true);

    auto stats = validator.GetStatistics();
    EXPECT_EQ(stats.totalValidated, 3u);
    EXPECT_EQ(stats.totalRejected, 1u);
}

TEST(PacketValidator_ConnectClientOnly)
{
    TestPacket::PacketValidator validator;
    validator.RegisterSchema(
        TestPacket::MessageType::Connect,
        {.maxPayloadSize = 256, .requiresAuth = false, .allowedFromClient = true, .allowedFromServer = false});

    TestPacket::NetworkMessage msg;
    msg.type = TestPacket::MessageType::Connect;
    msg.payload.resize(32);

    auto result = validator.ValidatePacket(msg, false, true);
    EXPECT_TRUE(result.valid);

    result = validator.ValidatePacket(msg, false, false);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == TestPacket::PacketViolation::DirectionViolation);
}
