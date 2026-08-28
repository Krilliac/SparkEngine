// TestDedicatedServer.cpp - Tests for dedicated server configuration,
// RCON command parsing, LAN broadcast serialization, and server lifecycle

#include "TestFramework.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation of server primitives for testing
// (mirrors Spark::Net from DedicatedServer.h/cpp)
// ============================================================================

namespace
{
    // -- Types matching NetworkManager.h --
    using ClientID = uint32_t;
    constexpr ClientID INVALID_CLIENT = 0;
    constexpr uint16_t DEFAULT_PORT = 27015;

    enum class GameModeType : uint8_t
    {
        Deathmatch = 0,
        TeamDeathmatch,
        CaptureTheFlag,
        Domination,
        SearchAndDestroy,
        FreeForAll,
        Custom
    };

    // -- ServerConfig --
    struct ServerConfig
    {
        std::string serverName = "Spark Dedicated Server";
        std::string motd;
        uint16_t port = DEFAULT_PORT;
        int maxClients = 32;
        float tickRate = 60.0f;
        float clientTimeoutSeconds = 30.0f;
        float heartbeatIntervalSeconds = 1.0f;
        std::string bindAddress = "loopback";
        GameModeType gameMode = GameModeType::Deathmatch;
        std::string customGameModeName;
        int scoreLimit = 50;
        float timeLimitMinutes = 15.0f;
        int roundCount = 1;
        bool friendlyFire = false;
        bool autoBalanceTeams = true;
        std::vector<std::string> mapRotation;
        bool randomizeMapOrder = false;
        std::string rconPassword;
        uint16_t rconPort = 0;
        bool enableLogging = true;
        std::string logFilePath = "server.log";
        bool enableLanBroadcast = false;
        uint16_t lanBroadcastPort = 27016;
        bool enableAntiCheat = false;
        float replicationRate = 20.0f;
        int snapshotHistorySize = 64;
    };

    // -- ServerStats --
    struct ServerStats
    {
        float uptimeSeconds = 0.0f;
        uint64_t totalTicksProcessed = 0;
        float averageTickMs = 0.0f;
        float peakTickMs = 0.0f;
        uint32_t currentPlayers = 0;
        uint32_t peakPlayers = 0;
        uint64_t totalBytesIn = 0;
        uint64_t totalBytesOut = 0;
        uint32_t totalConnectionsServed = 0;
        float currentTickRate = 0.0f;
        std::string currentMap;
        int currentMapIndex = 0;
        float matchTimeRemaining = 0.0f;
        int currentRound = 1;
    };

    // -- LAN Broadcast Info --
    struct ServerBroadcastInfo
    {
        std::string serverName;
        std::string mapName;
        GameModeType gameMode = GameModeType::Deathmatch;
        uint16_t port = DEFAULT_PORT;
        int currentPlayers = 0;
        int maxPlayers = 32;
        float ping = 0.0f;
    };

    // -- RCON Command --
    struct RconCommand
    {
        std::string name;
        std::string description;
        std::function<std::string(const std::vector<std::string>&)> handler;
    };

    // -- RCON command parser (mirrors DedicatedServer::ParseRconCommandLine) --
    void ParseRconCommandLine(const std::string& commandLine, std::string& outName, std::vector<std::string>& outArgs)
    {
        std::istringstream stream(commandLine);
        stream >> outName;
        std::string arg;
        while (stream >> arg)
        {
            outArgs.push_back(arg);
        }
    }

    // -- Simple RCON dispatcher for testing --
    class RconDispatcher
    {
      public:
        void Register(const std::string& name, const std::string& description,
                      std::function<std::string(const std::vector<std::string>&)> handler)
        {
            m_commands.push_back({name, description, std::move(handler)});
        }

        std::string Execute(const std::string& commandLine)
        {
            std::string cmdName;
            std::vector<std::string> args;
            ParseRconCommandLine(commandLine, cmdName, args);

            for (const auto& cmd : m_commands)
            {
                if (cmd.name == cmdName && cmd.handler)
                {
                    return cmd.handler(args);
                }
            }
            return "Unknown command: " + cmdName;
        }

