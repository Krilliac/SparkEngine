/**
 * @file LiveEditBridge.h
 * @brief Bridge between collaborative editing and a running AreaServer
 * @author Spark Engine Team
 * @date 2026
 *
 * Enables HeroEngine-style live editing: committed edits from the collaborative
 * session are forwarded to a running AreaServer, which applies them to the live
 * game world and replicates to all connected game clients.
 *
 * The bridge connects as a privileged "editor client" to the game's networking
 * layer using the UserDefined message range (MessageType >= 1000).
 */

#pragma once

#include "CollaborativeEditSession.h"
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace SparkEditor
{

    /// @brief Custom message types for editor→AreaServer communication.
    /// Starts at UserDefined (1000) to avoid collision with game messages.
    enum class EditorNetMessageType : uint16_t
    {
        SceneEdit = 1000,   ///< A scene edit (node/component change)
        LockNotify = 1001,  ///< Lock state change notification
        EditorJoin = 1002,  ///< Editor connecting as privileged client
        EditorLeave = 1003, ///< Editor disconnecting
    };

    /**
     * @brief Forwards collaborative edits to a running AreaServer for live game updates
     *
     * ## Architecture
     * ```
     * [Editor A] ─── CollaborativeEditSession ──── [Editor B]
     *     │                                              │
     *     └──────── LiveEditBridge (TCP) ────────────────┘
     *                      │
     *              [AreaServer:interServerPort]
     *                      │
     *              [Game Clients see changes]
     * ```
     */
    class LiveEditBridge
    {
      public:
        LiveEditBridge();
        ~LiveEditBridge();

        LiveEditBridge(const LiveEditBridge&) = delete;
        LiveEditBridge& operator=(const LiveEditBridge&) = delete;

        /**
         * @brief Connect to a running AreaServer's inter-server port
         * @param address AreaServer IP address
         * @param port AreaServer inter-server port
         * @param editorName Display name for this editor connection
         * @return true if TCP connection succeeded
         */
        bool Connect(const std::string& address, uint16_t port, const std::string& editorName);

        /**
         * @brief Disconnect from the AreaServer
         */
        void Disconnect();

        /**
         * @brief Check if connected to an AreaServer
         */
        bool IsConnected() const { return m_connected.load(std::memory_order_acquire); }

        /**
         * @brief Forward an edit to the AreaServer for live application
         * @param edit The edit to push to the running game
         */
        void PushEdit(const EditMessage& edit);

        /**
         * @brief Process pending outgoing edits (call each frame)
         */
        void Update();

        /**
         * @brief Get the address of the connected AreaServer
         */
        const std::string& GetServerAddress() const { return m_serverAddress; }

        /**
         * @brief Get the port of the connected AreaServer
         */
        uint16_t GetServerPort() const { return m_serverPort; }

        /**
         * @brief Get count of edits pushed to the live server
         */
        uint32_t GetEditsPushed() const { return m_editsPushed; }

      private:
        void SendThread();

        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_shuttingDown{false};
        std::string m_serverAddress;
        uint16_t m_serverPort = 0;
        std::string m_editorName;
        int m_socket = -1;

        std::queue<EditMessage> m_pendingEdits;
        std::mutex m_editMutex;
        std::thread m_sendThread;

        uint32_t m_editsPushed = 0;
    };

} // namespace SparkEditor
