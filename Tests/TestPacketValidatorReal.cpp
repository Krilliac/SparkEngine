/**
 * @file TestPacketValidatorReal.cpp
 * @brief Production-linked packet validation regression tests.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/PacketValidator.h"

using namespace Spark::Net;

TEST(PacketValidatorReal_UnknownCustomTypeRequiresAuthentication)
{
    PacketValidator validator;
    NetworkMessage message;
    message.type = static_cast<MessageType>(static_cast<uint16_t>(MessageType::UserDefined) + 7);

    auto rejected = validator.ValidatePacket(message, false, true);
    EXPECT_FALSE(rejected.valid);
    EXPECT_EQ(static_cast<int>(rejected.violation), static_cast<int>(PacketViolation::Unauthenticated));
    EXPECT_EQ(validator.GetStatistics().rejectedUnauthenticated, 1u);

    auto accepted = validator.ValidatePacket(message, true, true);
    EXPECT_TRUE(accepted.valid);
}

TEST(PacketValidatorReal_RegisteredCustomSchemaStillEnforcesPolicy)
{
    PacketValidator validator;
    const auto customType = static_cast<MessageType>(static_cast<uint16_t>(MessageType::UserDefined) + 8);
    validator.RegisterSchema(customType, {.minPayloadSize = 4,
                                          .maxPayloadSize = 4,
                                          .requiresAuth = true,
                                          .allowedFromClient = false,
                                          .allowedFromServer = true});

    NetworkMessage message;
    message.type = customType;
    message.payload.resize(3);
    EXPECT_FALSE(validator.ValidatePacket(message, true, false).valid);

    message.payload.resize(4);
    auto wrongDirection = validator.ValidatePacket(message, true, true);
    EXPECT_FALSE(wrongDirection.valid);
    EXPECT_EQ(static_cast<int>(wrongDirection.violation), static_cast<int>(PacketViolation::DirectionViolation));

    EXPECT_TRUE(validator.ValidatePacket(message, true, false).valid);
}

TEST(PacketValidatorReal_ConnectionAndAckSchemasMatchConsumers)
{
    PacketValidator validator;
    NetworkMessage message;

    message.type = MessageType::ConnectAccepted;
    message.payload.resize(3);
    EXPECT_FALSE(validator.ValidatePacket(message, false, false).valid);
    message.payload.resize(4);
    EXPECT_TRUE(validator.ValidatePacket(message, false, false).valid);
    message.payload.resize(8);
    EXPECT_TRUE(validator.ValidatePacket(message, false, false).valid);
    message.payload.resize(9);
    EXPECT_FALSE(validator.ValidatePacket(message, false, false).valid);

    message.type = MessageType::Ack;
    message.payload.resize(7);
    EXPECT_FALSE(validator.ValidatePacket(message, true, true).valid);
    message.payload.resize(8);
    EXPECT_TRUE(validator.ValidatePacket(message, true, true).valid);
    message.payload.resize(9);
    EXPECT_FALSE(validator.ValidatePacket(message, true, true).valid);
}