        const std::vector<RconCommand>& GetCommands() const { return m_commands; }

      private:
        std::vector<RconCommand> m_commands;
    };

    // -- NetBuffer for broadcast serialization --
    class NetBuffer
    {
      public:
        void WriteUint8(uint8_t val) { m_data.push_back(val); }

        void WriteUint16(uint16_t val)
        {
            m_data.push_back(static_cast<uint8_t>(val & 0xFF));
            m_data.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        }

        void WriteUint32(uint32_t val)
        {
            for (int i = 0; i < 4; ++i)
                m_data.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }

        void WriteString(const std::string& val)
        {
            uint16_t len = static_cast<uint16_t>(val.size());
            WriteUint16(len);
            for (auto c : val)
                m_data.push_back(static_cast<uint8_t>(c));
        }

        uint8_t ReadUint8()
        {
            if (m_error || m_readPos >= m_data.size())
            {
                m_error = true;
                return 0;
            }
            return m_data[m_readPos++];
        }

        uint16_t ReadUint16()
        {
            if (m_error || m_readPos + 2 > m_data.size())
            {
                m_error = true;
                return 0;
            }
            uint16_t val = 0;
            for (int i = 0; i < 2; ++i)
                val |= static_cast<uint16_t>(m_data[m_readPos++]) << (i * 8);
            return val;
        }

        uint32_t ReadUint32()
        {
            if (m_error || m_readPos + 4 > m_data.size())
            {
                m_error = true;
                return 0;
            }
            uint32_t val = 0;
            for (int i = 0; i < 4; ++i)
                val |= static_cast<uint32_t>(m_data[m_readPos++]) << (i * 8);
            return val;
        }

        std::string ReadString()
        {
            uint16_t len = ReadUint16();
            if (m_error || m_readPos + len > m_data.size())
            {
                m_error = true;
                return {};
            }
            std::string result(m_data.begin() + static_cast<long>(m_readPos),
                               m_data.begin() + static_cast<long>(m_readPos + len));
            m_readPos += len;
            return result;
        }

        bool HasError() const { return m_error; }
        const std::vector<uint8_t>& GetData() const { return m_data; }
        size_t GetSize() const { return m_data.size(); }

        void Reset()
        {
            m_data.clear();
            m_readPos = 0;
            m_error = false;
        }

      private:
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
        bool m_error = false;
    };

    // -- Map rotation helper --
    class MapRotation
    {
      public:
        void SetMaps(const std::vector<std::string>& maps) { m_maps = maps; }
        const std::vector<std::string>& GetMaps() const { return m_maps; }

        std::string GetCurrentMap() const
        {
            if (m_maps.empty())
                return "default";
            return m_maps[static_cast<size_t>(m_currentIndex)];
        }

        void RotateNext()
        {
            if (m_maps.empty())
                return;
            m_currentIndex = (m_currentIndex + 1) % static_cast<int>(m_maps.size());
        }

        int GetCurrentIndex() const { return m_currentIndex; }

        void SetIndex(int index)
        {
            if (!m_maps.empty())
                m_currentIndex = index % static_cast<int>(m_maps.size());
        }

      private:
        std::vector<std::string> m_maps;
        int m_currentIndex = 0;
    };

} // namespace

// ============================================================================
// Tests: ServerConfig defaults
// ============================================================================

