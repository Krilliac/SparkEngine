/**
 * @file MMOChatSystem.cpp
 * @brief Multi-channel chat with network message routing
 */

#include "MMOChatSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

// Windows.h defines SendMessage as SendMessageA/SendMessageW — undo that
#ifdef SendMessage
#undef SendMessage
#endif

namespace MMO
{
#ifdef ENABLE_NETWORKING
    namespace
    {
        /// This module's chat payload is `uint8 channel` followed by two NetBuffer
        /// strings (sender, text). The engine's built-in MessageType::ChatMessage
        /// declares a single string at offset 0, so the module used to re-register
        /// the BUILT-IN type's schema with stringFieldOffset = 1 — a process-wide
        /// change to a shared type that was never restored on Shutdown, so every
        /// other user of ChatMessage (and the engine itself after this module
        /// unloaded) kept validating against the MMO layout. A module-owned layout
        /// gets a module-owned message type in the UserDefined range instead.
        constexpr Spark::Net::MessageType kMMOChatMessageType =
            static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(Spark::Net::MessageType::UserDefined) + 1u);
    } // namespace
#endif

    bool MMOChatSystem::Initialize(Spark::IEngineContext* context)
    {
        if (m_initialized || m_context)
            Shutdown();

        m_context = context;
        m_time = 0.0f;
        m_history.clear();

        SetupNetworkHandlers();

        // Post a welcome message
        ChatMessage welcome{};
        welcome.channel = ChatChannel::Global;
        welcome.senderName = "System";
        welcome.text = "Welcome to SparkMMO! Use /area, /global, /party, or /whisper to chat.";
        welcome.timestamp = 0.0f;
        m_history.push_back(welcome);

        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO chat system initialized (4 channels)");
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO Chat] Chat system initialized (4 channels)");
        return true;
    }

    void MMOChatSystem::SetupNetworkHandlers()
    {
#ifdef ENABLE_NETWORKING
        // Resolve networking through the injected engine context, not the global
        // singleton — the module is handed its NetworkManager via Initialize(context).
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (!netMgr)
            return;

        // Register the module's OWN type, leaving the engine's built-in
        // ChatMessage schema untouched (see kMMOChatMessageType above).
        netMgr->GetPacketValidator().RegisterSchema(kMMOChatMessageType, {.minPayloadSize = 1,
                                                                          .maxPayloadSize = 1024,
                                                                          .requiresAuth = true,
                                                                          .allowedFromClient = true,
                                                                          .allowedFromServer = true,
                                                                          .stringFieldOffset = 1});

        netMgr->RegisterHandler(kMMOChatMessageType, [this, netMgr](const Spark::Net::NetworkMessage& netMsg)
                                { HandleNetworkChat(*netMgr, netMsg); });
#endif
    }

#ifdef ENABLE_NETWORKING
    void MMOChatSystem::HandleNetworkChat(Spark::Net::NetworkManager& netMgr, const Spark::Net::NetworkMessage& netMsg)
    {
        std::optional<WirePayload> decoded = DecodeWirePayload(netMsg.payload);
        if (!decoded)
        {
            return;
        }

        if (netMgr.GetRole() == Spark::Net::NetworkRole::Server)
        {
            // Identity comes from the connection, never from the payload. The connection name itself is
            // client-chosen and unauthenticated, so the relayed name always carries the server-assigned id.
            std::string connectionName;
            {
                const auto clients = netMgr.GetClients();
                const auto client = clients.find(netMsg.senderID);
                if (client == clients.end())
                {
                    return;
                }
                connectionName = client->second.name;
            }
            std::optional<std::vector<uint8_t>> relayPayload =
                BuildServerRelayPayload(netMsg.payload, connectionName, netMsg.senderID);
            if (!relayPayload)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "MMO chat: dropped %s message from client %u (no server-side routing for it)",
                               ChannelToString(decoded->channel), static_cast<unsigned>(netMsg.senderID));
                return;
            }
            Spark::Net::NetworkMessage relay = netMsg;
            relay.payload = std::move(*relayPayload);
            netMgr.SendToAllExcept(netMsg.senderID, relay);
            decoded->senderName = ServerAttributedSenderName(connectionName, netMsg.senderID);
        }

        ChatMessage msg{};
        msg.channel = decoded->channel;
        msg.senderClientId = netMsg.senderID;
        msg.senderName = decoded->senderName;
        msg.text = decoded->text;
        msg.timestamp = m_time;

        m_history.push_back(msg);
        if (m_history.size() > MAX_HISTORY)
            m_history.pop_front();

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[" + std::string(ChannelToString(msg.channel)) + "] " + msg.senderName + ": " + msg.text);
    }
