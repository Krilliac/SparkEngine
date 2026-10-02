/**
 * @file MMOChatWire.cpp
 * @brief MMO chat wire codec and server relay policy (MMOChatSystem statics).
 *
 * Split from MMOChatSystem.cpp so these functions -- the ones HandleNetworkChat
 * runs on every client chat datagram -- link without NetworkManager, the
 * console or the rest of the chat system. The SEC-120 fuzz target
 * (FuzzerTests/FuzzMMOChatWire.cpp) compiles this file with only the NetBuffer
 * reader. Thread affinity: none (pure functions).
 */

#include "MMOChatSystem.h"

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/NetworkManager.h"

namespace MMO
{
    std::optional<MMOChatSystem::WirePayload> MMOChatSystem::DecodeWirePayload(const std::vector<uint8_t>& payload)
    {
        if (payload.size() < 2)
        {
            return std::nullopt;
        }

        Spark::Net::NetBuffer buf;
        buf.WriteBytes(payload.data(), payload.size());
        const uint8_t channelValue = buf.ReadUint8();
        WirePayload decoded;
        decoded.senderName = buf.ReadString();
        decoded.text = buf.ReadString();
        if (buf.HasError() || channelValue > static_cast<uint8_t>(ChatChannel::Whisper) || decoded.senderName.empty() ||
            decoded.text.empty())
        {
            return std::nullopt;
        }
        decoded.channel = static_cast<ChatChannel>(channelValue);
        return decoded;
    }

    std::vector<uint8_t> MMOChatSystem::EncodeWirePayload(ChatChannel channel, const std::string& senderName,
                                                          const std::string& text)
    {
        Spark::Net::NetBuffer buf;
        buf.WriteUint8(static_cast<uint8_t>(channel));
        buf.WriteString(senderName);
        buf.WriteString(text);
        return {buf.GetData().begin(), buf.GetData().end()};
    }

    std::string MMOChatSystem::ServerAttributedSenderName(std::string_view connectionName, uint32_t clientId)
    {
        // Printable ASCII only: no control characters (log/UI injection), no bytes >= 0x80 (bidi overrides
        // and look-alike glyphs), and no '#', which is reserved for the id suffix appended below.
        std::string display;
        display.reserve(MAX_SENDER_DISPLAY_NAME);
        for (const char c : connectionName)
        {
            if (display.size() >= MAX_SENDER_DISPLAY_NAME)
            {
                break;
            }
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte < 0x7F && c != '#')
            {
                display.push_back(c);
            }
        }
        if (display.find_first_not_of(' ') == std::string::npos)
        {
            display = "Player";
        }
        return display + "#" + std::to_string(clientId);
    }

    std::optional<std::vector<uint8_t>> MMOChatSystem::BuildServerRelayPayload(
        const std::vector<uint8_t>& clientPayload, std::string_view connectionName, uint32_t senderClientId)
    {
        if (senderClientId == Spark::Net::INVALID_CLIENT)
        {
            return std::nullopt;
        }
        const std::optional<WirePayload> decoded = DecodeWirePayload(clientPayload);
        if (!decoded || !IsNetworkRoutableChannel(decoded->channel))
        {
            return std::nullopt;
        }
        return EncodeWirePayload(decoded->channel, ServerAttributedSenderName(connectionName, senderClientId),
                                 decoded->text);
    }
} // namespace MMO

#endif // ENABLE_NETWORKING
