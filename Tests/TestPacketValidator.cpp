/**
 * @file TestPacketValidator.cpp
 * @brief PacketValidator schema/auth/direction contract against the SHIPPED class.
 *
 * This file used to declare its own namespace TestPacket with a private copy of
 * MessageSchema/PacketValidator/ValidateString/SanitizeString and included no
 * engine header, so the coverage it appeared to provide said nothing about the
 * code that ships. The mock is gone; every assertion below runs
 * Engine/Networking/PacketValidator.cpp.
 *
 * Hostile-input regressions (string-field screening, malformed length prefixes)
 * live in TestSecurityParsersReal.cpp.
 */

#include "TestFramework.h"

#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/PacketValidator.h"

#include <string>

using namespace Spark::Net;

TEST(PacketValidator_GlobalMaxPayloadSize)
{
    PacketValidator validator;
    validator.SetMaxPayloadSize(100);

    NetworkMessage msg;
    msg.type = MessageType::UserDefined;
    msg.payload.resize(50);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);

    msg.payload.resize(200);
    result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::PayloadTooLarge);
}

TEST(PacketValidator_PerTypePayloadBounds)
{
    PacketValidator validator;
    validator.RegisterSchema(MessageType::ClientInput, {.minPayloadSize = 8, .maxPayloadSize = 512});

    NetworkMessage msg;
    msg.type = MessageType::ClientInput;

    msg.payload.resize(4);
    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::PayloadTooSmall);

    msg.payload.resize(100);
    result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);

    msg.payload.resize(1000);
    result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::PayloadTooLarge);
}

TEST(PacketValidator_AuthenticationRequired)
{
    PacketValidator validator;
    validator.RegisterSchema(MessageType::Heartbeat, {.maxPayloadSize = 16, .requiresAuth = true});

    NetworkMessage msg;
    msg.type = MessageType::Heartbeat;
    msg.payload.resize(8);

    auto result = validator.ValidatePacket(msg, false, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::Unauthenticated);

    result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);
}

TEST(PacketValidator_DirectionEnforcement)
{
    PacketValidator validator;
    validator.RegisterSchema(
        MessageType::EntityStateUpdate,
        {.minPayloadSize = 8, .maxPayloadSize = 2048, .allowedFromClient = false, .allowedFromServer = true});

    NetworkMessage msg;
    msg.type = MessageType::EntityStateUpdate;
    msg.payload.resize(100);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::DirectionViolation);

    result = validator.ValidatePacket(msg, true, false);
    EXPECT_TRUE(result.valid);
}

TEST(PacketValidator_UnknownBuiltinTypeRejected)
{
    PacketValidator validator;

    NetworkMessage msg;
    msg.type = static_cast<MessageType>(999);
    msg.payload.resize(10);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::InvalidType);
}

TEST(PacketValidator_UserDefinedTypesPass)
{
    PacketValidator validator;

    NetworkMessage msg;
    msg.type = static_cast<MessageType>(1001);
    msg.payload.resize(100);

    auto result = validator.ValidatePacket(msg, true, true);
    EXPECT_TRUE(result.valid);
}

TEST(PacketValidator_StringValidation)
{
    PacketValidator validator;
    validator.SetMaxStringLength(50);

    EXPECT_TRUE(validator.ValidateString("Hello, world!"));
    EXPECT_TRUE(validator.ValidateString("Line 1\nLine 2\tTabbed"));

    const std::string longStr(100, 'A');
    EXPECT_FALSE(validator.ValidateString(longStr));

    const std::string withControl = "bad\x01string";
    EXPECT_FALSE(validator.ValidateString(withControl));

    const std::string withDel = "bad\x7Fstring";
    EXPECT_FALSE(validator.ValidateString(withDel));
}

TEST(PacketValidator_StringSanitization)
{
    PacketValidator validator;
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
    PacketValidator validator;
    validator.SetMaxPayloadSize(100);

    NetworkMessage good;
    good.type = MessageType::UserDefined;
    good.payload.resize(50);

    NetworkMessage bad;
    bad.type = MessageType::UserDefined;
    bad.payload.resize(200);

    EXPECT_TRUE(validator.ValidatePacket(good, true, true).valid);
    EXPECT_TRUE(validator.ValidatePacket(good, true, true).valid);
    EXPECT_FALSE(validator.ValidatePacket(bad, true, true).valid);

    const auto stats = validator.GetStatistics();
    EXPECT_EQ(stats.totalValidated, 3u);
    EXPECT_EQ(stats.totalRejected, 1u);
}

TEST(PacketValidator_ConnectClientOnly)
{
    PacketValidator validator;
    validator.RegisterSchema(
        MessageType::Connect,
        {.maxPayloadSize = 256, .requiresAuth = false, .allowedFromClient = true, .allowedFromServer = false});

    NetworkMessage msg;
    msg.type = MessageType::Connect;
    msg.payload.resize(32);

    auto result = validator.ValidatePacket(msg, false, true);
    EXPECT_TRUE(result.valid);

    result = validator.ValidatePacket(msg, false, false);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == PacketViolation::DirectionViolation);
}
