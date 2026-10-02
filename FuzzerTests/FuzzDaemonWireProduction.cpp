/**
 * @file FuzzDaemonWireProduction.cpp
 * @brief libc++-compiled production adapter for the SparkDaemon bounded wire libFuzzer harness.
 *
 * Input byte 0 selects one of the shipped header-only decoders in
 * SparkDaemon/src/CollaborationProtocol.h and OrchestrationProtocol.h (the
 * collaboration broker, the orchestration service and their clients decode
 * peer messages with them over Wire::Reader); the remaining bytes are the
 * message payload. A violated contract aborts so libFuzzer records a crash:
 *  - every decoder insists on consuming the whole payload, so an accepted
 *    message re-encodes through its Encode* counterpart to exactly the input,
 *  - every decoded string stays within the cap its decoder passed,
 *  - a decoded record list never holds (or reserves) more records than the
 *    payload could encode at the smallest record size, plus one,
 *  - the publish-on-success decoders (DecodeSnapshot, DecodeBoolean) leave a
 *    sentinel output untouched on rejection.
 * The smoke test's -malloc_limit_mb also fails any single allocation a
 * rejected count overclaim would have reserved.
 */

#include "FuzzDaemonWireProduction.h"

#include "CollaborationProtocol.h"
#include "OrchestrationProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    namespace Daemon = Spark::Daemon;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::size_t kStatusListMaximum = 1024; // OrchestratorMain's status-list cap

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzDaemonWire: decoder violated: %s\n", what);
        std::abort();
    }

    void RequireExactReencode(bool encoded, const std::vector<std::uint8_t>& reencoded,
                              const std::vector<std::uint8_t>& payload)
    {
        if (!encoded)
            InvariantFailure("an accepted message does not encode again");
        if (reencoded != payload)
            InvariantFailure("re-encoding differs from the accepted payload");
    }

    void RequireWithin(const std::string& value, std::size_t maximum)
    {
        if (value.size() > maximum)
            InvariantFailure("a decoded string exceeds its cap");
    }

    /// Bytes one more record adds to an encoding: the size difference between
    /// encodings with one and zero minimal records, measured with the shipped
    /// encoders so the bound is not restated by hand.
    template <typename Encode> std::size_t RecordBytes(Encode&& encode)
    {
        std::vector<std::uint8_t> none;
        std::vector<std::uint8_t> one;
        if (!encode(0, none) || !encode(1, one) || one.size() <= none.size())
            InvariantFailure("cannot measure the minimal record size");
        return one.size() - none.size();
    }

    template <typename Container>
    void RequireRecordsFit(const Container& records, std::size_t payloadBytes, std::size_t recordBytes)
    {
        const std::size_t limit = payloadBytes / recordBytes + 1;
        if (records.size() > limit || records.capacity() > limit)
            InvariantFailure("a record list is larger than its payload can encode");
    }

    void RequireAuthWithinCaps(const Daemon::CollaborationAuth& auth)
    {
        RequireWithin(auth.sessionId, Daemon::kMaximumSessionIdLength);
        RequireWithin(auth.token, Daemon::kCollaborationTokenLength);
    }

    void FuzzAuth(const std::vector<std::uint8_t>& payload)
    {
        Daemon::CollaborationAuth auth;
        if (!Daemon::DecodeCollaborationAuth(payload, auth))
            return;
        RequireAuthWithinCaps(auth);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeCollaborationAuth(auth, reencoded), reencoded, payload);
    }

    void FuzzSessionSecret(const std::vector<std::uint8_t>& payload)
    {
        std::string sessionId;
        std::string secret;
        if (!Daemon::DecodeSessionSecret(payload, sessionId, secret))
            return;
        RequireWithin(sessionId, Daemon::kMaximumSessionIdLength);
        RequireWithin(secret, Daemon::kCollaborationTokenLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeSessionSecret(sessionId, secret, reencoded), reencoded, payload);
    }

    void FuzzJoinRequest(const std::vector<std::uint8_t>& payload)
    {
        std::string sessionId;
        std::string name;
        if (!Daemon::DecodeJoinRequest(payload, sessionId, name))
            return;
        RequireWithin(sessionId, Daemon::kMaximumSessionIdLength);
        RequireWithin(name, Daemon::kMaximumPeerNameLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeJoinRequest(sessionId, name, reencoded), reencoded, payload);
    }

    void FuzzJoinResponse(const std::vector<std::uint8_t>& payload)
    {
        std::uint32_t peerId = 0;
        std::string token;
        if (!Daemon::DecodeJoinResponse(payload, peerId, token))
            return;
        RequireWithin(token, Daemon::kCollaborationTokenLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeJoinResponse(peerId, token, reencoded), reencoded, payload);
    }

    void FuzzAuthString(const std::vector<std::uint8_t>& payload, std::size_t maximum)
    {
        Daemon::CollaborationAuth auth;
        std::string value;
        if (!Daemon::DecodeAuthString(payload, auth, value, maximum))
            return;
        RequireAuthWithinCaps(auth);
        RequireWithin(value, maximum);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeAuthString(auth, value, maximum, reencoded), reencoded, payload);
    }

    void FuzzEditRequest(const std::vector<std::uint8_t>& payload)
    {
        Daemon::CollaborationAuth auth;
        std::string nodeId;
        std::string edit;
        if (!Daemon::DecodeEditRequest(payload, auth, nodeId, edit))
            return;
        RequireAuthWithinCaps(auth);
        RequireWithin(nodeId, Daemon::kMaximumNodeIdLength);
        RequireWithin(edit, Daemon::kMaximumEditPayloadLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeEditRequest(auth, nodeId, edit, reencoded), reencoded, payload);
    }

    void FuzzBoolean(const std::vector<std::uint8_t>& payload)
    {
        // Two sentinels, so a rejection that writes either value is caught.
        for (const bool sentinel : {false, true})
        {
            bool value = sentinel;
            if (!Daemon::DecodeBoolean(payload, value))
            {
                if (value != sentinel)
                    InvariantFailure("a rejected boolean modified the caller's output");
                continue;
            }
            std::vector<std::uint8_t> reencoded;
            RequireExactReencode(Daemon::EncodeBoolean(value, reencoded), reencoded, payload);
        }
    }

    void FuzzSequence(const std::vector<std::uint8_t>& payload)
    {
        std::uint64_t sequence = 0;
        if (!Daemon::DecodeSequence(payload, sequence))
            return;
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeSequence(sequence, reencoded), reencoded, payload);
    }

    void FuzzSnapshot(const std::vector<std::uint8_t>& payload)
    {
        static const std::size_t peerBytes = RecordBytes(
            [](std::size_t count, std::vector<std::uint8_t>& out)
            {
                Daemon::CollaborationSnapshot snapshot;
                snapshot.peers.resize(count);
                return Daemon::EncodeSnapshot(snapshot, out);
            });
        static const std::size_t lockBytes = RecordBytes(
            [](std::size_t count, std::vector<std::uint8_t>& out)
            {
                Daemon::CollaborationSnapshot snapshot;
                snapshot.locks.resize(count);
                return Daemon::EncodeSnapshot(snapshot, out);
            });
        static const std::size_t editBytes = RecordBytes(
            [](std::size_t count, std::vector<std::uint8_t>& out)
            {
                Daemon::CollaborationSnapshot snapshot;
                snapshot.edits.resize(count);
                return Daemon::EncodeSnapshot(snapshot, out);
            });

        Daemon::CollaborationSnapshot snapshot;
        snapshot.sessionId = "sentinel";
        snapshot.nextSequence = 0xDEADBEEFull;
        snapshot.peers.resize(1);
        if (!Daemon::DecodeSnapshot(payload, snapshot))
        {
            if (snapshot.sessionId != "sentinel" || snapshot.nextSequence != 0xDEADBEEFull ||
                snapshot.peers.size() != 1 || !snapshot.locks.empty() || !snapshot.edits.empty())
                InvariantFailure("a rejected snapshot modified the caller's output");
            return;
        }
        RequireWithin(snapshot.sessionId, Daemon::kMaximumSessionIdLength);
        RequireRecordsFit(snapshot.peers, payload.size(), peerBytes);
        RequireRecordsFit(snapshot.locks, payload.size(), lockBytes);
        RequireRecordsFit(snapshot.edits, payload.size(), editBytes);
        for (const auto& peer : snapshot.peers)
        {
            RequireWithin(peer.name, Daemon::kMaximumPeerNameLength);
            RequireWithin(peer.presence, Daemon::kMaximumPresenceLength);
        }
        for (const auto& lock : snapshot.locks)
            RequireWithin(lock.nodeId, Daemon::kMaximumNodeIdLength);
        for (const auto& edit : snapshot.edits)
        {
            RequireWithin(edit.nodeId, Daemon::kMaximumNodeIdLength);
            RequireWithin(edit.payload, Daemon::kMaximumEditPayloadLength);
        }
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeSnapshot(snapshot, reencoded), reencoded, payload);
    }

    void FuzzProcessMutation(const std::vector<std::uint8_t>& payload)
    {
        Daemon::MutationKey key;
        std::string id;
        if (!Daemon::DecodeProcessMutation(payload, key, id))
            return;
        RequireWithin(key.clientInstance, Daemon::kMaximumClientInstanceLength);
        RequireWithin(id, Daemon::kMaximumProcessIdLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeProcessMutation(key, id, reencoded), reencoded, payload);
    }

    void FuzzProcessId(const std::vector<std::uint8_t>& payload)
    {
        std::string id;
        if (!Daemon::DecodeProcessId(payload, id))
            return;
        RequireWithin(id, Daemon::kMaximumProcessIdLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeProcessId(id, reencoded), reencoded, payload);
    }

    void FuzzProcessDefinition(const std::vector<std::uint8_t>& payload)
    {
        static const std::size_t argumentBytes = RecordBytes(
            [](std::size_t count, std::vector<std::uint8_t>& out)
            {
                Daemon::ProcessDefinition definition;
                definition.arguments.resize(count);
                return Daemon::EncodeProcessDefinition(Daemon::MutationKey{"c", 1}, definition, out);
            });

        Daemon::MutationKey key;
        Daemon::ProcessDefinition definition;
        if (!Daemon::DecodeProcessDefinition(payload, key, definition))
            return;
        RequireWithin(key.clientInstance, Daemon::kMaximumClientInstanceLength);
        RequireWithin(definition.id, Daemon::kMaximumProcessIdLength);
        RequireWithin(definition.executable, Daemon::kMaximumProcessPathLength);
        RequireWithin(definition.workingDirectory, Daemon::kMaximumProcessPathLength);
        if (definition.arguments.size() > Daemon::kMaximumProcessArguments)
            InvariantFailure("a process definition exceeds its argument cap");
        RequireRecordsFit(definition.arguments, payload.size(), argumentBytes);
        for (const auto& argument : definition.arguments)
            RequireWithin(argument, Daemon::kMaximumProcessArgumentLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeProcessDefinition(key, definition, reencoded), reencoded, payload);
    }

    void FuzzProcessStatuses(const std::vector<std::uint8_t>& payload)
    {
        static const std::size_t statusBytes =
            RecordBytes([](std::size_t count, std::vector<std::uint8_t>& out)
                        { return Daemon::EncodeProcessStatuses(std::vector<Daemon::ProcessStatus>(count), out); });

        std::vector<Daemon::ProcessStatus> statuses;
        if (!Daemon::DecodeProcessStatuses(payload, statuses, kStatusListMaximum))
            return;
        if (statuses.size() > kStatusListMaximum)
            InvariantFailure("a status list exceeds its cap");
        RequireRecordsFit(statuses, payload.size(), statusBytes);
        for (const auto& status : statuses)
            RequireWithin(status.id, Daemon::kMaximumProcessIdLength);
        std::vector<std::uint8_t> reencoded;
        RequireExactReencode(Daemon::EncodeProcessStatuses(statuses, reencoded), reencoded, payload);
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoders.
extern "C" int SparkFuzzDecodeDaemonWireMessage(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr || size == 0)
        return 0;

    const std::vector<std::uint8_t> payload(data + 1, data + size);
    switch (data[0] % 14u)
    {
    case 0:
        FuzzAuth(payload);
        break;
    case 1:
        FuzzSessionSecret(payload);
        break;
    case 2:
        FuzzJoinRequest(payload);
        break;
    case 3:
        FuzzJoinResponse(payload);
        break;
    case 4:
        FuzzAuthString(payload, Daemon::kMaximumNodeIdLength); // lock acquire/release
        break;
    case 5:
        FuzzAuthString(payload, Daemon::kMaximumPresenceLength); // presence update
        break;
    case 6:
        FuzzEditRequest(payload);
        break;
    case 7:
        FuzzBoolean(payload);
        break;
    case 8:
        FuzzSequence(payload);
        break;
    case 9:
        FuzzSnapshot(payload);
        break;
    case 10:
        FuzzProcessMutation(payload);
        break;
    case 11:
        FuzzProcessId(payload);
        break;
    case 12:
        FuzzProcessDefinition(payload);
        break;
    default:
        FuzzProcessStatuses(payload);
        break;
    }
    return 0;
}