#endif

    void MMOChatSystem::SendMessage(const std::string& channelName, const std::string& text)
    {
        SendMessage(ParseChannelName(channelName), text);
    }

    void MMOChatSystem::SendMessage(ChatChannel channel, const std::string& text, uint32_t targetId)
    {
        if (!m_initialized || text.empty() ||
            static_cast<uint8_t>(channel) > static_cast<uint8_t>(ChatChannel::Whisper))
            return;

#ifdef ENABLE_NETWORKING
        // Refuse to put a "private" message on the wire when nothing can route it privately.
        if (auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
            netMgr && netMgr->GetRole() != Spark::Net::NetworkRole::None && !IsNetworkRoutableChannel(channel))
        {
            Spark::SimpleConsole::GetInstance().LogWarning(
                "[MMO Chat] " + std::string(ChannelToString(channel)) +
                " chat is not available in networked sessions yet; message not sent");
            return;
        }
#endif

        ChatMessage msg{};
        msg.channel = channel;
        msg.senderClientId = 1; // Local client
        msg.senderName = "Player";
        msg.text = text;
        msg.timestamp = m_time;
        msg.targetAreaId = targetId;

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Chat message sent on channel %d", static_cast<int>(channel));
        m_history.push_back(msg);
        if (m_history.size() > MAX_HISTORY)
            m_history.pop_front();

#ifdef ENABLE_NETWORKING
        // Send over the network on the reliable ordered channel, using the
        // NetworkManager provided by the injected engine context.
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (netMgr && netMgr->GetRole() != Spark::Net::NetworkRole::None)
        {
            Spark::Net::NetworkMessage netMsg;
            netMsg.type = kMMOChatMessageType;
            netMsg.channel = Spark::Net::ChannelType::ReliableOrdered;
            netMsg.payload = EncodeWirePayload(channel, msg.senderName, text);

            if (channel == ChatChannel::Global)
            {
                netMgr->BroadcastMessage(netMsg);
            }
            else
            {
                netMgr->SendMessage(netMsg);
            }
        }
#endif

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[" + std::string(ChannelToString(channel)) + "] " + msg.senderName + ": " + text);
    }

    void MMOChatSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        if (deltaTime > 0.0f)
            m_time += deltaTime;
    }

    void MMOChatSystem::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (auto* netMgr = m_context ? m_context->GetNetwork() : nullptr)
        {
            // Remove (never replace) the chat observer: an empty replacement lambda is itself
            // code in this image, and during hot reload it overwrote the replacement module's
            // handler. Inside the module's teardown scope NetworkManager leaves a slot the
            // replacement already owns untouched.
            netMgr->UnregisterHandler(kMMOChatMessageType);
        }
#endif
        m_history.clear();
        m_time = 0.0f;
        m_context = nullptr;
        m_initialized = false;
    }

    size_t MMOChatSystem::GetChannelCount() const
    {
        return 4; // Area, Global, Party, Whisper
    }

    void MMOChatSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("MMO Chat"))
            return;

        ImGui::Text("Messages: %zu / %zu", m_history.size(), MAX_HISTORY);
        ImGui::Text("Channels: Area, Global, Party, Whisper");
        ImGui::Separator();

        // Chat history scroll area
        ImGui::BeginChild("ChatHistory", ImVec2(0, 200), true);
        for (const auto& msg : m_history)
        {
            const char* channelTag = ChannelToString(msg.channel);

            // Color-code by channel
            ImVec4 color;
            switch (msg.channel)
            {
            case ChatChannel::Area:
                color = ImVec4(0.8f, 0.8f, 0.4f, 1.0f);
                break;
            case ChatChannel::Global:
                color = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
                break;
            case ChatChannel::Party:
                color = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
                break;
            case ChatChannel::Whisper:
                color = ImVec4(0.9f, 0.4f, 0.9f, 1.0f);
                break;
            }

            ImGui::TextColored(color, "[%s] %s: %s", channelTag, msg.senderName.c_str(), msg.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        // Chat input
        static char chatInput[256] = "";
        static int channelIdx = 1; // Default to Global
        const char* channels[] = {"Area", "Global", "Party", "Whisper"};
        ImGui::Combo("Channel", &channelIdx, channels, 4);
        ImGui::SameLine();
        if (ImGui::InputText("##chatinput", chatInput, sizeof(chatInput), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (chatInput[0] != '\0')
            {
                SendMessage(static_cast<ChatChannel>(channelIdx), chatInput);
                chatInput[0] = '\0';
            }
        }
#endif
    }

    ChatChannel MMOChatSystem::ParseChannelName(const std::string& name)
    {
        if (name == "area" || name == "a")
            return ChatChannel::Area;
        if (name == "global" || name == "g")
            return ChatChannel::Global;
        if (name == "party" || name == "p")
            return ChatChannel::Party;
        if (name == "whisper" || name == "w")
            return ChatChannel::Whisper;
        return ChatChannel::Area; // Default
    }

    const char* MMOChatSystem::ChannelToString(ChatChannel ch)
    {
        switch (ch)
        {
        case ChatChannel::Area:
            return "Area";
        case ChatChannel::Global:
            return "Global";
        case ChatChannel::Party:
            return "Party";
        case ChatChannel::Whisper:
            return "Whisper";
        }
        return "Unknown";
    }

} // namespace MMO
