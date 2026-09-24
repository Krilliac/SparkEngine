/**
 * @file TestSEC100ChatAuditLogReal.cpp
 * @brief SEC-100: remote chat must not be able to forge server audit records.
 *
 * DedicatedServer writes both remote chat and the administration audit trail
 * ("RCON: command=<name> disposition=...") to the same server log, one record per
 * line. PacketValidator deliberately admits '\n' and '\r' in chat text, and chat
 * was logged verbatim, so any admitted client could end its own record and write a
 * line indistinguishable from a genuine admin audit record. These tests drive the
 * shipped DedicatedServer (only the network runtime is substituted) and read the
 * real log file it writes.
 */

#include "TestFramework.h"

#include "Engine/Networking/DedicatedServer.h"

#ifdef ENABLE_NETWORKING

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Spark::Net;

namespace
{
    class Sec100ChatRuntime final : public INetworkRuntime
    {
      public:
        bool Initialize() override { return true; }
        bool StartServer(uint16_t, int, const NetworkEndpointPolicy&, bool) override { return true; }
        void StopServer() override {}
        void Shutdown() override {}
        void Update(float) override {}
        void SendToClient(ClientID, const NetworkMessage&) override {}
        void SendToAll(const NetworkMessage&) override {}
        void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg) override
        {
            relayed.emplace_back(excludeClient, msg);
        }
        void RegisterHandler(MessageType type, MessageHandler handler) override
        {
            handlers[static_cast<uint16_t>(type)] = std::move(handler);
        }
        void ClearHandlers() override { handlers.clear(); }
        std::unordered_map<ClientID, ClientInfo> GetClients() const override { return {}; }
        NetworkStats GetStats() const override { return NetworkStats{}; }
        void KickClient(ClientID, const std::string&) override {}

        bool Dispatch(const NetworkMessage& msg)
        {
            auto it = handlers.find(static_cast<uint16_t>(msg.type));
            if (it == handlers.end())
                return false;
            it->second(msg);
            return true;
        }

        std::unordered_map<uint16_t, MessageHandler> handlers;
        std::vector<std::pair<ClientID, NetworkMessage>> relayed;
    };

    NetworkMessage BuildChat(ClientID sender, const std::string& text)
    {
        NetworkMessage msg;
        msg.type = MessageType::ChatMessage;
        msg.senderID = sender;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(text);
        msg.payload = buf.GetData();
        return msg;
    }

    std::vector<std::string> ReadLines(const std::filesystem::path& path)
    {
        std::vector<std::string> lines;
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line))
        {
            // The server log is a text-mode stream, so Windows terminates each record
            // with CRLF. Strip only that terminator; any other CR is still a forgery.
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
        return lines;
    }

    /// A record is "[HH:MM:SS] <body>". Returns true when the body of this line is
    /// an administration audit record.
    bool IsAuditRecordLine(const std::string& line)
    {
        const std::size_t close = line.find("] ");
        return close != std::string::npos && line.compare(close + 2, 5, "RCON:") == 0;
    }

    bool IsPrintableAscii(const std::string& text)
    {
        for (const char ch : text)
        {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte > 0x7E)
                return false;
        }
        return true;
    }
} // namespace

TEST(SEC100_ChatCannotForgeAdminAuditRecordInServerLog)
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path logPath =
        std::filesystem::temp_directory_path() / ("sec100_chat_audit_" + std::to_string(unique) + ".log");
    std::error_code ec;
    std::filesystem::remove(logPath, ec);

    // A normal, admitted client forges a following audit line. Both terminators
    // pass PacketValidator's ChatMessage string screen.
    const std::string forgedLf = "gg\n[12:00:00] RCON: command=ban disposition=dispatched";
    const std::string forgedCrLf = "gg\r\n[12:00:01] RCON: command=kick disposition=dispatched";

    Sec100ChatRuntime runtime;
    {
        DedicatedServer server(runtime);
        ServerConfig config;
        config.enableLanBroadcast = false;
        config.enableLogging = true;
        config.logFilePath = logPath.string();
        config.mapRotation = {"arena"};
        ASSERT_TRUE(server.InitializeOnly(config));

        ASSERT_TRUE(runtime.Dispatch(BuildChat(12, forgedLf)));
        ASSERT_TRUE(runtime.Dispatch(BuildChat(12, forgedCrLf)));

        // One genuine audit record for contrast: the detector must see it.
        server.RegisterRconCommand("sec100_probe", "SEC-100 probe",
                                   [](const std::vector<std::string>&) { return std::string("ok"); });
        EXPECT_EQ(server.ExecuteRcon("sec100_probe"), std::string("ok"));
        server.Stop();
    }

    const std::vector<std::string> lines = ReadLines(logPath);
    std::filesystem::remove(logPath, ec);
    ASSERT_TRUE(!lines.empty());

    int auditLines = 0;
    int chatLines = 0;
    for (const std::string& line : lines)
    {
        // Every physical line is a whole, timestamped record.
        EXPECT_TRUE(!line.empty() && line.front() == '[');
        EXPECT_TRUE(line.find('\r') == std::string::npos);
        if (IsAuditRecordLine(line))
            ++auditLines;
        if (line.find("] Chat: client=12 ") != std::string::npos)
            ++chatLines;
    }

    // Only the genuine dispatch produced an audit record; neither forgery did.
    EXPECT_EQ(auditLines, 1);
    EXPECT_EQ(chatLines, 2);
    bool sawGenuine = false;
    for (const std::string& line : lines)
    {
        if (IsAuditRecordLine(line))
            sawGenuine = line.find("RCON: command=sec100_probe disposition=dispatched") != std::string::npos;
    }
    EXPECT_TRUE(sawGenuine);
}

