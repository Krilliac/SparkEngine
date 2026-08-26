/**
 * @file CollaborationProtocol.h
 * @brief Versioned DTOs for SparkDaemon's headless collaboration broker.
 */

#pragma once

#include "BoundedWireCodec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Daemon
{
    inline constexpr size_t kMaximumSessionIdLength = 64;
    inline constexpr size_t kMaximumPeerNameLength = 128;
    inline constexpr size_t kMaximumNodeIdLength = 256;
    inline constexpr size_t kMaximumPresenceLength = 512;
    inline constexpr size_t kMaximumEditPayloadLength = 64 * 1024;
    inline constexpr size_t kCollaborationTokenLength = 64;

    enum class CollaborationMessage : uint16_t
    {
        CreateSessionRequest = 0x0001,
        CreateSessionResponse = 0x0002,
        DeleteSessionRequest = 0x0003,
        DeleteSessionResponse = 0x0004,
        JoinSessionRequest = 0x0005,
        JoinSessionResponse = 0x0006,
        LeaveSessionRequest = 0x0007,
        LeaveSessionResponse = 0x0008,
        PresenceRequest = 0x0009,
        PresenceResponse = 0x000A,
        AcquireLockRequest = 0x000B,
        AcquireLockResponse = 0x000C,
        ReleaseLockRequest = 0x000D,
        ReleaseLockResponse = 0x000E,
        SubmitEditRequest = 0x000F,
        SubmitEditResponse = 0x0010,
        SnapshotRequest = 0x0011,
        SnapshotResponse = 0x0012,
    };

    struct CollaborationAuth
    {
        std::string sessionId;
        uint32_t peerId = 0;
        std::string token;
    };

    struct CollaborationPeer
    {
        uint32_t id = 0;
        std::string name;
        std::string presence;
    };

    struct CollaborationLock
    {
        std::string nodeId;
        uint32_t ownerPeerId = 0;
    };

    struct CollaborationEdit
    {
        uint64_t sequence = 0;
        uint32_t authorPeerId = 0;
        std::string nodeId;
        std::string payload;
    };

    struct CollaborationSnapshot
    {
        std::string sessionId;
        uint64_t nextSequence = 1;
        std::vector<CollaborationPeer> peers;
        std::vector<CollaborationLock> locks;
        std::vector<CollaborationEdit> edits;
    };

    inline bool WriteAuth(Wire::Writer& writer, const CollaborationAuth& auth)
    {
        return writer.WriteString(auth.sessionId, kMaximumSessionIdLength) &&
               (writer.Write<uint32_t>(auth.peerId), true) && writer.WriteString(auth.token, kCollaborationTokenLength);
    }

    inline bool ReadAuth(Wire::Reader& reader, CollaborationAuth& auth)
    {
        return reader.ReadString(auth.sessionId, kMaximumSessionIdLength) && reader.Read(auth.peerId) &&
               reader.ReadString(auth.token, kCollaborationTokenLength);
    }

    inline bool EncodeCollaborationAuth(const CollaborationAuth& auth, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!WriteAuth(writer, auth))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeCollaborationAuth(const std::vector<uint8_t>& bytes, CollaborationAuth& auth)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && ReadAuth(reader, auth) && reader.Finished();
    }

    inline bool EncodeSessionSecret(std::string_view sessionId, std::string_view secret, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!writer.WriteString(sessionId, kMaximumSessionIdLength) ||
            !writer.WriteString(secret, kCollaborationTokenLength))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeSessionSecret(const std::vector<uint8_t>& bytes, std::string& sessionId, std::string& secret)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && reader.ReadString(sessionId, kMaximumSessionIdLength) &&
               reader.ReadString(secret, kCollaborationTokenLength) && reader.Finished();
    }

    inline bool EncodeJoinRequest(std::string_view sessionId, std::string_view name, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!writer.WriteString(sessionId, kMaximumSessionIdLength) ||
            !writer.WriteString(name, kMaximumPeerNameLength))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeJoinRequest(const std::vector<uint8_t>& bytes, std::string& sessionId, std::string& name)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && reader.ReadString(sessionId, kMaximumSessionIdLength) &&
               reader.ReadString(name, kMaximumPeerNameLength) && reader.Finished();
    }

    inline bool EncodeJoinResponse(uint32_t peerId, std::string_view token, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        writer.Write(peerId);
        if (!writer.WriteString(token, kCollaborationTokenLength))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeJoinResponse(const std::vector<uint8_t>& bytes, uint32_t& peerId, std::string& token)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && reader.Read(peerId) &&
               reader.ReadString(token, kCollaborationTokenLength) && reader.Finished();
    }

    inline bool EncodeAuthString(const CollaborationAuth& auth, std::string_view value, size_t maximum,
                                 std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!WriteAuth(writer, auth) || !writer.WriteString(value, maximum))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeAuthString(const std::vector<uint8_t>& bytes, CollaborationAuth& auth, std::string& value,
                                 size_t maximum)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && ReadAuth(reader, auth) && reader.ReadString(value, maximum) &&
               reader.Finished();
    }

    inline bool EncodeEditRequest(const CollaborationAuth& auth, std::string_view nodeId, std::string_view edit,
                                  std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!WriteAuth(writer, auth) || !writer.WriteString(nodeId, kMaximumNodeIdLength) ||
            !writer.WriteString(edit, kMaximumEditPayloadLength))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeEditRequest(const std::vector<uint8_t>& bytes, CollaborationAuth& auth, std::string& nodeId,
                                  std::string& edit)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && ReadAuth(reader, auth) && reader.ReadString(nodeId, kMaximumNodeIdLength) &&
               reader.ReadString(edit, kMaximumEditPayloadLength) && reader.Finished();
    }

    inline bool EncodeBoolean(bool value, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        writer.Write<uint8_t>(value ? 1 : 0);
        out = writer.Take();
        return true;
    }

    inline bool EncodeSequence(uint64_t sequence, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        writer.Write(sequence);
        out = writer.Take();
        return true;
    }

    inline bool EncodeSnapshot(const CollaborationSnapshot& snapshot, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!writer.WriteString(snapshot.sessionId, kMaximumSessionIdLength))
            return false;
        writer.Write(snapshot.nextSequence);
        writer.Write<uint32_t>(static_cast<uint32_t>(snapshot.peers.size()));
        for (const auto& peer : snapshot.peers)
        {
            writer.Write(peer.id);
            if (!writer.WriteString(peer.name, kMaximumPeerNameLength) ||
                !writer.WriteString(peer.presence, kMaximumPresenceLength))
                return false;
        }
        writer.Write<uint32_t>(static_cast<uint32_t>(snapshot.locks.size()));
        for (const auto& lock : snapshot.locks)
        {
            if (!writer.WriteString(lock.nodeId, kMaximumNodeIdLength))
                return false;
            writer.Write(lock.ownerPeerId);
        }
        writer.Write<uint32_t>(static_cast<uint32_t>(snapshot.edits.size()));
        for (const auto& edit : snapshot.edits)
        {
            writer.Write(edit.sequence);
            writer.Write(edit.authorPeerId);
            if (!writer.WriteString(edit.nodeId, kMaximumNodeIdLength) ||
                !writer.WriteString(edit.payload, kMaximumEditPayloadLength))
                return false;
        }
        out = writer.Take();
        return true;
    }
} // namespace Spark::Daemon
