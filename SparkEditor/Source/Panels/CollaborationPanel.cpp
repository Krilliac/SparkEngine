/**
 * @file CollaborationPanel.cpp
 * @brief Editor panel for collaborative editing session management
 */

#include "CollaborationPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <sstream>

namespace SparkEditor
{

    CollaborationPanel::CollaborationPanel(CollaborativeEditSession* collabSession)
        : EditorPanel("Collaboration", "Collaboration"), m_collabSession(collabSession)
    {
    }

    bool CollaborationPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        m_isInitialized = true;

        // Wire up the edit callback to capture edits for the log
        if (m_collabSession)
        {
            m_collabSession->SetEditReceivedCallback(
                [this](const EditMessage& edit)
                {
                    std::ostringstream oss;
                    const char* typeNames[] = {"NodeAdded",        "NodeRemoved",      "NodeModified",
                                               "NodeMoved",        "NodeRenamed",      "ComponentAdded",
                                               "ComponentRemoved", "ComponentModified"};
                    auto idx = static_cast<size_t>(edit.type);
                    oss << (idx < 8 ? typeNames[idx] : "Unknown");
                    oss << " " << edit.nodeId;
                    if (!edit.propertyName.empty())
                        oss << "." << edit.propertyName;

                    EditLogEntry entry;
                    entry.description = oss.str();
                    entry.timestamp = static_cast<float>(edit.timestamp) / 1000.0f;

                    if (m_editLog.size() >= MAX_EDIT_LOG)
                        m_editLog.erase(m_editLog.begin());
                    m_editLog.push_back(std::move(entry));
                });
        }

        return true;
    }

    void CollaborationPanel::Update(float /*deltaTime*/) {}

    void CollaborationPanel::Render()
    {
        if (!m_isVisible)
            return;

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderConnectionControls();
        ImGui::Separator();

        if (m_collabSession && m_collabSession->IsConnected())
        {
            RenderPeerList();
            ImGui::Separator();
            RenderLockList();
            ImGui::Separator();
            RenderEditLog();
            ImGui::Separator();
            RenderSessionStats();
        }

        EndPanel();
    }

    void CollaborationPanel::Shutdown() {}

    void CollaborationPanel::RenderConnectionControls()
    {
        bool isConnected = m_collabSession && m_collabSession->IsConnected();

        if (isConnected)
        {
            bool isHost = m_collabSession->IsHost();
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "%s",
                               isHost ? "Hosting session" : "Connected to session");

            if (isHost)
            {
                ImGui::SameLine();
                ImGui::Text("on port %u", m_collabSession->GetPort());
            }
            else
            {
                ImGui::SameLine();
                ImGui::Text("at %s:%u", m_collabSession->GetHostAddress().c_str(), m_collabSession->GetPort());
            }

            if (ImGui::Button("Disconnect"))
            {
                m_collabSession->Disconnect();
                m_statusMessage = "Disconnected.";
            }
        }
        else
        {
            ImGui::Text("User Name:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputText("##UserName", m_userNameBuffer, sizeof(m_userNameBuffer));

            ImGui::Text("Port:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("##Port", &m_portValue, 0, 0);

            if (ImGui::Button("Host Session"))
            {
                if (m_collabSession)
                {
                    auto port = static_cast<uint16_t>(m_portValue);
                    if (m_collabSession->Host(port, m_userNameBuffer))
                    {
                        m_statusMessage = "Hosting on port " + std::to_string(port);
                    }
                    else
                    {
                        m_statusMessage = "Failed to host session.";
                    }
                }
            }

            ImGui::SameLine();
            ImGui::Text("or");
            ImGui::SameLine();

            ImGui::Text("Address:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputText("##Address", m_hostAddressBuffer, sizeof(m_hostAddressBuffer));

            ImGui::SameLine();
            if (ImGui::Button("Join Session"))
            {
                if (m_collabSession)
                {
                    auto port = static_cast<uint16_t>(m_portValue);
                    if (m_collabSession->Connect(m_hostAddressBuffer, port, m_userNameBuffer))
                    {
                        m_statusMessage = "Connected to " + std::string(m_hostAddressBuffer);
                    }
                    else
                    {
                        m_statusMessage = "Failed to connect.";
                    }
                }
            }
        }

        if (!m_statusMessage.empty())
        {
            ImGui::TextWrapped("%s", m_statusMessage.c_str());
        }
    }

    void CollaborationPanel::RenderPeerList()
    {
        if (!m_collabSession)
            return;

        auto peers = m_collabSession->GetConnectedPeers();

        if (ImGui::CollapsingHeader("Connected Peers", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (const auto& peer : peers)
            {
                ImVec4 col(peer.color.r, peer.color.g, peer.color.b, peer.color.a);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::BulletText("%s", peer.userName.c_str());
                ImGui::PopStyleColor();

                ImGui::SameLine();
                bool isLocal = (peer.id == m_collabSession->GetLocalPeerID());
                if (isLocal)
                {
                    ImGui::TextDisabled("(you)");
                }
                else
                {
                    if (!peer.selectedNode.empty())
                    {
                        ImGui::TextDisabled("editing: %s", peer.selectedNode.c_str());
                    }
                    else
                    {
                        ImGui::TextDisabled("idle");
                    }
                }
            }

            if (peers.empty())
            {
                ImGui::TextDisabled("No peers connected.");
            }
        }
    }

    void CollaborationPanel::RenderLockList()
    {
        if (!m_collabSession)
            return;

        auto locks = m_collabSession->GetAllLocks();

        if (ImGui::CollapsingHeader("Active Locks", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (locks.empty())
            {
                ImGui::TextDisabled("No active locks.");
            }
            else
            {
                for (const auto& lock : locks)
                {
                    bool isLocal = (lock.ownerPeer == m_collabSession->GetLocalPeerID());
                    auto elapsed =
                        std::chrono::duration<float>(std::chrono::steady_clock::now() - lock.lockTime).count();

                    ImGui::BulletText("%s", lock.nodeId.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("by %s (%.0fs / %.0fs)", lock.ownerName.c_str(), elapsed,
                                        lock.maxDurationSeconds);

                    if (isLocal)
                    {
                        ImGui::SameLine();
                        std::string btnLabel = "Release##" + lock.nodeId;
                        if (ImGui::SmallButton(btnLabel.c_str()))
                        {
                            m_collabSession->ReleaseLock(lock.nodeId);
                        }
                    }
                }
            }
        }
    }

    void CollaborationPanel::RenderEditLog()
    {
        if (ImGui::CollapsingHeader("Recent Edits"))
        {
            if (m_editLog.empty())
            {
                ImGui::TextDisabled("No edits recorded.");
            }
            else
            {
                // Show most recent first
                for (auto it = m_editLog.rbegin(); it != m_editLog.rend(); ++it)
                {
                    ImGui::TextDisabled("[%.1fs]", it->timestamp);
                    ImGui::SameLine();
                    ImGui::Text("%s", it->description.c_str());
                }
            }
        }
    }

    void CollaborationPanel::RenderSessionStats()
    {
        if (!m_collabSession)
            return;

        auto stats = m_collabSession->GetStats();

        if (ImGui::CollapsingHeader("Session Statistics"))
        {
            ImGui::Text("Peers: %u", stats.peerCount);
            ImGui::Text("Active Locks: %u", stats.activeLocks);
            ImGui::Text("Edits Sent: %u", stats.editsBroadcast);
            ImGui::Text("Edits Received: %u", stats.editsReceived);
            ImGui::Text("Session Duration: %.0fs", stats.sessionDuration);
        }
    }

} // namespace SparkEditor
