/**
 * @file CollaborativeWireCodec.cpp
 * @brief Bounded serializer and fail-closed decoder for editor collaboration frames.
 *
 * Thread affinity: callable from the editor and network threads; no shared mutable state.
 * Ownership: callers own input and output objects; successful decode publishes one complete value.
 * Allocation: accepted frames allocate only for bounded string fields; rejected frames publish nothing.
 */

#include "CollaborativeEditSession.h"

#include <cstring>
#include <limits>
#include <utility>

namespace SparkEditor
{
    namespace
    {
        void WriteU8(std::vector<uint8_t>& buffer, uint8_t value)
        {
            buffer.push_back(value);
        }

        void WriteU32(std::vector<uint8_t>& buffer, uint32_t value)
        {
            buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
            buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            buffer.push_back(static_cast<uint8_t>(value & 0xFF));
        }

        void WriteU64(std::vector<uint8_t>& buffer, uint64_t value)
        {
            WriteU32(buffer, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF));
            WriteU32(buffer, static_cast<uint32_t>(value & 0xFFFFFFFF));
        }

        void WriteFloat(std::vector<uint8_t>& buffer, float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            WriteU32(buffer, bits);
        }

        void WriteString(std::vector<uint8_t>& buffer, const std::string& value)
        {
            WriteU32(buffer, static_cast<uint32_t>(value.size()));
            buffer.insert(buffer.end(), value.begin(), value.end());
        }

        struct Reader
        {
            const uint8_t* data;
            size_t size;
            size_t pos = 0;
            bool failed = false;

            bool HasBytes(size_t count) const { return pos <= size && count <= size - pos; }

            uint8_t ReadU8()
            {
                if (!HasBytes(1))
                {
                    failed = true;
                    return 0;
                }
                return data[pos++];
            }

            uint32_t ReadU32()
            {
                if (!HasBytes(4))
                {
                    failed = true;
                    return 0;
                }
                const uint32_t value =
                    (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
                    (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
                pos += 4;
                return value;
            }

            uint64_t ReadU64()
            {
                const uint64_t high = ReadU32();
                const uint64_t low = ReadU32();
                return (high << 32) | low;
            }

            float ReadFloat()
            {
                const uint32_t bits = ReadU32();
                float value = 0.0f;
                if (!failed)
                    std::memcpy(&value, &bits, sizeof(value));
                return value;
            }

            std::string ReadString(size_t maximum = std::numeric_limits<size_t>::max())
            {
                const uint32_t length = ReadU32();
                if (failed || length > maximum || !HasBytes(length))
                {
                    failed = true;
                    return {};
                }
                std::string value(reinterpret_cast<const char*>(data + pos), length);
                pos += length;
                return value;
            }
        };

        void WriteEditMessage(std::vector<uint8_t>& buffer, const EditMessage& edit)
        {
            WriteU8(buffer, static_cast<uint8_t>(edit.type));
            WriteU32(buffer, edit.sourceEditor);
            WriteString(buffer, edit.nodeId);
            WriteString(buffer, edit.componentType);
            WriteString(buffer, edit.propertyName);
            WriteString(buffer, edit.newValue);
            WriteString(buffer, edit.oldValue);
            WriteU64(buffer, edit.timestamp);
        }

        EditMessage ReadEditMessage(Reader& reader)
        {
            EditMessage edit;
            if (reader.failed)
                return edit;
            edit.type = static_cast<EditMessageType>(reader.ReadU8());
            edit.sourceEditor = reader.ReadU32();
            edit.nodeId = reader.ReadString(kCollabMaxIdentifierBytes);
            edit.componentType = reader.ReadString(kCollabMaxIdentifierBytes);
            edit.propertyName = reader.ReadString(kCollabMaxIdentifierBytes);
            edit.newValue = reader.ReadString();
            edit.oldValue = reader.ReadString();
            edit.timestamp = reader.ReadU64();
            return edit;
        }

        void WriteEditorPeer(std::vector<uint8_t>& buffer, const EditorPeer& peer)
        {
            WriteU32(buffer, peer.id);
            WriteString(buffer, peer.userName);
            WriteString(buffer, peer.selectedNode);
            WriteFloat(buffer, peer.viewportCameraPos.x);
            WriteFloat(buffer, peer.viewportCameraPos.y);
            WriteFloat(buffer, peer.viewportCameraPos.z);
            WriteFloat(buffer, peer.viewportCameraDir.x);
            WriteFloat(buffer, peer.viewportCameraDir.y);
            WriteFloat(buffer, peer.viewportCameraDir.z);
            WriteFloat(buffer, peer.color.r);
            WriteFloat(buffer, peer.color.g);
            WriteFloat(buffer, peer.color.b);
            WriteFloat(buffer, peer.color.a);
        }

        EditorPeer ReadEditorPeer(Reader& reader)
        {
            EditorPeer peer;
            if (reader.failed)
                return peer;
            peer.id = reader.ReadU32();
            peer.userName = reader.ReadString(kCollabMaxIdentifierBytes);
            peer.selectedNode = reader.ReadString(kCollabMaxIdentifierBytes);
            peer.viewportCameraPos.x = reader.ReadFloat();
            peer.viewportCameraPos.y = reader.ReadFloat();
            peer.viewportCameraPos.z = reader.ReadFloat();
            peer.viewportCameraDir.x = reader.ReadFloat();
            peer.viewportCameraDir.y = reader.ReadFloat();
            peer.viewportCameraDir.z = reader.ReadFloat();
            peer.color.r = reader.ReadFloat();
            peer.color.g = reader.ReadFloat();
            peer.color.b = reader.ReadFloat();
            peer.color.a = reader.ReadFloat();
            peer.isActive = true;
            return peer;
        }
    } // namespace

    std::vector<uint8_t> SerializeMessage(const InternalMessage& message)
    {
        static const EditMessage defaultEditMessage{};
        std::vector<uint8_t> buffer;
        buffer.reserve(256);

        WriteU8(buffer, static_cast<uint8_t>(message.type));
        WriteU32(buffer, message.sourcePeer);
        WriteString(buffer, message.nodeId);
        WriteString(buffer, message.payload);
        WriteU64(buffer, message.timestamp);
        WriteEditMessage(buffer,
                         message.type == InternalMessageType::EditBroadcast ? message.editMessage : defaultEditMessage);
        WriteEditorPeer(buffer, message.peerInfo);
        return buffer;
    }

    bool DeserializeMessage(const uint8_t* data, size_t size, InternalMessage& outMessage)
    {
        if (!data || size == 0)
            return false;

        Reader reader{data, size, 0, false};
        if (!reader.HasBytes(1))
            return false;

        InternalMessage decoded;
        decoded.type = static_cast<InternalMessageType>(reader.ReadU8());
        decoded.sourcePeer = reader.ReadU32();
        decoded.nodeId = reader.ReadString(kCollabMaxIdentifierBytes);
        decoded.payload = reader.ReadString();
        decoded.timestamp = reader.ReadU64();
        decoded.editMessage = ReadEditMessage(reader);
        decoded.peerInfo = ReadEditorPeer(reader);

        if (reader.failed || reader.pos != reader.size)
            return false;
        if (static_cast<uint8_t>(decoded.type) > static_cast<uint8_t>(InternalMessageType::AuthAccepted) ||
            static_cast<uint8_t>(decoded.editMessage.type) > static_cast<uint8_t>(EditMessageType::ComponentModified))
            return false;

        outMessage = std::move(decoded);
        return true;
    }
} // namespace SparkEditor