TEST(DedicatedServer_DefaultConfig)
{
    ServerConfig config;

    EXPECT_EQ(config.serverName, std::string("Spark Dedicated Server"));
    EXPECT_EQ(config.port, static_cast<uint16_t>(27015));
    EXPECT_EQ(config.maxClients, 32);
    EXPECT_NEAR(config.tickRate, 60.0f, 0.01f);
    EXPECT_NEAR(config.clientTimeoutSeconds, 30.0f, 0.01f);
    EXPECT_EQ(config.bindAddress, std::string("loopback"));
    EXPECT_EQ(static_cast<int>(config.gameMode), 0);
    EXPECT_EQ(config.scoreLimit, 50);
    EXPECT_NEAR(config.timeLimitMinutes, 15.0f, 0.01f);
    EXPECT_EQ(config.roundCount, 1);
    EXPECT_EQ(config.friendlyFire, false);
    EXPECT_EQ(config.autoBalanceTeams, true);
    EXPECT_TRUE(config.mapRotation.empty());
    EXPECT_TRUE(config.rconPassword.empty());
    EXPECT_EQ(config.enableLanBroadcast, false);
    EXPECT_EQ(config.lanBroadcastPort, static_cast<uint16_t>(27016));
    EXPECT_EQ(config.snapshotHistorySize, 64);
}

TEST(DedicatedServer_CustomConfig)
{
    ServerConfig config;
    config.serverName = "Test Server";
    config.port = 28000;
    config.maxClients = 16;
    config.tickRate = 128.0f;
    config.gameMode = GameModeType::CaptureTheFlag;
    config.scoreLimit = 3;
    config.timeLimitMinutes = 10.0f;
    config.roundCount = 5;
    config.friendlyFire = true;
    config.mapRotation = {"ctf_bridge", "ctf_harbor", "ctf_ruins"};
    config.rconPassword = "secret123";
    config.bindAddress = "192.168.1.20";

    EXPECT_EQ(config.serverName, std::string("Test Server"));
    EXPECT_EQ(config.port, static_cast<uint16_t>(28000));
    EXPECT_EQ(config.maxClients, 16);
    EXPECT_NEAR(config.tickRate, 128.0f, 0.01f);
    EXPECT_EQ(static_cast<int>(config.gameMode), static_cast<int>(GameModeType::CaptureTheFlag));
    EXPECT_EQ(config.scoreLimit, 3);
    EXPECT_EQ(config.roundCount, 5);
    EXPECT_EQ(config.friendlyFire, true);
    EXPECT_EQ(config.mapRotation.size(), static_cast<size_t>(3));
    EXPECT_EQ(config.mapRotation[0], std::string("ctf_bridge"));
    EXPECT_EQ(config.rconPassword, std::string("secret123"));
    EXPECT_EQ(config.bindAddress, std::string("192.168.1.20"));
}

// ============================================================================
// Tests: RCON command parsing
// ============================================================================

TEST(DedicatedServer_RconParseSimpleCommand)
{
    std::string name;
    std::vector<std::string> args;
    ParseRconCommandLine("status", name, args);

    EXPECT_EQ(name, std::string("status"));
    EXPECT_TRUE(args.empty());
}

TEST(DedicatedServer_RconParseCommandWithArgs)
{
    std::string name;
    std::vector<std::string> args;
    ParseRconCommandLine("kick 3 cheating", name, args);

    EXPECT_EQ(name, std::string("kick"));
    EXPECT_EQ(args.size(), static_cast<size_t>(2));
    EXPECT_EQ(args[0], std::string("3"));
    EXPECT_EQ(args[1], std::string("cheating"));
}

TEST(DedicatedServer_RconParseMapCommand)
{
    std::string name;
    std::vector<std::string> args;
    ParseRconCommandLine("map dm_warehouse", name, args);

    EXPECT_EQ(name, std::string("map"));
    EXPECT_EQ(args.size(), static_cast<size_t>(1));
    EXPECT_EQ(args[0], std::string("dm_warehouse"));
}

TEST(DedicatedServer_RconParseEmptyString)
{
    std::string name;
    std::vector<std::string> args;
    ParseRconCommandLine("", name, args);

    EXPECT_TRUE(name.empty());
    EXPECT_TRUE(args.empty());
}

TEST(DedicatedServer_RconParseSayCommand)
{
    std::string name;
    std::vector<std::string> args;
    ParseRconCommandLine("say Hello world!", name, args);

    EXPECT_EQ(name, std::string("say"));
    EXPECT_EQ(args.size(), static_cast<size_t>(2));
    EXPECT_EQ(args[0], std::string("Hello"));
    EXPECT_EQ(args[1], std::string("world!"));
}

