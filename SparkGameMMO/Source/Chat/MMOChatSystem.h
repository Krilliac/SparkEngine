/**
 * @file MMOChatSystem.h
 * @brief Multi-channel chat system for the MMO showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Demonstrates networked chat using the engine's reliable messaging channel:
 * - Area chat (visible only to players in the same area)
 * - Global chat (broadcast to all connected players via WorldServer)
 * - Party chat (visible to party members across areas)
 * - Whisper (direct player-to-player messages)
 *
 * Uses MessageType::ChatMessage on the ReliableOrdered channel for
 * guaranteed in-order delivery.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Windows.h defines SendMessage as SendMessageA/SendMessageW — undo that
#ifdef SendMessage
#undef SendMessage
#endif

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

      private:
        void SetupNetworkHandlers();
        static ChatChannel ParseChannelName(const std::string& name);
        static const char* ChannelToString(ChatChannel ch);

        Spark::IEngineContext* m_context{nullptr};
        std::deque<ChatMessage> m_history;
        float m_time{0.0f};
        bool m_initialized{false};

        static constexpr size_t MAX_HISTORY = 200;
    };

} // namespace MMO
