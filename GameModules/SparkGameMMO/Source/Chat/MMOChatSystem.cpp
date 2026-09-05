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
        netMgr->GetPacketValidator().RegisterSchema(kMMOChatMessageType,
                                                    {.minPayloadSize = 1,
                                                     .maxPayloadSize = 1024,
                                                     .requiresAuth = true,
                                                     .allowedFromClient = true,
                                                     .allowedFromServer = true,
                                                     .stringFieldOffset = 1});

        netMgr->RegisterHandler(kMMOChatMessageType,
                                [this, netMgr](const Spark::Net::NetworkMessage& netMsg)
                                {
                                    if (netMsg.payload.size() < 2)
                                        return;

                                    Spark::Net::NetBuffer buf;
                                    buf.WriteBytes(netMsg.payload.data(), netMsg.payload.size());
                                    const uint8_t channelValue = buf.ReadUint8();
                                    std::string senderName = buf.ReadString();
                                    std::string text = buf.ReadString();
                                    if (buf.HasError() || channelValue > static_cast<uint8_t>(ChatChannel::Whisper) ||
                                        senderName.empty() || text.empty())
                                    {
                                        return;
                                    }
                                    const auto channel = static_cast<ChatChannel>(channelValue);

                                    // Validate before relaying. Routing by party/area is owned by the
                                    // authoritative game service; this showcase relays accepted messages.
                                    if (netMgr->GetRole() == Spark::Net::NetworkRole::Server)
                                        netMgr->SendToAllExcept(netMsg.senderID, netMsg);

                                    ChatMessage msg{};
                                    msg.channel = channel;
                                    msg.senderClientId = netMsg.senderID;
                                    msg.senderName = senderName;
                                    msg.text = text;
                                    msg.timestamp = m_time;

                                    m_history.push_back(msg);
                                    if (m_history.size() > MAX_HISTORY)
                                        m_history.pop_front();

                                    auto& console = Spark::SimpleConsole::GetInstance();
                                    console.LogInfo("[" + std::string(ChannelToString(channel)) + "] " + senderName +
                                                    ": " + text);
                                });
#endif
    }

    void MMOChatSystem::SendMessage(const std::string& channelName, const std::string& text)
    {
        SendMessage(ParseChannelName(channelName), text);
    }

    void MMOChatSystem::SendMessage(ChatChannel channel, const std::string& text, uint32_t targetId)
    {
        if (!m_initialized || text.empty() ||
            static_cast<uint8_t>(channel) > static_cast<uint8_t>(ChatChannel::Whisper))
            return;

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
            Spark::Net::NetBuffer buf;
            buf.WriteUint8(static_cast<uint8_t>(channel));
            buf.WriteString(msg.senderName);
            buf.WriteString(text);

            Spark::Net::NetworkMessage netMsg;
            netMsg.type = kMMOChatMessageType;
            netMsg.channel = Spark::Net::ChannelType::ReliableOrdered;
            netMsg.payload = std::vector<uint8_t>(buf.GetData().begin(), buf.GetData().end());

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
            // NetworkManager currently has one handler slot per message type and
            // no unregister API. Replace the DLL-owned callback before unload so
            // hot reload cannot invoke a lambda whose code/data have been freed.
            netMgr->RegisterHandler(kMMOChatMessageType, [](const Spark::Net::NetworkMessage&) {});
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