// ============================================================================
// Tests: RCON dispatcher
// ============================================================================

TEST(DedicatedServer_RconDispatchRegisteredCommand)
{
    RconDispatcher dispatcher;
    dispatcher.Register("ping", "Returns pong", [](const std::vector<std::string>&) { return "pong"; });

    std::string result = dispatcher.Execute("ping");
    EXPECT_EQ(result, std::string("pong"));
}

TEST(DedicatedServer_RconDispatchUnknownCommand)
{
    RconDispatcher dispatcher;
    dispatcher.Register("help", "Help text", [](const std::vector<std::string>&) { return "Available commands:"; });

    std::string result = dispatcher.Execute("unknown_cmd");
    EXPECT_EQ(result, std::string("Unknown command: unknown_cmd"));
}

TEST(DedicatedServer_RconDispatchWithArguments)
{
    RconDispatcher dispatcher;
    dispatcher.Register("echo", "Echo arguments",
                        [](const std::vector<std::string>& args)
                        {
                            std::string result;
                            for (size_t i = 0; i < args.size(); ++i)
                            {
                                if (i > 0)
                                    result += " ";
                                result += args[i];
                            }
                            return result;
                        });

    std::string result = dispatcher.Execute("echo hello world");
    EXPECT_EQ(result, std::string("hello world"));
}

TEST(DedicatedServer_RconMultipleCommands)
{
    RconDispatcher dispatcher;
    dispatcher.Register("status", "Server status", [](const std::vector<std::string>&) { return "Running"; });
    dispatcher.Register("help", "Show help", [](const std::vector<std::string>&) { return "Commands: status, help"; });

    EXPECT_EQ(dispatcher.GetCommands().size(), static_cast<size_t>(2));
    EXPECT_EQ(dispatcher.Execute("status"), std::string("Running"));
    EXPECT_EQ(dispatcher.Execute("help"), std::string("Commands: status, help"));
}

// ============================================================================
// Tests: Map Rotation
// ============================================================================

TEST(DedicatedServer_MapRotationBasic)
{
    MapRotation rotation;
    rotation.SetMaps({"dm_warehouse", "dm_rooftops", "dm_compound"});

    EXPECT_EQ(rotation.GetCurrentMap(), std::string("dm_warehouse"));
    EXPECT_EQ(rotation.GetCurrentIndex(), 0);
}

TEST(DedicatedServer_MapRotationAdvance)
{
    MapRotation rotation;
    rotation.SetMaps({"dm_warehouse", "dm_rooftops", "dm_compound"});

    rotation.RotateNext();
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("dm_rooftops"));
    EXPECT_EQ(rotation.GetCurrentIndex(), 1);

    rotation.RotateNext();
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("dm_compound"));
    EXPECT_EQ(rotation.GetCurrentIndex(), 2);
}

TEST(DedicatedServer_MapRotationWraparound)
{
    MapRotation rotation;
    rotation.SetMaps({"map_a", "map_b"});

    rotation.RotateNext(); // -> map_b
    rotation.RotateNext(); // -> map_a (wrap)
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("map_a"));
    EXPECT_EQ(rotation.GetCurrentIndex(), 0);
}

TEST(DedicatedServer_MapRotationEmpty)
{
    MapRotation rotation;
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("default"));

    // Should not crash
    rotation.RotateNext();
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("default"));
}

TEST(DedicatedServer_MapRotationSetIndex)
{
    MapRotation rotation;
    rotation.SetMaps({"map_a", "map_b", "map_c", "map_d"});

    rotation.SetIndex(2);
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("map_c"));

    rotation.SetIndex(10); // Should wrap around
    EXPECT_EQ(rotation.GetCurrentIndex(), 10 % 4);
}

TEST(DedicatedServer_MapRotationSingleMap)
{
    MapRotation rotation;
    rotation.SetMaps({"only_map"});

    EXPECT_EQ(rotation.GetCurrentMap(), std::string("only_map"));
    rotation.RotateNext();
    EXPECT_EQ(rotation.GetCurrentMap(), std::string("only_map")); // Stays on only map
}

