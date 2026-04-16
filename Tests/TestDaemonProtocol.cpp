/**
 * @file TestDaemonProtocol.cpp
 * @brief Codec tests for the SparkDaemon wire format (frame header + whole-frame round-trip).
 */

#include "TestFramework.h"
#include "Utils/DaemonProtocol.h"

#include <cstdint>
#include <vector>

using Spark::Daemon::ControlMessage;
using Spark::Daemon::DecodeFrameHeader;
using Spark::Daemon::EncodeFrame;
using Spark::Daemon::EncodeFrameHeader;
using Spark::Daemon::FrameHeader;
using Spark::Daemon::kFrameHeaderSize;
using Spark::Daemon::kMaxPayloadSize;
using Spark::Daemon::ServiceId;

TEST(DaemonProtocol_HeaderRoundTrip)
{
    FrameHeader in;
    in.payloadSize = 0x12345678u;
    in.serviceId = static_cast<uint16_t>(ServiceId::Shader);
    in.messageType = 0xABCDu;

    uint8_t bytes[kFrameHeaderSize];
    EncodeFrameHeader(in, bytes);
    FrameHeader out = DecodeFrameHeader(bytes);

    EXPECT_EQ(out.payloadSize, in.payloadSize);
    EXPECT_EQ(out.serviceId, in.serviceId);
    EXPECT_EQ(out.messageType, in.messageType);
}

TEST(DaemonProtocol_HeaderWireLayoutIsLittleEndian)
{
    FrameHeader h;
    h.payloadSize = 0x04030201u;
    h.serviceId = 0x0605u;
    h.messageType = 0x0807u;

    uint8_t bytes[kFrameHeaderSize];
    EncodeFrameHeader(h, bytes);

    EXPECT_EQ(bytes[0], 0x01u);
    EXPECT_EQ(bytes[1], 0x02u);
    EXPECT_EQ(bytes[2], 0x03u);
    EXPECT_EQ(bytes[3], 0x04u);
    EXPECT_EQ(bytes[4], 0x05u);
    EXPECT_EQ(bytes[5], 0x06u);
    EXPECT_EQ(bytes[6], 0x07u);
    EXPECT_EQ(bytes[7], 0x08u);
}

TEST(DaemonProtocol_EncodeFrameEmptyPayload)
{
    auto frame = EncodeFrame(ServiceId::Control, static_cast<uint16_t>(ControlMessage::PingRequest), {});
    EXPECT_EQ(frame.size(), kFrameHeaderSize);

    FrameHeader decoded = DecodeFrameHeader(frame.data());
    EXPECT_EQ(decoded.payloadSize, 0u);
    EXPECT_EQ(decoded.serviceId, static_cast<uint16_t>(ServiceId::Control));
    EXPECT_EQ(decoded.messageType, static_cast<uint16_t>(ControlMessage::PingRequest));
}

TEST(DaemonProtocol_EncodeFrameWithPayload)
{
    std::vector<uint8_t> payload = {'p', 'o', 'n', 'g'};
    auto frame = EncodeFrame(ServiceId::Control, static_cast<uint16_t>(ControlMessage::PingResponse), payload);
    EXPECT_EQ(frame.size(), kFrameHeaderSize + payload.size());

    FrameHeader decoded = DecodeFrameHeader(frame.data());
    EXPECT_EQ(decoded.payloadSize, 4u);
    EXPECT_EQ(decoded.messageType, static_cast<uint16_t>(ControlMessage::PingResponse));

    for (size_t i = 0; i < payload.size(); ++i)
        EXPECT_EQ(frame[kFrameHeaderSize + i], payload[i]);
}

TEST(DaemonProtocol_ServiceIdConstantsAreStable)
{
    // These are wire-format constants — must never change or it breaks deployed clients.
    EXPECT_EQ(static_cast<uint16_t>(ServiceId::Control), 0x0000u);
    EXPECT_EQ(static_cast<uint16_t>(ServiceId::Asset), 0x0001u);
    EXPECT_EQ(static_cast<uint16_t>(ServiceId::Shader), 0x0002u);
    EXPECT_EQ(static_cast<uint16_t>(ServiceId::Collab), 0x0003u);
    EXPECT_EQ(static_cast<uint16_t>(ServiceId::Build), 0x0004u);
}

TEST(DaemonProtocol_MaxPayloadSizeIs16MiB)
{
    EXPECT_EQ(kMaxPayloadSize, 16u * 1024u * 1024u);
}