TEST(SEC100_ChatAuditFieldIsPrintableBoundedAndUnambiguous)
{
    std::vector<std::string> logMessages;
    ServerCallbacks callbacks;
    callbacks.onLogMessage = [&](const std::string& message) { logMessages.push_back(message); };

    Sec100ChatRuntime runtime;
    DedicatedServer server(runtime);
    ServerConfig config;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    config.mapRotation = {"arena"};
    ASSERT_TRUE(server.InitializeOnly(config));
    server.SetCallbacks(callbacks);

    const auto chatRecordFor = [&](const std::string& text) -> std::string
    {
        logMessages.clear();
        runtime.Dispatch(BuildChat(7, text));
        for (const std::string& message : logMessages)
        {
            if (message.find("Chat: client=7 ") != std::string::npos)
                return message;
        }
        return {};
    };

    // Terminators and the field delimiters are escaped, so the quoted field
    // cannot be closed early and the encoding stays injective.
    const std::string delimiters = chatRecordFor("a\"b\\c\nd\re\tf");
    EXPECT_STR_CONTAINS(delimiters, "Chat: client=7 bytes=11 text=\"a\\\"b\\\\c\\nd\\re\\tf\"");
    EXPECT_TRUE(IsPrintableAscii(delimiters));

    // Non-ASCII bytes (including U+2028 LINE SEPARATOR, which several log viewers
    // treat as a line break) and DEL are hex-escaped.
    const std::string unicode = chatRecordFor(std::string("caf\xC3\xA9 \xE2\x80\xA8x\x7F"));
    EXPECT_STR_CONTAINS(unicode, "text=\"caf\\xC3\\xA9 \\xE2\\x80\\xA8x\\x7F\"");
    EXPECT_TRUE(IsPrintableAscii(unicode));

    // Oversized chat is bounded in the log; the original size is still recorded.
    const std::string oversized(1000, 'A');
    const std::string bounded = chatRecordFor(oversized);
    EXPECT_STR_CONTAINS(bounded, "bytes=1000 ");
    EXPECT_STR_CONTAINS(bounded, "...[truncated]\"");
    EXPECT_TRUE(bounded.find(std::string(257, 'A')) == std::string::npos);
    EXPECT_TRUE(bounded.find(std::string(256, 'A')) != std::string::npos);
    EXPECT_TRUE(bounded.size() < 400u);

    server.Stop();
}

TEST(SEC100_ChatRelayAndHostCallbackStillReceiveOriginalText)
{
    std::string seenChat;
    ServerCallbacks callbacks;
    callbacks.onChatMessage = [&](const std::string& chat) { seenChat = chat; };

    Sec100ChatRuntime runtime;
    DedicatedServer server(runtime);
    ServerConfig config;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    config.mapRotation = {"arena"};
    ASSERT_TRUE(server.InitializeOnly(config));
    server.SetCallbacks(callbacks);

    // Escaping is a property of the log record only; players and the host still
    // get the text they were sent.
    const std::string text = "line one\nline \"two\"";
    const NetworkMessage chat = BuildChat(3, text);
    ASSERT_TRUE(runtime.Dispatch(chat));
    ASSERT_EQ(runtime.relayed.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(runtime.relayed[0].first, static_cast<ClientID>(3));
    EXPECT_TRUE(runtime.relayed[0].second.payload == chat.payload);
    EXPECT_EQ(seenChat, text);

    server.Stop();
}

#endif // ENABLE_NETWORKING
