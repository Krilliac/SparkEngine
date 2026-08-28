/**
 * @file RemoteDebugSystem.h
 * @brief Remote debugging and live link between editor and running game
 * @author Spark Engine Team
 * @date 2026
 *
 * Bidirectional command channel for inspecting/modifying a running game from
 * the editor.  This header provides queue and in-process loopback plumbing;
 * it does not implement a socket listener or credential protocol. Raw
 * transport calls reach dispatch anonymously and are denied until a future
 * authenticated transport adapter is implemented. For testing, EnableLoopback()
 * uses a server-owned local grant through private queues.
 */

#pragma once

#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/RemoteDebug/RemoteDebugAccessControl.h"
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
            // A public/raw queue call has no authenticated principal.  It is
            // retained for API compatibility but deliberately reaches server
            // dispatch as anonymous and therefore fails closed.
            EnqueueReceivedWithPrincipal(c, RemoteDebugPrincipal{});
        }

        /** @brief Drain one received command; returns false if empty */
        bool DequeueReceived(RemoteCommand& out)
        {
            std::lock_guard lk(m_mtx);
            if (m_recvQ.empty())
                return false;
            out = std::move(m_recvQ.front().command);
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
        friend class RemoteDebugServer;
        friend class RemoteDebugSystem;
#if defined(SPARK_REMOTE_DEBUG_TESTING)
        friend class RemoteDebugAccessControlTestHarness;
#endif

        struct InboundCommand
        {
            RemoteCommand command;
            RemoteDebugPrincipal principal;
        };

        void EnqueueReceivedWithPrincipal(const RemoteCommand& command, const RemoteDebugPrincipal& principal)
        {
            std::lock_guard lk(m_mtx);
            m_recvQ.push({command, principal});
        }

        bool DequeueReceivedWithPrincipal(RemoteCommand& command, RemoteDebugPrincipal& principal)
        {
            std::lock_guard lk(m_mtx);
            if (m_recvQ.empty())
                return false;
            command = std::move(m_recvQ.front().command);
            principal = std::move(m_recvQ.front().principal);
            m_recvQ.pop();
            return true;
        }

        mutable std::mutex m_mtx;
        SessionState m_state{SessionState::Disconnected};
        std::string m_address;
        std::string m_name;
        uint16_t m_port{0};
        float m_uptime{0.0f};
        float m_pingMs{0.0f};
        std::queue<RemoteCommand> m_sendQ;
        std::queue<InboundCommand> m_recvQ;
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

    /** @brief Owns logical server state and dispatches authenticated incoming commands. */
    class RemoteDebugServer
    {
      public:
        /**
     * @brief Enter the logical listen state for a future authenticated transport.
     * @param port Configured port for an external adapter (default 9090).
     * @return True if the logical listen state was entered; this does not bind a socket.
     */
        bool StartListening(uint16_t port = 9090)
        {
            // A restart starts a new authority epoch. Preserve the monotonically
            // increasing grant id in the access-control object so a stale copied
            // principal cannot become valid again after reset.
            m_accessControl.Reset();
            m_session.SetPort(port);
            m_session.SetState(SessionState::Listening);
            RegisterBuiltinHandlers();
            SPARK_LOG_INFO(Spark::LogCategory::Network,
                           "RemoteDebugServer: logical listen state configured for port %d (no transport adapter)", port);
            return true;
        }

        /** @brief Stop listening and disconnect */
        void StopListening()
        {
            m_session.Reset();
            m_accessControl.Reset();
        }

        /** @brief Process queued commands and send responses (call per frame) */
        void Update()
        {
            RemoteCommand cmd;
            RemoteDebugPrincipal principal;
            while (m_session.DequeueReceivedWithPrincipal(cmd, principal))
            {
                RemoteCommand resp = ProcessCommandWithPrincipal(cmd, principal);
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
            // This public entry point deliberately has no authenticated
            // principal. Keeping it for source compatibility must not turn a
            // direct call into a bypass around the server-owned dispatch path.
            return ProcessCommandWithPrincipal(cmd, RemoteDebugPrincipal{});
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
            // Existing callers retain their registration API. A custom handler
            // defaults to the most restrictive command capability because its
            // side effects are not knowable to the RemoteDebug framework.
            RegisterCommandHandler(type, RemoteDebugCapability::ExecuteConsole, std::move(handler));
        }

        /**
     * @brief Register a handler with the capability required at dispatch time.
     * @param type Command type string.
     * @param requiredCapability Least privilege capability needed by handler.
     * @param handler Callback returning a response.
     */
        void RegisterCommandHandler(const std::string& type, RemoteDebugCapability requiredCapability,
                                    CommandHandler handler)
        {
            m_handlers[type] = std::move(handler);
            // A custom handler cannot silently become available to every
            // authenticated role. Callers that have no narrower capability
            // declaration get the conservative execution capability.
            m_handlerCapabilities[type] = requiredCapability == RemoteDebugCapability::None
                                              ? RemoteDebugCapability::ExecuteConsole
                                              : requiredCapability;
        }

        RemoteSession& GetSession() { return m_session; }             ///< @brief Access session
        const RemoteSession& GetSession() const { return m_session; } ///< @brief Access session (const)

        /** @brief Return bounded, credential-free dispatch audit events. */
        [[nodiscard]] std::vector<RemoteDebugAuditEvent> GetAuditEvents() const
        {
            return m_accessControl.GetAuditEvents();
        }

      private:
        friend class RemoteDebugSystem;
#if defined(SPARK_REMOTE_DEBUG_TESTING)
        friend class RemoteDebugAccessControlTestHarness;
#endif

        [[nodiscard]] RemoteDebugPrincipal IssueTrustedLoopbackPrincipal(RemoteDebugRole role,
                                                                           uint64_t lifetimeMilliseconds)
        {
            return m_accessControl.IssueTrustedLoopbackPrincipal(role, lifetimeMilliseconds);
        }

        [[nodiscard]] static RemoteCommand AccessDeniedResponse(const RemoteCommand& cmd)
        {
            // Deliberately avoid distinguishing anonymous, expired, replayed,
            // rate-limited, or under-privileged callers on the public channel.
            // Detailed disposition stays in the bounded server audit trail.
            return {"error", R"({"error":"access_denied"})", cmd.requestId, 0.0f};
        }

        [[nodiscard]] RemoteCommand ProcessCommandWithPrincipal(const RemoteCommand& cmd,
                                                                  const RemoteDebugPrincipal& principal)
        {
            const auto handlerIt = m_handlers.find(cmd.type);
            if (handlerIt == m_handlers.end())
            {
                // An authenticated caller can receive the historical
                // unknown-command response. Raw callers remain denied before
                // type details are exposed.
                const auto authorization = m_accessControl.Authorize(
                    principal, cmd.type, cmd.requestId, cmd.payload.size(), RemoteDebugCapability::Inspect);
                if (!authorization.allowed)
                    return AccessDeniedResponse(cmd);

                m_accessControl.RecordOutcome(principal, cmd.type, cmd.requestId,
                                              RemoteDebugAuditDecision::UnknownCommandDenied);
                return {"error", "{\"error\":\"unknown_command\",\"type\":\"" + EscapeJson(cmd.type) + "\"}",
                        cmd.requestId, 0.0f};
            }

            const auto capabilityIt = m_handlerCapabilities.find(cmd.type);
            const RemoteDebugCapability requiredCapability =
                capabilityIt == m_handlerCapabilities.end() ? RemoteDebugCapability::ExecuteConsole : capabilityIt->second;
            const auto authorization =
                m_accessControl.Authorize(principal, cmd.type, cmd.requestId, cmd.payload.size(), requiredCapability);
            if (!authorization.allowed)
                return AccessDeniedResponse(cmd);

            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "RemoteDebugServer: processing command type='%s'",
                            cmd.type.c_str());
            RemoteCommand response = handlerIt->second(cmd);
            m_accessControl.RecordOutcome(principal, cmd.type, cmd.requestId, RemoteDebugAuditDecision::Allowed);
            return response;
        }

        void RegisterBuiltinHandlers()
        {
            if (m_builtinsRegistered)
                return;
            m_builtinsRegistered = true;

            RegisterCommandHandler("console_cmd", RemoteDebugCapability::ExecuteConsole, [](const RemoteCommand& c)
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
            });
            RegisterCommandHandler("property_get", RemoteDebugCapability::Inspect, [](const RemoteCommand& c)
            {
                std::string payload = "{\"path\":\"" + EscapeJson(c.payload) + "\",\"value\":null}";
                return RemoteCommand{"property_value", payload, c.requestId, 0.0f};
            });
            RegisterCommandHandler("property_set", RemoteDebugCapability::ModifyProperties, [](const RemoteCommand& c)
            { return RemoteCommand{"property_set_result", R"({"status":"ok"})", c.requestId, 0.0f}; });
            RegisterCommandHandler("profile_data", RemoteDebugCapability::Inspect, [](const RemoteCommand& c) {
                return RemoteCommand{"profile_data", R"({"fps":0,"cpuMs":0,"gpuMs":0,"memoryMB":0})", c.requestId,
                                     0.0f};
            });
            RegisterCommandHandler("heartbeat", RemoteDebugCapability::Inspect, [](const RemoteCommand& c)
            { return RemoteCommand{"heartbeat", R"({"status":"alive"})", c.requestId, 0.0f}; });
        }

        RemoteSession m_session;
        std::unordered_map<std::string, CommandHandler> m_handlers;
        std::unordered_map<std::string, RemoteDebugCapability> m_handlerCapabilities;
        RemoteDebugAccessControl m_accessControl;
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
     * @brief Record an intent to connect to a game instance.
     * @param address Hostname or IP of the target.
     * @param port    Port configured by a future transport adapter.
     * @return True after recording intent; no network handshake is implemented here.
     */
        bool Connect(const std::string& address, uint16_t port = 9090)
        {
            m_session.SetAddress(address);
            m_session.SetPort(port);
            m_session.SetName("Editor@" + address);
            m_session.SetState(SessionState::Connecting);
            return true; // A future authenticated transport begins its handshake here.
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
            m_loopbackPrincipal.reset();
            if (m_server)
                m_server->StopListening();
            if (m_client)
                m_client->Disconnect();
            m_server.reset();
            m_client.reset();
            m_initialized = false;
        }

        /**
     * @brief Enter logical server state for a future authenticated transport.
     * @param port Configured port for that future adapter (default 9090).
     */
        bool StartServer(uint16_t port = 9090)
        {
            if (!m_initialized || !m_server)
                return false;
            // Starting an external transport epoch must not retain the local
            // loopback authority from a prior test/debug session.
            m_loopbackEnabled = false;
            m_loopbackPrincipal.reset();
            return m_server->StartListening(port);
        }

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
            if (!m_initialized || !m_server || !m_client)
                return;
            m_server->StartListening(0);
            // The grant is minted and retained by the server/system plumbing;
            // it is never stored in a RemoteCommand or exposed by client APIs.
            m_loopbackPrincipal = m_server->IssueTrustedLoopbackPrincipal(
                RemoteDebugRole::Administrator, RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);
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
                    s += " | server: logical-listen:" + std::to_string(m_server->GetSession().GetPort()) +
                         " (no transport)";
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
            {
                if (m_loopbackPrincipal)
                    m_server->GetSession().EnqueueReceivedWithPrincipal(cmd, *m_loopbackPrincipal);
                else
                    m_server->GetSession().EnqueueReceived(cmd);
            }
            while (m_server->GetSession().DequeuePendingSend(cmd))
                m_client->GetSession().EnqueueReceived(cmd);
        }

        std::unique_ptr<RemoteDebugServer> m_server;
        std::unique_ptr<RemoteDebugClient> m_client;
        std::optional<RemoteDebugPrincipal> m_loopbackPrincipal;
        bool m_initialized{false};
        bool m_loopbackEnabled{false};
    };

} // namespace Spark::RemoteDebug
