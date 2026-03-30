/**
 * @file DedicatedServer.cpp
 * @brief Dedicated game server implementation
 *
 * Full implementation of the headless dedicated server with tick loop,
 * map rotation, RCON, LAN discovery, and match state management.
 *
 * All socket-level code is guarded by ENABLE_NETWORKING.
 */

#include "DedicatedServer.h"

#ifdef ENABLE_NETWORKING

// Windows headers may redefine SendMessage after our includes.
#ifdef SendMessage
#undef SendMessage
#endif

#include "../../Core/FaultIsolation.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Spark::Net
{

    // ============================================================================
    // Constructor / Destructor
    // ============================================================================

    DedicatedServer::DedicatedServer() = default;

    DedicatedServer::~DedicatedServer()
    {
        if (m_running.load(std::memory_order_acquire))
        {
            Stop();
        }
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool DedicatedServer::InitializeOnly(const ServerConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (m_running.load(std::memory_order_acquire))
            return false;

        m_config = config;
        m_stats = ServerStats{};
        m_currentRound = 1;
        m_matchInProgress = false;

        auto& netMgr = NetworkManager::GetInstance();
        if (!netMgr.Initialize())
        {
            Log("ERROR: Failed to initialize NetworkManager");
            return false;
        }

        if (!netMgr.StartServer(m_config.port, m_config.maxClients))
        {
            Log("ERROR: Failed to start server on port " + std::to_string(m_config.port));
            netMgr.Shutdown();
            return false;
        }

        RegisterBuiltInRconCommands();

        // Register handlers for dedicated-server-specific messages
        netMgr.RegisterHandler(MessageType::Connect,
                               [this](const NetworkMessage& msg)
                               {
                                   // Check bans
                                   {
                                       std::lock_guard<std::mutex> lock(m_banMutex);
                                       if (Spark::ContainerUtils::Contains(m_bannedClients, msg.senderID))
                                       {
                                           NetworkManager::GetInstance().KickClient(msg.senderID,
                                                                                    "You are banned from this server");
                                           return;
                                       }
                                   }
                                   m_stats.totalConnectionsServed++;
                                   uint32_t playerCount = GetPlayerCount();
                                   if (playerCount > m_stats.peakPlayers)
                                       m_stats.peakPlayers = playerCount;
                                   m_stats.currentPlayers = playerCount;

                                   if (m_callbacks.onClientConnected)
                                   {
                                       auto clients = NetworkManager::GetInstance().GetClients();
                                       auto it = clients.find(msg.senderID);
                                       std::string name = (it != clients.end()) ? it->second.name : "Unknown";
                                       m_callbacks.onClientConnected(msg.senderID, name);
                                   }
                                   Log("Client " + std::to_string(msg.senderID) + " connected");
                               });

        netMgr.RegisterHandler(MessageType::Disconnect,
                               [this](const NetworkMessage& msg)
                               {
                                   m_stats.currentPlayers = GetPlayerCount();
                                   if (m_callbacks.onClientDisconnected)
                                   {
                                       m_callbacks.onClientDisconnected(msg.senderID, "Disconnected");
                                   }
                                   Log("Client " + std::to_string(msg.senderID) + " disconnected");
                               });

        netMgr.RegisterHandler(MessageType::ChatMessage,
                               [this](const NetworkMessage& msg)
                               {
                                   if (msg.payload.empty())
                                       return;
                                   NetBuffer buf;
                                   buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                   std::string chatText = buf.ReadString();

                                   // Check for RCON prefix
                                   if (chatText.size() > 1 && chatText[0] == '/')
                                   {
                                       std::string rconCmd = chatText.substr(1);
                                       std::string response = ExecuteRcon(rconCmd);
                                       // Send response back to the issuing client
                                       NetworkMessage reply;
                                       reply.type = MessageType::ChatMessage;
                                       reply.channel = ChannelType::Reliable;
                                       NetBuffer replyBuf;
                                       replyBuf.WriteString("[RCON] " + response);
                                       reply.payload = replyBuf.GetData();
                                       NetworkManager::GetInstance().SendToClient(msg.senderID, reply);
                                       return;
                                   }

                                   // Broadcast chat to all clients
                                   NetworkMessage broadcast;
                                   broadcast.type = MessageType::ChatMessage;
                                   broadcast.channel = ChannelType::Reliable;
                                   broadcast.payload = msg.payload;
                                   NetworkManager::GetInstance().SendToAllExcept(msg.senderID, broadcast);

                                   if (m_callbacks.onChatMessage)
                                       m_callbacks.onChatMessage(chatText);
                                   Log("Chat: " + chatText);
                               });

        // Set initial map
        if (!m_config.mapRotation.empty())
        {
            m_currentMapIndex = 0;
            m_currentMap = m_config.mapRotation[0];
        }
        else
        {
            m_currentMap = "default";
        }
        m_stats.currentMap = m_currentMap;

        m_startTime = std::chrono::steady_clock::now();
        m_running.store(true, std::memory_order_release);

        Log("Server '" + m_config.serverName + "' initialized on port " + std::to_string(m_config.port));
        return true;
    }

    bool DedicatedServer::Start(const ServerConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (!InitializeOnly(config))
            return false;

        // Start LAN broadcast if enabled
        if (m_config.enableLanBroadcast)
        {
            StartLanBroadcast();
        }

        // Auto-start the first match
        StartMatch();

        // Launch the tick loop thread
        m_tickThread = std::thread(&DedicatedServer::TickLoop, this);

        if (m_callbacks.onServerStarted)
            m_callbacks.onServerStarted();

        Log("Server started — tick rate: " + std::to_string(static_cast<int>(m_config.tickRate)) + " Hz");
        return true;
    }

    void DedicatedServer::Stop()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (!m_running.load(std::memory_order_acquire))
            return;

        Log("Server shutting down...");

        // Signal all clients
        auto& netMgr = NetworkManager::GetInstance();
        NetworkMessage shutdownMsg;
        shutdownMsg.type = MessageType::Disconnect;
        shutdownMsg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString("Server shutting down");
        shutdownMsg.payload = buf.GetData();
        netMgr.SendToAll(shutdownMsg);

        // Give the message a chance to be flushed
        netMgr.Update(0.0f);

        m_running.store(false, std::memory_order_release);

        // Stop LAN broadcast
        StopLanBroadcast();

        // Join tick thread
        if (m_tickThread.joinable())
            m_tickThread.join();

        // Join LAN broadcast thread
        if (m_lanBroadcastThread.joinable())
            m_lanBroadcastThread.join();

        netMgr.StopServer();
        netMgr.Shutdown();

        if (m_callbacks.onServerStopped)
            m_callbacks.onServerStopped();

        Log("Server stopped. Uptime: " + std::to_string(static_cast<int>(m_stats.uptimeSeconds)) + "s");
    }

    void DedicatedServer::Tick(float deltaTime)
    {
        if (!m_running.load(std::memory_order_acquire))
            return;

        auto tickStart = std::chrono::steady_clock::now();

        auto& netMgr = NetworkManager::GetInstance();
        SPARK_GUARDED_UPDATE("Server:Network", "Network", { netMgr.Update(deltaTime); });

        SPARK_GUARDED_UPDATE("Server:Messages", "Network", { ProcessServerMessages(deltaTime); });
        SPARK_GUARDED_UPDATE("Server:MatchState", "Network", { UpdateMatchState(deltaTime); });

        // Update stats
        m_stats.totalTicksProcessed++;
        auto now = std::chrono::steady_clock::now();
        m_stats.uptimeSeconds = std::chrono::duration<float>(now - m_startTime).count();

        float tickMs = std::chrono::duration<float, std::milli>(now - tickStart).count();
        if (tickMs > m_stats.peakTickMs)
            m_stats.peakTickMs = tickMs;

        // Running average
        float alpha = 0.05f;
        m_stats.averageTickMs = m_stats.averageTickMs * (1.0f - alpha) + tickMs * alpha;
        m_stats.currentTickRate = (tickMs > 0.0f) ? (1000.0f / tickMs) : m_config.tickRate;

        // Update network byte counters
        const auto& netStats = netMgr.GetStats();
        m_stats.totalBytesIn = netStats.bytesReceived;
        m_stats.totalBytesOut = netStats.bytesSent;
        m_stats.currentPlayers = GetPlayerCount();
    }

    // ============================================================================
    // Tick Loop (background thread)
    // ============================================================================

    void DedicatedServer::TickLoop()
    {
        const auto tickInterval = std::chrono::duration<double>(1.0 / m_config.tickRate);
        auto previousTime = std::chrono::steady_clock::now();

        while (m_running.load(std::memory_order_acquire))
        {
            auto currentTime = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - previousTime).count();
            previousTime = currentTime;

            Tick(deltaTime);

            // Sleep until next tick
            auto elapsed = std::chrono::steady_clock::now() - currentTime;
            auto sleepTime = tickInterval - elapsed;
            if (sleepTime > std::chrono::duration<double>::zero())
            {
                std::this_thread::sleep_for(sleepTime);
            }
        }
    }

    // ============================================================================
    // Server Message Processing
    // ============================================================================

    void DedicatedServer::ProcessServerMessages(float /*deltaTime*/)
    {
        // NetworkManager dispatches to registered handlers already.
        // This method is reserved for future server-specific per-tick logic
        // such as anti-cheat validation or rate-limit enforcement.
    }

    // ============================================================================
    // Match State
    // ============================================================================

    void DedicatedServer::StartMatch()
    {
        m_matchInProgress = true;
        m_matchTimeRemaining = m_config.timeLimitMinutes * 60.0f;
        m_currentRound = 1;
        m_stats.matchTimeRemaining = m_matchTimeRemaining;
        m_stats.currentRound = m_currentRound;

        // Broadcast match start to all clients
        NetworkMessage msg;
        msg.type = MessageType::MatchStart;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(m_currentMap);
        buf.WriteUint8(static_cast<uint8_t>(m_config.gameMode));
        buf.WriteFloat(m_matchTimeRemaining);
        buf.WriteUint32(static_cast<uint32_t>(m_config.scoreLimit));
        msg.payload = buf.GetData();
        NetworkManager::GetInstance().SendToAll(msg);

        Log("Match started on map '" + m_currentMap + "'");
    }

    void DedicatedServer::EndMatch()
    {
        if (!m_matchInProgress)
            return;

        m_matchInProgress = false;

        // Broadcast match end
        NetworkMessage msg;
        msg.type = MessageType::MatchEnd;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(m_currentMap);
        buf.WriteUint32(static_cast<uint32_t>(m_currentRound));
        msg.payload = buf.GetData();
        NetworkManager::GetInstance().SendToAll(msg);

        Log("Match ended on map '" + m_currentMap + "'");

        // Advance round or map
        if (m_currentRound < m_config.roundCount)
        {
            m_currentRound++;
            m_stats.currentRound = m_currentRound;
            StartMatch();
        }
        else
        {
            // Rotate to next map
            RotateToNextMap();
            m_currentRound = 1;
            m_stats.currentRound = m_currentRound;
            StartMatch();
        }
    }

    void DedicatedServer::UpdateMatchState(float deltaTime)
    {
        if (!m_matchInProgress)
            return;

        m_matchTimeRemaining -= deltaTime;
        m_stats.matchTimeRemaining = m_matchTimeRemaining;

        if (m_matchTimeRemaining <= 0.0f)
        {
            m_matchTimeRemaining = 0.0f;
            EndMatch();
        }

        // Periodically sync game state to clients
        static float syncTimer = 0.0f;
        syncTimer += deltaTime;
        if (syncTimer >= 5.0f)
        {
            syncTimer = 0.0f;
            NetworkMessage msg;
            msg.type = MessageType::GameStateSync;
            msg.channel = ChannelType::Unreliable;
            NetBuffer buf;
            buf.WriteFloat(m_matchTimeRemaining);
            buf.WriteUint32(static_cast<uint32_t>(m_currentRound));
            buf.WriteUint32(m_stats.currentPlayers);
            msg.payload = buf.GetData();
            NetworkManager::GetInstance().SendToAll(msg);
        }
    }

    // ============================================================================
    // Map Management
    // ============================================================================

    void DedicatedServer::ChangeMap(const std::string& mapName)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Network, mapName);
        if (m_matchInProgress)
            EndMatch();

        m_currentMap = mapName;
        m_stats.currentMap = m_currentMap;

        // Update rotation index if this map is in the list
        for (size_t i = 0; i < m_config.mapRotation.size(); ++i)
        {
            if (m_config.mapRotation[i] == mapName)
            {
                m_currentMapIndex = static_cast<int>(i);
                break;
            }
        }

        if (m_callbacks.onMapChanged)
            m_callbacks.onMapChanged(mapName);

        Log("Map changed to '" + mapName + "'");
        StartMatch();
    }

    void DedicatedServer::RotateToNextMap()
    {
        if (m_config.mapRotation.empty())
            return;

        if (m_config.randomizeMapOrder)
        {
            // Simple pseudo-random: use tick count as seed
            int idx = static_cast<int>(m_stats.totalTicksProcessed % m_config.mapRotation.size());
            m_currentMapIndex = idx;
        }
        else
        {
            m_currentMapIndex = (m_currentMapIndex + 1) % static_cast<int>(m_config.mapRotation.size());
        }

        m_currentMap = m_config.mapRotation[static_cast<size_t>(m_currentMapIndex)];
        m_stats.currentMap = m_currentMap;
        m_stats.currentMapIndex = m_currentMapIndex;

        if (m_callbacks.onMapChanged)
            m_callbacks.onMapChanged(m_currentMap);

        Log("Map rotated to '" + m_currentMap + "'");
    }

    // ============================================================================
    // Player Management
    // ============================================================================

    void DedicatedServer::KickPlayer(ClientID id, const std::string& reason)
    {
        SPARK_WARN_IF(Spark::LogCategory::Network, reason.empty(), "KickPlayer called with empty reason");
        NetworkManager::GetInstance().KickClient(id, reason);
        m_stats.currentPlayers = GetPlayerCount();
        Log("Kicked client " + std::to_string(id) + ": " + reason);
    }

    void DedicatedServer::BanPlayer(ClientID id, const std::string& reason)
    {
        {
            std::lock_guard<std::mutex> lock(m_banMutex);
            m_bannedClients.push_back(id);
        }
        KickPlayer(id, "Banned: " + reason);
        Log("Banned client " + std::to_string(id) + ": " + reason);
    }

    std::vector<ClientInfo> DedicatedServer::GetConnectedClients() const
    {
        std::vector<ClientInfo> result;
        const auto& clients = NetworkManager::GetInstance().GetClients();
        result.reserve(clients.size());
        for (const auto& [id, info] : clients)
        {
            result.push_back(info);
        }
        return result;
    }

    uint32_t DedicatedServer::GetPlayerCount() const
    {
        return static_cast<uint32_t>(NetworkManager::GetInstance().GetClients().size());
    }

    // ============================================================================
    // RCON
    // ============================================================================

    void DedicatedServer::RegisterRconCommand(const std::string& name, const std::string& description,
                                              std::function<std::string(const std::vector<std::string>&)> handler)
    {
        std::lock_guard<std::mutex> lock(m_rconMutex);
        m_rconCommands.push_back({name, description, std::move(handler)});
    }

    std::string DedicatedServer::ExecuteRcon(const std::string& commandLine)
    {
        std::string cmdName;
        std::vector<std::string> args;
        ParseRconCommandLine(commandLine, cmdName, args);

        std::lock_guard<std::mutex> lock(m_rconMutex);
        for (const auto& cmd : m_rconCommands)
        {
            if (cmd.name == cmdName && cmd.handler)
            {
                std::string response = cmd.handler(args);
                if (m_callbacks.onRconCommand)
                    m_callbacks.onRconCommand(commandLine, response);
                Log("RCON: " + commandLine + " -> " + response);
                return response;
            }
        }

        std::string err = "Unknown command: " + cmdName;
        Log("RCON: " + err);
        return err;
    }

    void DedicatedServer::RegisterBuiltInRconCommands()
    {
        RegisterRconCommand("help", "List available commands",
                            [this](const std::vector<std::string>&)
                            {
                                std::lock_guard<std::mutex> lock(m_rconMutex);
                                std::ostringstream oss;
                                oss << "Available commands:\n";
                                for (const auto& cmd : m_rconCommands)
                                {
                                    oss << "  " << cmd.name << " — " << cmd.description << "\n";
                                }
                                return oss.str();
                            });

        RegisterRconCommand("status", "Show server status",
                            [this](const std::vector<std::string>&) { return Console_GetStatus(); });

        RegisterRconCommand("kick", "Kick a player: kick <id> [reason]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (args.empty())
                                    return std::string("Usage: kick <clientID> [reason]");
                                try
                                {
                                    ClientID id = static_cast<ClientID>(std::stoul(args[0]));
                                    std::string reason = (args.size() > 1) ? args[1] : "Kicked by admin";
                                    KickPlayer(id, reason);
                                    return std::format("Kicked client {}", id);
                                }
                                catch (const std::exception&)
                                {
                                    return std::format("Invalid client ID: {}", args[0]);
                                }
                            });

        RegisterRconCommand("ban", "Ban a player: ban <id> [reason]",
                            [this](const std::vector<std::string>& args)
                            {
                                if (args.empty())
                                    return std::string("Usage: ban <clientID> [reason]");
                                try
                                {
                                    ClientID id = static_cast<ClientID>(std::stoul(args[0]));
                                    std::string reason = (args.size() > 1) ? args[1] : "Banned by admin";
                                    BanPlayer(id, reason);
                                    return std::format("Banned client {}", id);
                                }
                                catch (const std::exception&)
                                {
                                    return std::format("Invalid client ID: {}", args[0]);
                                }
                            });

        RegisterRconCommand("map", "Change map: map <name>",
                            [this](const std::vector<std::string>& args)
                            {
                                if (args.empty())
                                    return std::format("Current map: {}", m_currentMap);
                                ChangeMap(args[0]);
                                return std::format("Changing map to {}", args[0]);
                            });

        RegisterRconCommand("say", "Broadcast server message: say <text>",
                            [](const std::vector<std::string>& args)
                            {
                                if (args.empty())
                                    return std::string("Usage: say <message>");
                                std::string text;
                                for (size_t i = 0; i < args.size(); ++i)
                                {
                                    if (i > 0)
                                        text += " ";
                                    text += args[i];
                                }
                                NetworkMessage msg;
                                msg.type = MessageType::ChatMessage;
                                msg.channel = ChannelType::Reliable;
                                NetBuffer buf;
                                buf.WriteString("[SERVER] " + text);
                                msg.payload = buf.GetData();
                                NetworkManager::GetInstance().SendToAll(msg);
                                return std::format("Broadcast: {}", text);
                            });

        RegisterRconCommand("players", "List connected players",
                            [this](const std::vector<std::string>&)
                            {
                                auto clients = GetConnectedClients();
                                std::ostringstream oss;
                                oss << "Connected players (" << clients.size() << "):\n";
                                for (const auto& c : clients)
                                {
                                    oss << "  [" << c.id << "] " << c.name << " | Ping: " << c.stats.ping << "ms\n";
                                }
                                return oss.str();
                            });

        RegisterRconCommand("endmatch", "End the current match",
                            [this](const std::vector<std::string>&)
                            {
                                EndMatch();
                                return std::string("Match ended");
                            });

        RegisterRconCommand("nextmap", "Skip to the next map in rotation",
                            [this](const std::vector<std::string>&)
                            {
                                RotateToNextMap();
                                StartMatch();
                                return std::format("Rotated to map: {}", m_currentMap);
                            });

        RegisterRconCommand("quit", "Shut down the server",
                            [this](const std::vector<std::string>&)
                            {
                                Stop();
                                return std::string("Server shutting down...");
                            });
    }

    void DedicatedServer::ParseRconCommandLine(const std::string& commandLine, std::string& outName,
                                               std::vector<std::string>& outArgs)
    {
        std::istringstream stream(commandLine);
        stream >> outName;
        std::string arg;
        while (stream >> arg)
        {
            outArgs.push_back(arg);
        }
    }

    // ============================================================================
    // LAN Discovery
    // ============================================================================

    void DedicatedServer::StartLanBroadcast()
    {
        if (m_lanBroadcastActive.load(std::memory_order_acquire))
            return;

        m_lanBroadcastActive.store(true, std::memory_order_release);

        m_lanBroadcastThread = std::thread(
            [this]()
            {
                // Create a UDP socket for broadcasting
                SOCKET broadcastSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (broadcastSocket == INVALID_SOCKET)
                    return;

                // Enable broadcast
                int broadcastEnable = 1;
                setsockopt(broadcastSocket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcastEnable),
                           sizeof(broadcastEnable));

                sockaddr_in broadcastAddr{};
                broadcastAddr.sin_family = AF_INET;
                broadcastAddr.sin_port = htons(m_config.lanBroadcastPort);
                broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

                while (m_lanBroadcastActive.load(std::memory_order_acquire))
                {
                    // Serialize broadcast info
                    NetBuffer buf;
                    // Magic header for identification
                    buf.WriteUint32(0x5350524B); // "SPRK"
                    buf.WriteString(m_config.serverName);
                    buf.WriteString(m_currentMap);
                    buf.WriteUint8(static_cast<uint8_t>(m_config.gameMode));
                    buf.WriteUint16(m_config.port);
                    buf.WriteUint32(m_stats.currentPlayers);
                    buf.WriteUint32(static_cast<uint32_t>(m_config.maxClients));

                    const auto& data = buf.GetData();
                    sendto(broadcastSocket, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                           0, reinterpret_cast<const sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));

                    // Sleep between broadcasts
                    auto sleepTime = std::chrono::duration<float>(m_lanBroadcastInterval);
                    auto endTime = std::chrono::steady_clock::now() + sleepTime;
                    while (std::chrono::steady_clock::now() < endTime &&
                           m_lanBroadcastActive.load(std::memory_order_acquire))
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }

#ifdef SPARK_PLATFORM_WINDOWS
                closesocket(broadcastSocket);
#else
                close(broadcastSocket);
#endif
            });
    }

    void DedicatedServer::StopLanBroadcast()
    {
        m_lanBroadcastActive.store(false, std::memory_order_release);
        if (m_lanBroadcastThread.joinable())
            m_lanBroadcastThread.join();
    }

    std::vector<ServerBroadcastInfo> DedicatedServer::DiscoverLanServers(uint16_t broadcastPort, int timeoutMs)
    {
        std::vector<ServerBroadcastInfo> servers;

        SOCKET listenSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (listenSocket == INVALID_SOCKET)
            return servers;

        // Allow address reuse
        int reuseAddr = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr),
                   sizeof(reuseAddr));

        // Bind to broadcast port
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(broadcastPort);
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(listenSocket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            closesocket(listenSocket);
#else
            close(listenSocket);
#endif
            return servers;
        }

        // Set non-blocking
