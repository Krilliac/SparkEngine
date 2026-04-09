/**
 * @file RemoteDebugSystem.h
 * @brief Remote debugging and live link between editor and running game
 * @author Spark Engine Team
 * @date 2026
 *
 * Bidirectional command channel for inspecting/modifying a running game from
 * the editor.  Transport is abstracted: a thin TCP socket adapter plugs into
 * RemoteSession::EnqueueReceived() and DequeuePendingSend().  For testing,
 * EnableLoopback() connects server and client through shared queues.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

namespace Spark::RemoteDebug
{

    // ============================================================================
    // RemoteCommand — wire message between editor and game
    // ============================================================================

    /** @brief A command/response message exchanged over the remote debug link. */
    struct RemoteCommand
    {
        std::string type;      ///< "property_get", "property_set", "console_cmd",
                               ///< "profile_data", "log", "heartbeat"
        std::string payload;   ///< JSON-formatted body (schema depends on type)
        uint32_t requestId{0}; ///< Caller-assigned ID to correlate responses
        float timestamp{0.0f}; ///< Time the command was created (seconds)
    };

    // ============================================================================
    // RemoteSession — connection state and thread-safe message queues
    // ============================================================================

    /** @brief Connection lifecycle states */
    enum class SessionState
    {
        Disconnected,
        Listening,
        Connecting,
        Connected
    };

    /** @brief Mutable state of a single remote debug connection (thread-safe queues). */
    class RemoteSession
    {
      public:
        SessionState GetState() const
        {
            std::lock_guard lk(m_mtx);
            return m_state;
        } ///< @brief Current state
        void SetState(SessionState s)
        {
            std::lock_guard lk(m_mtx);
            m_state = s;
        } ///< @brief Set state

        void SetAddress(const std::string& a) { m_address = a; }    ///< @brief Set target address
        const std::string& GetAddress() const { return m_address; } ///< @brief Get target address
        void SetPort(uint16_t p) { m_port = p; }                    ///< @brief Set target port
        uint16_t GetPort() const { return m_port; }                 ///< @brief Get target port
        void SetName(const std::string& n) { m_name = n; }          ///< @brief Set session name
        const std::string& GetName() const { return m_name; }       ///< @brief Get session name

        float GetUptime() const { return m_uptime; } ///< @brief Uptime in seconds
        void AddUptime(float dt) { m_uptime += dt; } ///< @brief Accumulate uptime
        float GetPing() const { return m_pingMs; }   ///< @brief Smoothed RTT (ms)
        void SetPing(float ms) { m_pingMs = ms; }    ///< @brief Update ping

        /** @brief Queue a command for sending (thread-safe) */
        void EnqueueSend(const RemoteCommand& c)
        {
            std::lock_guard lk(m_mtx);
            m_sendQ.push(c);
        }

        /** @brief Drain one pending send; returns false if empty */
        bool DequeuePendingSend(RemoteCommand& out)
        {
            std::lock_guard lk(m_mtx);
            if (m_sendQ.empty())
                return false;
            out = std::move(m_sendQ.front());
            m_sendQ.pop();
            return true;
        }

        /** @brief Enqueue a received command (called by transport layer) */
        void EnqueueReceived(const RemoteCommand& c)
        {
            std::lock_guard lk(m_mtx);
            m_recvQ.push(c);
        }

        /** @brief Drain one received command; returns false if empty */
        bool DequeueReceived(RemoteCommand& out)
        {
            std::lock_guard lk(m_mtx);
            if (m_recvQ.empty())
                return false;
            out = std::move(m_recvQ.front());
            m_recvQ.pop();
            return true;
        }

        /** @brief True if commands are waiting */
        bool HasReceivedCommands() const
        {
            std::lock_guard lk(m_mtx);
            return !m_recvQ.empty();
        }

        /** @brief Reset all state to defaults */
        void Reset()
        {
            std::lock_guard lk(m_mtx);
            m_state = SessionState::Disconnected;
            m_address.clear();
            m_name.clear();
            m_port = 0;
            m_uptime = 0.0f;
            m_pingMs = 0.0f;
            m_sendQ = {};
            m_recvQ = {};
        }

      private:
        mutable std::mutex m_mtx;
        SessionState m_state{SessionState::Disconnected};
        std::string m_address;
        std::string m_name;
        uint16_t m_port{0};
        float m_uptime{0.0f};
        float m_pingMs{0.0f};
        std::queue<RemoteCommand> m_sendQ;
        std::queue<RemoteCommand> m_recvQ;
    };

    // ============================================================================
    // RemoteDebugServer — runs in the game runtime
    // ============================================================================

    /** @brief Escape a string for safe embedding in JSON values */
    inline std::string EscapeJson(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    /** @brief Command handler callback: receives a command, returns a response */
    using CommandHandler = std::function<RemoteCommand(const RemoteCommand&)>;

    /** @brief Accepts an editor connection and dispatches incoming commands. */
    class RemoteDebugServer
    {
      public:
        /**
     * @brief Begin listening for an editor connection.
     * @param port TCP port to bind (default 9090).
     * @return True if the listen state was entered.
     */
        bool StartListening(uint16_t port = 9090)
        {
            m_session.SetPort(port);
            m_session.SetState(SessionState::Listening);
            RegisterBuiltinHandlers();
            SPARK_LOG_INFO(Spark::LogCategory::Network, "RemoteDebugServer: listening on port %d", port);
            return true;
        }

        /** @brief Stop listening and disconnect */
        void StopListening() { m_session.Reset(); }

        /** @brief Process queued commands and send responses (call per frame) */
        void Update()
        {
            RemoteCommand cmd;
            while (m_session.DequeueReceived(cmd))
            {
                RemoteCommand resp = ProcessCommand(cmd);
                if (!resp.type.empty())
                    m_session.EnqueueSend(resp);
            }
        }

        /**
     * @brief Dispatch a command to the matching handler.
     * @param cmd The incoming command.
     * @return Response (empty type if unhandled).
     */
        RemoteCommand ProcessCommand(const RemoteCommand& cmd)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "RemoteDebugServer: processing command type='%s'",
                            cmd.type.c_str());
            auto it = m_handlers.find(cmd.type);
            if (it != m_handlers.end())
                return it->second(cmd);
            return {"error", "{\"error\":\"unknown_command\",\"type\":\"" + EscapeJson(cmd.type) + "\"}", cmd.requestId,
                    0.0f};
        }

        /** @brief Queue a response for the connected editor */
        void SendResponse(const RemoteCommand& cmd) { m_session.EnqueueSend(cmd); }

        /**
     * @brief Register a handler for a command type.
     * @param type    Command type string.
     * @param handler Callback returning a response.
     */
        void RegisterCommandHandler(const std::string& type, CommandHandler handler)
        {
            m_handlers[type] = std::move(handler);
        }

        RemoteSession& GetSession() { return m_session; }             ///< @brief Access session
        const RemoteSession& GetSession() const { return m_session; } ///< @brief Access session (const)

      private:
        void RegisterBuiltinHandlers()
        {
            if (m_builtinsRegistered)
                return;
            m_builtinsRegistered = true;

            m_handlers["console_cmd"] = [](const RemoteCommand& c)
            {
                // Extract the command string from the JSON payload
                std::string command = c.payload;
                // Simple JSON extraction: find "command":"<value>"
                auto cmdPos = command.find("\"command\"");
                if (cmdPos != std::string::npos)
                {
                    auto valStart = command.find('\"', cmdPos + 9);
                    if (valStart != std::string::npos)
                    {
                        auto valEnd = command.find('\"', valStart + 1);
                        if (valEnd != std::string::npos)
                            command = command.substr(valStart + 1, valEnd - valStart - 1);
                    }
                }

                auto& console = Spark::SimpleConsole::GetInstance();
                uint64_t logsBefore = console.GetStats().totalLogsWritten;
                bool success = console.ExecuteCommand(command);

                // Collect output generated by the command
                std::string output;
                auto logs = console.GetLogHistory();
                for (const auto& entry : logs)
                {
                    if (entry.sequenceNumber >= logsBefore)
                    {
                        if (!output.empty())
                            output += '\n';
                        output += entry.message;
                    }
                }

                std::string payload = "{\"status\":\"" + std::string(success ? "ok" : "error") + "\",\"output\":\"" +
                                      EscapeJson(output) + "\"}";
                return RemoteCommand{"console_cmd_result", payload, c.requestId, 0.0f};
            };
            m_handlers["property_get"] = [](const RemoteCommand& c)
            {
                std::string payload = "{\"path\":\"" + EscapeJson(c.payload) + "\",\"value\":null}";
                return RemoteCommand{"property_value", payload, c.requestId, 0.0f};
            };
            m_handlers["property_set"] = [](const RemoteCommand& c)
            { return RemoteCommand{"property_set_result", R"({"status":"ok"})", c.requestId, 0.0f}; };
            m_handlers["profile_data"] = [](const RemoteCommand& c)
            {
                return RemoteCommand{"profile_data", R"({"fps":0,"cpuMs":0,"gpuMs":0,"memoryMB":0})", c.requestId,
                                     0.0f};
            };
            m_handlers["heartbeat"] = [](const RemoteCommand& c)
            { return RemoteCommand{"heartbeat", R"({"status":"alive"})", c.requestId, 0.0f}; };
        }

        RemoteSession m_session;
        std::unordered_map<std::string, CommandHandler> m_handlers;
        bool m_builtinsRegistered{false};
    };

    // ============================================================================
    // RemoteDebugClient — runs in the editor
    // ============================================================================

    /** @brief Connects to a running game and provides convenience debug methods. */
    class RemoteDebugClient
    {
      public:
        /**
     * @brief Initiate a connection to a game instance.
     * @param address Hostname or IP of the target.
     * @param port    Port the server is listening on.
     * @return True if the connection attempt was initiated.
     */
        bool Connect(const std::string& address, uint16_t port = 9090)
        {
            m_session.SetAddress(address);
            m_session.SetPort(port);
            m_session.SetName("Editor@" + address);
            m_session.SetState(SessionState::Connecting);
            return true; // Real transport: begin TCP handshake here
        }

        /** @brief Disconnect from the game instance */
        void Disconnect() { m_session.Reset(); }

        /** @brief Send a command to the connected game */
        void SendCommand(const RemoteCommand& cmd) { m_session.EnqueueSend(cmd); }

        /** @brief Drain all received responses since the last poll */
        std::vector<RemoteCommand> PollResponses()
        {
            std::vector<RemoteCommand> out;
            RemoteCommand cmd;
            while (m_session.DequeueReceived(cmd))
                out.push_back(std::move(cmd));
            return out;
        }

        /**
     * @brief Execute a console command on the remote game.
     * @param command The console command string.
     * @return Request ID for correlating the response.
     */
        uint32_t ExecuteConsoleCommand(const std::string& command)
        {
            uint32_t id = m_nextRequestId++;
            SendCommand({"console_cmd", "{\"command\":\"" + EscapeJson(command) + "\"}", id, 0.0f});
            return id;
        }

        /**
     * @brief Request the value of a named property.
     * @param path Dot-separated property path (e.g. "player.health").
     * @return Request ID for correlating the response.
     */
        uint32_t GetProperty(const std::string& path)
        {
            uint32_t id = m_nextRequestId++;
            SendCommand({"property_get", path, id, 0.0f});
            return id;
        }

        /**
     * @brief Set the value of a named property.
     * @param path  Dot-separated property path.
     * @param value JSON-formatted value string.
     * @return Request ID for correlating the response.
     */
        uint32_t SetProperty(const std::string& path, const std::string& value)
        {
            uint32_t id = m_nextRequestId++;
            SendCommand({"property_set", "{\"path\":\"" + EscapeJson(path) + "\",\"value\":" + value + "}", id, 0.0f});
            return id;
        }

        /**
     * @brief Request a performance snapshot (CPU, GPU, memory).
     * @return Request ID for correlating the response.
     */
        uint32_t RequestPerformanceSnapshot()
        {
            uint32_t id = m_nextRequestId++;
            SendCommand({"profile_data", R"({"request":"snapshot"})", id, 0.0f});
            return id;
        }

        RemoteSession& GetSession() { return m_session; }             ///< @brief Access session
        const RemoteSession& GetSession() const { return m_session; } ///< @brief Access session (const)

      private:
        RemoteSession m_session;
        uint32_t m_nextRequestId{1};
    };

    // ============================================================================
    // RemoteDebugSystem — singleton manager
    // ============================================================================

    /** @brief Top-level singleton owning both server and client instances. */
    class RemoteDebugSystem
    {
      public:
        /** @brief Get the singleton instance */
        static RemoteDebugSystem& GetInstance()
        {
            static RemoteDebugSystem instance;
            return instance;
        }

        /** @brief Initialize the remote debug subsystem. @return True on success. */
        bool Initialize()
        {
            if (m_initialized)
                return true;
            m_server = std::make_unique<RemoteDebugServer>();
            m_client = std::make_unique<RemoteDebugClient>();
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Network, "RemoteDebugSystem initialized");
            return true;
        }

        /** @brief Shut down and release all resources */
        void Shutdown()
        {
            if (!m_initialized)
                return;
            m_loopbackEnabled = false;
            if (m_server)
                m_server->StopListening();
            if (m_client)
                m_client->Disconnect();
            m_server.reset();
            m_client.reset();
            m_initialized = false;
        }

        /**
     * @brief Start the debug server.
     * @param port TCP port to listen on (default 9090).
     */
        bool StartServer(uint16_t port = 9090) { return m_initialized && m_server && m_server->StartListening(port); }

        /**
     * @brief Connect the client to a running game instance.
     * @param address Hostname or IP of the target.
     * @param port    Port the server is listening on.
     */
        bool ConnectToTarget(const std::string& address, uint16_t port = 9090)
        {
            return m_initialized && m_client && m_client->Connect(address, port);
        }

        /** @brief True if either side has an active connection */
        bool IsConnected() const
        {
            if (!m_initialized)
                return false;
            if (m_loopbackEnabled)
                return true;
            return (m_server && m_server->GetSession().GetState() == SessionState::Connected) ||
                   (m_client && m_client->GetSession().GetState() == SessionState::Connected);
        }

        const RemoteSession* GetServerSession() const { return m_server ? &m_server->GetSession() : nullptr; }
        const RemoteSession* GetClientSession() const { return m_client ? &m_client->GetSession() : nullptr; }

        /**
     * @brief Per-frame update: pump loopback, update server and client.
     * @param dt Frame delta time in seconds.
     */
        void Update(float dt)
        {
            if (!m_initialized)
                return;
            if (m_loopbackEnabled)
                PumpLoopback(); // Move client sends → server receives
            if (m_server)
            {
                m_server->GetSession().AddUptime(dt);
                m_server->Update(); // Process received, enqueue responses
            }
            if (m_client)
            {
                m_client->GetSession().AddUptime(dt);
            }
            if (m_loopbackEnabled)
                PumpLoopback(); // Move server responses → client receives
        }

        /**
     * @brief Enable in-process loopback (shared queues, no sockets).
     * @details Client send -> server receive, server send -> client receive.
     */
        void EnableLoopback()
        {
            if (!m_initialized)
                return;
            m_server->StartListening(0);
            m_server->GetSession().SetState(SessionState::Connected);
            m_client->GetSession().SetState(SessionState::Connected);
            m_loopbackEnabled = true;
            SPARK_LOG_INFO(Spark::LogCategory::Network, "RemoteDebugSystem: loopback enabled");
        }

        RemoteDebugServer* GetServer() { return m_server.get(); } ///< @brief Get server (may be null)
        RemoteDebugClient* GetClient() { return m_client.get(); } ///< @brief Get client (may be null)

        /** @brief Console status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "RemoteDebug: not initialized";
            std::string s = "RemoteDebug: initialized";
            if (m_loopbackEnabled)
                s += " [loopback]";
            if (m_server)
            {
                auto st = m_server->GetSession().GetState();
                if (st == SessionState::Listening)
                    s += " | server: listening:" + std::to_string(m_server->GetSession().GetPort());
                else if (st == SessionState::Connected)
                    s += " | server: connected";
                else
                    s += " | server: idle";
            }
            if (m_client)
            {
                auto st = m_client->GetSession().GetState();
                if (st == SessionState::Connecting)
                    s += " | client: connecting to " + m_client->GetSession().GetAddress();
                else if (st == SessionState::Connected)
                    s += " | client: connected (ping " +
                         std::to_string(static_cast<int>(m_client->GetSession().GetPing())) + " ms)";
                else
                    s += " | client: disconnected";
            }
            return s;
        }

      private:
        RemoteDebugSystem() = default;
        ~RemoteDebugSystem() { Shutdown(); }
        RemoteDebugSystem(const RemoteDebugSystem&) = delete;
        RemoteDebugSystem& operator=(const RemoteDebugSystem&) = delete;

        void PumpLoopback()
        {
            RemoteCommand cmd;
            while (m_client->GetSession().DequeuePendingSend(cmd))
                m_server->GetSession().EnqueueReceived(cmd);
            while (m_server->GetSession().DequeuePendingSend(cmd))
                m_client->GetSession().EnqueueReceived(cmd);
        }

        std::unique_ptr<RemoteDebugServer> m_server;
        std::unique_ptr<RemoteDebugClient> m_client;
        bool m_initialized{false};
        bool m_loopbackEnabled{false};
    };

} // namespace Spark::RemoteDebug