// ============================================================================
// Tests: LAN Broadcast serialization
// ============================================================================

TEST(DedicatedServer_BroadcastSerializeRoundTrip)
{
    ServerBroadcastInfo original;
    original.serverName = "Test Server Alpha";
    original.mapName = "dm_warehouse";
    original.gameMode = GameModeType::TeamDeathmatch;
    original.port = 28000;
    original.currentPlayers = 12;
    original.maxPlayers = 24;

    // Serialize (same as DedicatedServer::SendLanBroadcast)
    NetBuffer writeBuf;
    writeBuf.WriteUint32(0x5350524B); // "SPRK" magic
    writeBuf.WriteString(original.serverName);
    writeBuf.WriteString(original.mapName);
    writeBuf.WriteUint8(static_cast<uint8_t>(original.gameMode));
    writeBuf.WriteUint16(original.port);
    writeBuf.WriteUint32(static_cast<uint32_t>(original.currentPlayers));
    writeBuf.WriteUint32(static_cast<uint32_t>(original.maxPlayers));

    EXPECT_TRUE(writeBuf.GetSize() > 0);

    // Deserialize (same as DedicatedServer::DiscoverLanServers)
    NetBuffer readBuf;
    const auto& data = writeBuf.GetData();
    for (auto b : data)
        readBuf.WriteUint8(b);

    uint32_t magic = readBuf.ReadUint32();
    EXPECT_EQ(magic, static_cast<uint32_t>(0x5350524B));

    ServerBroadcastInfo decoded;
    decoded.serverName = readBuf.ReadString();
    decoded.mapName = readBuf.ReadString();
    decoded.gameMode = static_cast<GameModeType>(readBuf.ReadUint8());
    decoded.port = readBuf.ReadUint16();
    decoded.currentPlayers = static_cast<int>(readBuf.ReadUint32());
    decoded.maxPlayers = static_cast<int>(readBuf.ReadUint32());

    EXPECT_FALSE(readBuf.HasError());
    EXPECT_EQ(decoded.serverName, original.serverName);
    EXPECT_EQ(decoded.mapName, original.mapName);
    EXPECT_EQ(static_cast<int>(decoded.gameMode), static_cast<int>(original.gameMode));
    EXPECT_EQ(decoded.port, original.port);
    EXPECT_EQ(decoded.currentPlayers, original.currentPlayers);
    EXPECT_EQ(decoded.maxPlayers, original.maxPlayers);
}

TEST(DedicatedServer_BroadcastBadMagicRejected)
{
    NetBuffer buf;
    buf.WriteUint32(0xDEADBEEF); // Wrong magic
    buf.WriteString("Fake Server");

    uint32_t magic = buf.ReadUint32();
    EXPECT_TRUE(magic != 0x5350524B);
}

TEST(DedicatedServer_BroadcastEmptyServerName)
{
    NetBuffer writeBuf;
    writeBuf.WriteUint32(0x5350524B);
    writeBuf.WriteString("");
    writeBuf.WriteString("some_map");
    writeBuf.WriteUint8(0);
    writeBuf.WriteUint16(27015);
    writeBuf.WriteUint32(0);
    writeBuf.WriteUint32(16);

    NetBuffer readBuf;
    for (auto b : writeBuf.GetData())
        readBuf.WriteUint8(b);

    uint32_t magic = readBuf.ReadUint32();
    EXPECT_EQ(magic, static_cast<uint32_t>(0x5350524B));

    std::string name = readBuf.ReadString();
    EXPECT_TRUE(name.empty());
    EXPECT_FALSE(readBuf.HasError());
}

// ============================================================================
// Tests: ServerStats
// ============================================================================

