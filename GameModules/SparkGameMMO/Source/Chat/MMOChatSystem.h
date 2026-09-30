/**
 * @file MMOChatSystem.h
 * @brief Multi-channel chat system for the MMO showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Demonstrates networked chat using the engine's reliable messaging channel:
 * - Area chat (every client of this area server)
 * - Global chat (broadcast to all connected players)
 * - Party chat and Whisper: local history only. The server drops them and a
 *   networked client refuses to send them, because no recipient/party routing
 *   exists yet (see IsNetworkRoutableChannel).
 *
 * Uses a module-owned message type (UserDefined + 1) on the ReliableOrdered
 * channel. The server rebuilds every relayed payload with a sender name it
 * derives from the connection (`<sanitized display name>#<client id>`, see
 * ServerAttributedSenderName); a client-supplied name is never forwarded.
 *
 * Thread affinity: game thread, except the network handler. NetworkManager
 * dispatches handlers on whichever thread calls NetworkManager::Update (the
 * DedicatedServer tick thread inside SparkServer), so the handler only copies
 * the sender id and payload into a mutex-guarded inbox bounded at
 * MAX_PENDING_NETWORK messages (further datagrams are dropped). Update() drains
 * it on the game thread, where all decoding, relaying and history mutation
 * happen. Ownership: the network observer holds only a weak reference to the
 * inbox, never `this`; Shutdown unregisters it and discards anything still
 * queued.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Windows.h defines SendMessage as SendMessageA/SendMessageW — undo that
#ifdef SendMessage
#undef SendMessage
#endif

namespace Spark::Net
{
    struct NetworkMessage;
    enum class ChannelType;
} // namespace Spark::Net

namespace MMO
{

    /// @brief Chat channel types
    enum class ChatChannel : uint8_t
    {
        Area,   ///< Players in the same area
        Global, ///< All players on the server
        Party,  ///< Party members only
        Whisper ///< Direct message to one player
    };

    /// @brief A single chat message
    struct ChatMessage
    {
        ChatChannel channel = ChatChannel::Area;
        uint32_t senderClientId = 0;
        std::string senderName;
        std::string text;
        float timestamp = 0.0f;
        uint32_t targetAreaId = 0; ///< For area chat: which area
    };

    /**
     * @brief Multi-channel chat with network message routing
     *
     * Sends messages via NetworkManager::SendMessage on the ReliableOrdered
     * channel. On the server, the WorldServer routes global messages to all
     * AreaServers; area messages stay within the originating AreaServer.
     */
    class MMOChatSystem
    {
      public:
        MMOChatSystem() = default;
        ~MMOChatSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Send a message on the specified channel name ("area", "global", "party", "whisper")
        void SendMessage(const std::string& channelName, const std::string& text);

        /// Send a message on a typed channel
        void SendMessage(ChatChannel channel, const std::string& text, uint32_t targetId = 0);

        size_t GetChannelCount() const;
        const std::deque<ChatMessage>& GetHistory() const { return m_history; }

        /// Area and Global are routable over the network. Party and Whisper are not: the wire format
        /// carries no recipient or party id and this module keeps no server-side membership state, so the
        /// only way to deliver them would be broadcasting "private" text to every connected client.
        static constexpr bool IsNetworkRoutableChannel(ChatChannel channel)
        {
            return channel == ChatChannel::Area || channel == ChatChannel::Global;
        }

#ifdef ENABLE_NETWORKING
        /// Decoded module chat payload: `uint8 channel`, then NetBuffer strings sender and text.
        struct WirePayload
        {
            ChatChannel channel = ChatChannel::Area;
            std::string senderName;
            std::string text;
        };

        /// @return The payload, or nullopt when it is truncated, names an unknown channel or has an empty field.
        static std::optional<WirePayload> DecodeWirePayload(const std::vector<uint8_t>& payload);
        static std::vector<uint8_t> EncodeWirePayload(ChatChannel channel, const std::string& senderName,
                                                      const std::string& text);

        /// Longest display-name prefix kept in a server-attributed sender name.
        static constexpr size_t MAX_SENDER_DISPLAY_NAME = 32;

        /**
         * @brief The sender name the server attributes to a connection: `<display>#<clientId>`.
         *
         * The connection name is whatever the client asked for in its connect request; the engine does
         * not authenticate it or keep it unique, so it cannot be an identity on its own. The attributed
         * name keeps only printable ASCII other than '#' (at most MAX_SENDER_DISPLAY_NAME bytes, "Player"
         * when nothing is left) and always appends '#' and the server-assigned client id. The result
         * therefore always contains exactly one '#', followed by the connection's id: it can never equal
         * a reserved name such as "System" nor another connection's attributed name.
         */
        static std::string ServerAttributedSenderName(std::string_view connectionName, uint32_t clientId);

        /**
         * @brief Server relay policy for one client chat packet.
         *
         * The sender name is never taken from the client: the relayed payload is rebuilt with
         * ServerAttributedSenderName(@p connectionName, @p senderClientId).
         *
         * @return The payload to fan out to the other clients, or nullopt to drop the packet (malformed,
         *         a channel that IsNetworkRoutableChannel rejects, or an invalid sender id).
         */
        static std::optional<std::vector<uint8_t>> BuildServerRelayPayload(const std::vector<uint8_t>& clientPayload,
                                                                           std::string_view connectionName,
                                                                           uint32_t senderClientId);
#endif

      private:
        void SetupNetworkHandlers();
#ifdef ENABLE_NETWORKING
        /// A received chat datagram awaiting game-thread processing.
        struct PendingNetworkChat
        {
            uint32_t senderId = 0;
            Spark::Net::ChannelType channel{};
            std::vector<uint8_t> payload;
        };

        /// Lifetime-owned inbox state used by the network callback. Keeping this separate from the
        /// system lets a callback copied by NetworkManager finish without dereferencing a torn-down
        /// MMOChatSystem instance.
        struct PendingNetworkInbox
        {
            std::mutex mutex;
            std::deque<PendingNetworkChat> messages;
        };

        /// Any thread: copy the datagram into the bounded inbox.
        static void EnqueueNetworkChat(PendingNetworkInbox& inbox, const Spark::Net::NetworkMessage& netMsg);
        /// Game thread: process every queued datagram in arrival order.
        void DrainNetworkChat();
        /// Receive path for the module chat type: server validates, re-attributes and relays; client records.
        void HandleNetworkChat(Spark::Net::NetworkManager& netMgr, const PendingNetworkChat& received);
#endif
        static ChatChannel ParseChannelName(const std::string& name);
        static const char* ChannelToString(ChatChannel ch);

        Spark::IEngineContext* m_context{nullptr};
        std::deque<ChatMessage> m_history;
        float m_time{0.0f};
        bool m_initialized{false};
#ifdef ENABLE_NETWORKING
        std::shared_ptr<PendingNetworkInbox> m_pendingInbox;
#endif
        static constexpr size_t MAX_PENDING_NETWORK = 256;

        static constexpr size_t MAX_HISTORY = 200;
    };

} // namespace MMO
