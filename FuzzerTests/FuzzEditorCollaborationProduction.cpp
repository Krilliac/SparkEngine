/**
 * @file FuzzEditorCollaborationProduction.cpp
 * @brief Production adapter for the editor collaboration wire libFuzzer harness.
 *
 * The adapter calls SparkEditor's real SerializeMessage and DeserializeMessage
 * entry points. Accepted frames must reproduce their input exactly when
 * serialized again. Rejected frames must leave a caller-owned output sentinel
 * unchanged, which detects partial publication on malformed input.
 */

#include "FuzzEditorCollaborationProduction.h"

#include "Communication/CollaborativeEditSession.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1048576;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorCollaboration: decoder violated: %s\n", what);
        std::abort();
    }

    bool IsSentinel(const SparkEditor::InternalMessage& message)
    {
        return message.type == SparkEditor::InternalMessageType::AuthAccepted && message.sourcePeer == 0xDEADBEEFu &&
               message.nodeId == "sentinel-node" && message.payload == "sentinel-payload" &&
               message.timestamp == 0x0123456789ABCDEFu && message.editMessage.nodeId == "sentinel-edit" &&
               message.peerInfo.userName == "sentinel-peer";
    }

    SparkEditor::InternalMessage MakeSentinel()
    {
        SparkEditor::InternalMessage message;
        message.type = SparkEditor::InternalMessageType::AuthAccepted;
        message.sourcePeer = 0xDEADBEEFu;
        message.nodeId = "sentinel-node";
        message.payload = "sentinel-payload";
        message.timestamp = 0x0123456789ABCDEFu;
        message.editMessage.nodeId = "sentinel-edit";
        message.peerInfo.userName = "sentinel-peer";
        return message;
    }

    void CheckFrame(const std::vector<std::uint8_t>& frame)
    {
        SparkEditor::InternalMessage decoded = MakeSentinel();
        if (!SparkEditor::DeserializeMessage(frame.data(), frame.size(), decoded))
        {
            if (!IsSentinel(decoded))
                InvariantFailure("a rejected frame modified the caller's output");
            return;
        }

        // SerializeMessage writes a default edit section for every type except EditBroadcast,
        // so only an EditBroadcast frame must reproduce its input exactly. Every accepted
        // frame must re-encode to a canonical frame that decodes and re-encodes to itself.
        const std::vector<std::uint8_t> reencoded = SparkEditor::SerializeMessage(decoded);
        if (decoded.type == SparkEditor::InternalMessageType::EditBroadcast && reencoded != frame)
            InvariantFailure("an accepted edit broadcast does not round-trip byte-for-byte");
        SparkEditor::InternalMessage canonical;
        if (!SparkEditor::DeserializeMessage(reencoded.data(), reencoded.size(), canonical) ||
            SparkEditor::SerializeMessage(canonical) != reencoded)
            InvariantFailure("the canonical re-encoding of an accepted frame is not a fixed point");
        if (reencoded.size() > frame.size())
            InvariantFailure("the canonical re-encoding is larger than the accepted frame");

        if (decoded.nodeId.size() > SparkEditor::kCollabMaxIdentifierBytes ||
            decoded.editMessage.nodeId.size() > SparkEditor::kCollabMaxIdentifierBytes ||
            decoded.peerInfo.userName.size() > SparkEditor::kCollabMaxIdentifierBytes)
            InvariantFailure("an accepted identifier exceeds the wire cap");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ driver and production
// sources compiled into the adapter target.
extern "C" int SparkFuzzDecodeEditorCollaboration(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > kMaxInputBytes || data == nullptr)
        return 0;

    CheckFrame(std::vector<std::uint8_t>(data, data + size));
    return 0;
}