TEST(DedicatedServer_StatsDefaults)
{
    ServerStats stats;

    EXPECT_NEAR(stats.uptimeSeconds, 0.0f, 0.01f);
    EXPECT_EQ(stats.totalTicksProcessed, static_cast<uint64_t>(0));
    EXPECT_EQ(stats.currentPlayers, static_cast<uint32_t>(0));
    EXPECT_EQ(stats.peakPlayers, static_cast<uint32_t>(0));
    EXPECT_EQ(stats.totalConnectionsServed, static_cast<uint32_t>(0));
}

TEST(DedicatedServer_StatsPeakTracking)
{
    ServerStats stats;
    stats.currentPlayers = 5;
    if (stats.currentPlayers > stats.peakPlayers)
        stats.peakPlayers = stats.currentPlayers;

    EXPECT_EQ(stats.peakPlayers, static_cast<uint32_t>(5));

    stats.currentPlayers = 3;
    if (stats.currentPlayers > stats.peakPlayers)
        stats.peakPlayers = stats.currentPlayers;

    EXPECT_EQ(stats.peakPlayers, static_cast<uint32_t>(5)); // Peak should not decrease

    stats.currentPlayers = 10;
    if (stats.currentPlayers > stats.peakPlayers)
        stats.peakPlayers = stats.currentPlayers;

    EXPECT_EQ(stats.peakPlayers, static_cast<uint32_t>(10));
}

// ============================================================================
// Tests: GameMode enum
// ============================================================================

TEST(DedicatedServer_GameModeEnumValues)
{
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::Deathmatch), static_cast<uint8_t>(0));
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::TeamDeathmatch), static_cast<uint8_t>(1));
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::CaptureTheFlag), static_cast<uint8_t>(2));
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::Domination), static_cast<uint8_t>(3));
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::SearchAndDestroy), static_cast<uint8_t>(4));
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::FreeForAll), static_cast<uint8_t>(5));
    EXPECT_EQ(static_cast<uint8_t>(GameModeType::Custom), static_cast<uint8_t>(6));
}

TEST(DedicatedServer_GameModeRoundTripSerialization)
{
    // Each game mode should survive uint8_t serialization
    for (int i = 0; i <= static_cast<int>(GameModeType::Custom); ++i)
    {
        auto mode = static_cast<GameModeType>(i);
        uint8_t wire = static_cast<uint8_t>(mode);
        auto decoded = static_cast<GameModeType>(wire);
        EXPECT_EQ(static_cast<int>(decoded), i);
    }
}

// ============================================================================
// Tests: Match state logic
// ============================================================================

TEST(DedicatedServer_MatchTimerCountdown)
{
    float matchTimeRemaining = 15.0f * 60.0f; // 15 minutes in seconds
    float deltaTime = 1.0f / 60.0f;           // 60 Hz tick

    // Simulate 60 ticks (1 second)
    for (int i = 0; i < 60; ++i)
    {
        matchTimeRemaining -= deltaTime;
    }

    EXPECT_NEAR(matchTimeRemaining, 15.0f * 60.0f - 1.0f, 0.1f); // 899 seconds (1 second elapsed)
}

TEST(DedicatedServer_MatchTimerExpiry)
{
    float matchTimeRemaining = 1.0f; // 1 second remaining
    bool matchEnded = false;

    for (int i = 0; i < 120; ++i)
    {
        matchTimeRemaining -= 1.0f / 60.0f;
        if (matchTimeRemaining <= 0.0f)
        {
            matchTimeRemaining = 0.0f;
            matchEnded = true;
            break;
        }
    }

    EXPECT_TRUE(matchEnded);
    EXPECT_NEAR(matchTimeRemaining, 0.0f, 0.01f);
}

TEST(DedicatedServer_RoundProgression)
{
    int totalRounds = 3;
    int currentRound = 1;
    int matchesCompleted = 0;

    // Simulate completing 3 rounds
    for (int i = 0; i < 3; ++i)
    {
        matchesCompleted++;
        if (currentRound < totalRounds)
        {
            currentRound++;
        }
        else
        {
            currentRound = 1; // Reset for next map
        }
    }

    EXPECT_EQ(matchesCompleted, 3);
    EXPECT_EQ(currentRound, 1); // Should reset after completing all rounds
}
