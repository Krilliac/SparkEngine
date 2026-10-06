/**
 * @file FuzzMMOChatWireProduction.cpp
 * @brief libc++-compiled production adapter for the SparkGameMMO chat wire libFuzzer harness.
 *
 * MMOChatSystem::HandleNetworkChat runs DecodeWirePayload on every client chat
 * datagram, and the server relays through BuildServerRelayPayload. The input is
 * split into a sender client id (4 bytes, little-endian), a connection-name
 * length byte and that many name bytes; the rest is the chat payload. A
 * violation of the wire contract aborts so libFuzzer records a crash:
 *  - an accepted payload names a channel no higher than Whisper and carries a
 *    non-empty sender and text,
 *  - EncodeWirePayload of an accepted payload reproduces the input bytes it
 *    consumed and decodes back to the same payload,
 *  - the relay refuses client id 0, malformed payloads and channels that
 *    IsNetworkRoutableChannel rejects; otherwise it keeps the channel and text
 *    and attributes the message to `<display>#<id>`, where <display> is the
 *    connection name's printable ASCII other than '#' (at most 32 bytes, "Player"
 *    when nothing but spaces is left), computed here independently,
 *  - the relayed sender never depends on the sender name inside the payload.
 */

#include "FuzzMMOChatWireProduction.h"

#include "Chat/MMOChatSystem.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024;

    using MMO::ChatChannel;
    using MMO::MMOChatSystem;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzMMOChatWire: chat wire contract violated: %s\n", what);
        std::abort();
    }

    bool SamePayload(const MMOChatSystem::WirePayload& a, const MMOChatSystem::WirePayload& b)
    {
        return a.channel == b.channel && a.senderName == b.senderName && a.text == b.text;
    }

    /// The server-attributed name, re-derived from the documented rule rather than the production helper.
    std::string ExpectedSender(std::string_view connectionName, std::uint32_t clientId)
    {
        std::string display;
        for (const char c : connectionName)
        {
            if (display.size() == MMOChatSystem::MAX_SENDER_DISPLAY_NAME)
                break;
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte <= 0x7E && c != '#')
                display.push_back(c);
        }
        if (display.find_first_not_of(' ') == std::string::npos)
            display = "Player";
        return display + "#" + std::to_string(clientId);
    }

    void CheckDecode(const std::vector<std::uint8_t>& payload)
    {
        const std::optional<MMOChatSystem::WirePayload> decoded = MMOChatSystem::DecodeWirePayload(payload);
        if (!decoded)
            return;
        if (static_cast<std::uint8_t>(decoded->channel) > static_cast<std::uint8_t>(ChatChannel::Whisper))
            InvariantFailure("an accepted payload names an unknown channel");
        if (decoded->senderName.empty() || decoded->text.empty())
            InvariantFailure("an accepted payload has an empty sender or text");

        const std::vector<std::uint8_t> encoded =
            MMOChatSystem::EncodeWirePayload(decoded->channel, decoded->senderName, decoded->text);
        if (encoded.size() > payload.size() || !std::equal(encoded.begin(), encoded.end(), payload.begin()))
            InvariantFailure("re-encoding an accepted payload does not reproduce the bytes it consumed");
        const std::optional<MMOChatSystem::WirePayload> reparsed = MMOChatSystem::DecodeWirePayload(encoded);
        if (!reparsed || !SamePayload(*reparsed, *decoded))
            InvariantFailure("an encoded payload does not decode back to itself");
    }

    void CheckRelay(const std::vector<std::uint8_t>& payload, std::string_view connectionName, std::uint32_t clientId)
    {
        const std::optional<std::vector<std::uint8_t>> relayed =
            MMOChatSystem::BuildServerRelayPayload(payload, connectionName, clientId);
        const std::optional<MMOChatSystem::WirePayload> decoded = MMOChatSystem::DecodeWirePayload(payload);
        const bool mustRefuse = clientId == 0 || !decoded || !MMOChatSystem::IsNetworkRoutableChannel(decoded->channel);
        if (mustRefuse)
        {
            if (relayed)
                InvariantFailure("the relay forwarded an unattributed, malformed or unroutable payload");
            return;
        }
        if (!relayed)
            InvariantFailure("the relay dropped a well-formed routable payload");

        const std::optional<MMOChatSystem::WirePayload> out = MMOChatSystem::DecodeWirePayload(*relayed);
        if (!out)
            InvariantFailure("the relayed payload does not decode");
        if (out->channel != decoded->channel || !MMOChatSystem::IsNetworkRoutableChannel(out->channel))
            InvariantFailure("the relay changed the channel or relayed an unroutable one");
        if (out->text != decoded->text)
            InvariantFailure("the relay changed the text");
        if (out->senderName != ExpectedSender(connectionName, clientId))
            InvariantFailure("the relayed sender is not the server-attributed <display>#<id>");
        if (out->senderName.find('#') != out->senderName.rfind('#'))
            InvariantFailure("the relayed sender carries a second '#'");

        // Swap the payload's own sender name: the relay output must not change.
        const std::string otherSender = decoded->senderName == "x" ? "y" : "x";
        const std::optional<std::vector<std::uint8_t>> swapped = MMOChatSystem::BuildServerRelayPayload(
            MMOChatSystem::EncodeWirePayload(decoded->channel, otherSender, decoded->text), connectionName, clientId);
        if (!swapped || *swapped != *relayed)
            InvariantFailure("the relayed sender depends on the client-supplied sender name");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzDecodeMMOChat(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    // The whole input is also a payload on its own, so every seed exercises the decoder directly.
    const std::vector<std::uint8_t> whole(data, data + size);
    CheckDecode(whole);

    if (size < 5)
        return 0;
    const std::uint32_t clientId = static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
                                   (static_cast<std::uint32_t>(data[2]) << 16) |
                                   (static_cast<std::uint32_t>(data[3]) << 24);
    const std::size_t nameLength = data[4] < size - 5 ? data[4] : size - 5;
    const std::string_view connectionName(reinterpret_cast<const char*>(data + 5), nameLength);
    const std::vector<std::uint8_t> payload(data + 5 + nameLength, data + size);
    CheckDecode(payload);
    CheckRelay(payload, connectionName, clientId);
    return 0;
}
