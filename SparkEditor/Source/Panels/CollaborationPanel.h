/**
 * @file CollaborationPanel.h
 * @brief Editor panel for managing collaborative editing sessions
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides UI for hosting/joining collab sessions, viewing connected peers,
 * managing node locks, and monitoring edit activity.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Communication/CollaborativeEditSession.h"
#include <string>

namespace SparkEditor
{

    class CollaborationPanel : public EditorPanel
    {
      public:
        /// @param collabSession Non-owning pointer to the session owned by EditorUI
        explicit CollaborationPanel(CollaborativeEditSession* collabSession);
        ~CollaborationPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        std::string GetTypeName() const override { return "CollaborationPanel"; }

      private:
        void RenderConnectionControls();
        void RenderPeerList();
        void RenderLockList();
        void RenderEditLog();
        void RenderSessionStats();

        CollaborativeEditSession* m_collabSession = nullptr;

        // Connection UI state
        char m_hostAddressBuffer[128] = "127.0.0.1";
        int m_portValue = 27030;
        char m_userNameBuffer[64] = "Editor";
        std::string m_statusMessage;

        // Edit log (ring buffer of recent edits)
        struct EditLogEntry
        {
            std::string description;
            float timestamp = 0.0f;
        };
        std::vector<EditLogEntry> m_editLog;
        static constexpr size_t MAX_EDIT_LOG = 50;
    };

} // namespace SparkEditor