#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        ioctlsocket(listenSocket, FIONBIO, &nonBlocking);
#else
        int flags = fcntl(listenSocket, F_GETFL, 0);
        fcntl(listenSocket, F_SETFL, flags | O_NONBLOCK);
#endif

        auto startTime = std::chrono::steady_clock::now();
        auto timeout = std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() - startTime < timeout)
        {
            uint8_t recvBuf[1024];
            sockaddr_in senderAddr{};
            socklen_t senderLen = sizeof(senderAddr);

            int received = recvfrom(listenSocket, reinterpret_cast<char*>(recvBuf), sizeof(recvBuf), 0,
                                    reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

            if (received > 0)
            {
                NetBuffer buf;
                buf.WriteBytes(recvBuf, static_cast<size_t>(received));

                uint32_t magic = buf.ReadUint32();
                if (magic != 0x5350524B) // "SPRK"
                    continue;

                ServerBroadcastInfo info;
                info.serverName = buf.ReadString();
                info.mapName = buf.ReadString();
                info.gameMode = static_cast<GameModeType>(buf.ReadUint8());
                info.port = buf.ReadUint16();
                info.currentPlayers = static_cast<int>(buf.ReadUint32());
                info.maxPlayers = static_cast<int>(buf.ReadUint32());

                if (!buf.HasError())
                {
                    servers.push_back(info);
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

#ifdef SPARK_PLATFORM_WINDOWS
        closesocket(listenSocket);
#else
        close(listenSocket);
#endif
        return servers;
    }

    // ============================================================================
    // Console / Logging
    // ============================================================================

    std::string DedicatedServer::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Dedicated Server Status ===\n";
        oss << "Name:       " << m_config.serverName << "\n";
        oss << "Running:    " << (m_running.load(std::memory_order_acquire) ? "YES" : "NO") << "\n";
        oss << "Port:       " << m_config.port << "\n";
        oss << "Players:    " << m_stats.currentPlayers << "/" << m_config.maxClients
            << " (peak: " << m_stats.peakPlayers << ")\n";
        oss << "Map:        " << m_currentMap << "\n";
        oss << "Game Mode:  " << static_cast<int>(m_config.gameMode) << "\n";
        oss << "Match:      " << (m_matchInProgress ? "In Progress" : "Not Active") << "\n";
        if (m_matchInProgress)
        {
            int mins = static_cast<int>(m_matchTimeRemaining) / 60;
            int secs = static_cast<int>(m_matchTimeRemaining) % 60;
            oss << "Time Left:  " << mins << ":" << std::setw(2) << std::setfill('0') << secs << "\n";
            oss << "Round:      " << m_currentRound << "/" << m_config.roundCount << "\n";
        }
        oss << "Tick Rate:  " << std::fixed << std::setprecision(1) << m_stats.currentTickRate << " Hz\n";
        oss << "Avg Tick:   " << std::fixed << std::setprecision(2) << m_stats.averageTickMs << " ms\n";
        oss << "Peak Tick:  " << std::fixed << std::setprecision(2) << m_stats.peakTickMs << " ms\n";
        oss << "Uptime:     " << static_cast<int>(m_stats.uptimeSeconds) << "s\n";
        oss << "Total Ticks:" << m_stats.totalTicksProcessed << "\n";
        oss << "Connections:" << m_stats.totalConnectionsServed << "\n";
        oss << "LAN Bcast:  " << (m_lanBroadcastActive.load(std::memory_order_acquire) ? "ON" : "OFF") << "\n";
        oss << "RCON:       " << (m_config.rconPassword.empty() ? "DISABLED" : "ENABLED") << "\n";
        return oss.str();
    }

    void DedicatedServer::Log(const std::string& message)
    {
        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        std::tm timeInfo{};
#ifdef SPARK_PLATFORM_WINDOWS
        localtime_s(&timeInfo, &timeT);
#else
        localtime_r(&timeT, &timeInfo);
#endif

        std::ostringstream oss;
        oss << "[" << std::put_time(&timeInfo, "%H:%M:%S") << "] " << message;
        std::string formatted = oss.str();

        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            if (m_config.enableLogging && !m_config.logFilePath.empty())
            {
                std::ofstream logFile(m_config.logFilePath, std::ios::app);
                if (logFile.is_open())
                {
                    logFile << formatted << "\n";
                }
            }
        }

        if (m_callbacks.onLogMessage)
            m_callbacks.onLogMessage(formatted);
    }

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
