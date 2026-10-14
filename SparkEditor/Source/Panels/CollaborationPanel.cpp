/**
 * @file CollaborationPanel.cpp
 * @brief Editor panel for collaborative editing session management
 */

#include "CollaborationPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <sstream>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    CollaborationPanel::CollaborationPanel(CollaborativeEditSession* collabSession)
        : EditorPanel("Collaboration", "Collaboration"), m_collabSession(collabSession)
    {
    }

    bool CollaborationPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "CollaborationPanel initialized");
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
            Spark::Ui::StatusText(Spark::Ui::Status::Success, "%s",
                                  m_collabSession->IsStandaloneBroker()
                                      ? "Connected to standalone broker"
                                      : (isHost ? "Hosting session" : "Connected to session"));

            if (Spark::Ui::BeginPropertyGrid("CollabConnection"))
            {
                if (m_collabSession->IsStandaloneBroker())
                {
                    Spark::Ui::PropertyLabel("Broker");
                    Spark::Ui::MonoText("%s", m_collabSession->GetHostAddress().c_str());
                    Spark::Ui::PropertyLabel("Session");
                    Spark::Ui::MonoText("%s", m_collabSession->GetStandaloneSessionId().c_str());
                }
                else if (isHost)
                {
                    Spark::Ui::PropertyLabel("Port");
                    Spark::Ui::MonoText("%u", m_collabSession->GetPort());

                    // Collaborators need this code to join; share it out of band.
                    Spark::Ui::PropertyLabel("Join code");
                    Spark::Ui::MonoText("%s", m_collabSession->GetJoinCode().c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_COPY " Copy"))
                        ImGui::SetClipboardText(m_collabSession->GetJoinCode().c_str());
                }
                else
                {
                    Spark::Ui::PropertyLabel("Host");
                    Spark::Ui::MonoText("%s:%u", m_collabSession->GetHostAddress().c_str(), m_collabSession->GetPort());
                }
                Spark::Ui::EndPropertyGrid();
            }

            if (Spark::Ui::DangerButton("Disconnect"))
            {
                m_collabSession->Disconnect();
                m_statusMessage = "Disconnected.";
            }
        }
        else
        {
            if (Spark::Ui::BeginPropertyGrid("CollabIdentity"))
            {
                Spark::Ui::PropertyLabel("User Name");
                ImGui::InputText("##UserName", m_userNameBuffer, sizeof(m_userNameBuffer));
                Spark::Ui::PropertyLabel("Transport");
                ImGui::Checkbox("Use standalone SparkCollabServer", &m_useStandaloneBroker);
                Spark::Ui::EndPropertyGrid();
            }

            if (m_useStandaloneBroker)
            {
                Spark::Ui::SubHeader("Standalone broker");
                if (Spark::Ui::BeginPropertyGrid("CollabBroker"))
                {
                    Spark::Ui::PropertyLabel("Endpoint");
                    ImGui::InputText("##BrokerEndpoint", m_brokerEndpointBuffer, sizeof(m_brokerEndpointBuffer));
                    Spark::Ui::PropertyLabel("Session");
                    ImGui::InputText("##BrokerSession", m_brokerSessionBuffer, sizeof(m_brokerSessionBuffer));
                    Spark::Ui::EndPropertyGrid();
                }

                if (Spark::Ui::PrimaryButton("Create Broker Session"))
                {
                    if (m_collabSession && m_collabSession->HostStandaloneBroker(
                                               m_brokerEndpointBuffer, m_brokerSessionBuffer, m_userNameBuffer))
                    {
                        m_statusMessage = "Created broker session '" + std::string(m_brokerSessionBuffer) + "'.";
                    }
                    else
                    {
                        m_statusMessage = "Failed to create broker session. Is SparkCollabServer running?";
                    }
                }
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Join Broker Session"))
                {
                    if (m_collabSession && m_collabSession->ConnectStandaloneBroker(
                                               m_brokerEndpointBuffer, m_brokerSessionBuffer, m_userNameBuffer))
                    {
                        m_statusMessage = "Joined broker session '" + std::string(m_brokerSessionBuffer) + "'.";
                    }
                    else
                    {
                        m_statusMessage = "Failed to join broker session.";
                    }
                }
            }
            else
            {
                Spark::Ui::SubHeader("Peer session");
                if (Spark::Ui::BeginPropertyGrid("CollabPeer"))
                {
                    Spark::Ui::PropertyLabel("Port");
                    ImGui::InputInt("##Port", &m_portValue, 0, 0);
                    Spark::Ui::PropertyLabel("Address", "Host address to join; not used when hosting.");
                    ImGui::InputText("##Address", m_hostAddressBuffer, sizeof(m_hostAddressBuffer));
                    Spark::Ui::PropertyLabel("Join code", "The code the host shares with collaborators.");
                    ImGui::InputText("##JoinCode", m_joinCodeBuffer, sizeof(m_joinCodeBuffer),
                                     ImGuiInputTextFlags_Password);
                    Spark::Ui::EndPropertyGrid();
                }

                if (Spark::Ui::PrimaryButton("Host Peer Session"))
                {
                    const auto port = static_cast<uint16_t>(m_portValue);
                    if (m_collabSession && m_collabSession->Host(port, m_userNameBuffer))
                        m_statusMessage = "Hosting peer session on port " + std::to_string(port) + ".";
                    else
                        m_statusMessage = "Failed to host peer session.";
                }
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Join Peer Session"))
                {
                    const auto port = static_cast<uint16_t>(m_portValue);
                    if (m_collabSession &&
                        m_collabSession->Connect(m_hostAddressBuffer, port, m_userNameBuffer, m_joinCodeBuffer))
                    {
                        m_statusMessage = "Connected to peer at " + std::string(m_hostAddressBuffer) + ".";
                    }
                    else
                        m_statusMessage = "Failed to join peer session.";
                }
            }
        }

        if (!m_statusMessage.empty())
        {
            const Spark::Ui::Status status =
                m_statusMessage.starts_with("Failed") ? Spark::Ui::Status::Error : Spark::Ui::Status::Info;
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::StatusText(status, "%s", m_statusMessage.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    void CollaborationPanel::RenderPeerList()
    {
        if (!m_collabSession)
            return;

        auto peers = m_collabSession->GetConnectedPeers();

        if (Spark::Ui::SectionHeader("Connected Peers", ICON_FA_USERS))
        {
            for (const auto& peer : peers)
            {
                // The peer's assigned colour identifies them across viewport highlights; the name is always shown.
                ImVec4 col(peer.color.r, peer.color.g, peer.color.b, peer.color.a);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::BulletText("%s", peer.userName.c_str());
                ImGui::PopStyleColor();

                ImGui::SameLine();
                bool isLocal = (peer.id == m_collabSession->GetLocalPeerID());
                if (isLocal)
                {
                    Spark::Ui::SecondaryText("(you)");
                }
                else
                {
                    if (!peer.selectedNode.empty())
                    {
                        Spark::Ui::SecondaryText("editing: %s", peer.selectedNode.c_str());
                    }
                    else
                    {
                        Spark::Ui::SecondaryText("idle");
                    }
                }
            }

            if (peers.empty())
            {
                Spark::Ui::EmptyState(ICON_FA_USERS, "No peers connected.",
                                      "Share the join code or broker session so collaborators can join.");
            }
        }
    }

    void CollaborationPanel::RenderLockList()
    {
        if (!m_collabSession)
            return;

        auto locks = m_collabSession->GetAllLocks();

        if (Spark::Ui::SectionHeader("Active Locks", ICON_FA_LOCK))
        {
            if (locks.empty())
            {
                Spark::Ui::SecondaryText("No active locks.");
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
                    Spark::Ui::SecondaryText("by %s (%.0fs / %.0fs)", lock.ownerName.c_str(), elapsed,
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
        if (Spark::Ui::SectionHeader("Recent Edits", ICON_FA_HISTORY, false))
        {
            // Received edits carry only a type and a peer-local node id, and nothing
            // applies them yet: say so rather than implying scenes stay in sync.
            Spark::Ui::StatusText(Spark::Ui::Status::Info,
                                  "Activity log only: peers' edits are not applied to your scene.");
            if (m_editLog.empty())
            {
                Spark::Ui::SecondaryText("No edits recorded.");
            }
            else
            {
                // Show most recent first
                for (auto it = m_editLog.rbegin(); it != m_editLog.rend(); ++it)
                {
                    Spark::Ui::MonoText("[%.1fs]", it->timestamp);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(it->description.c_str());
                }
            }
        }
    }

    void CollaborationPanel::RenderSessionStats()
    {
        if (!m_collabSession)
            return;

        auto stats = m_collabSession->GetStats();

        if (Spark::Ui::SectionHeader("Session Statistics", ICON_FA_CHART_BAR, false) &&
            Spark::Ui::BeginPropertyGrid("CollabStats"))
        {
            Spark::Ui::PropertyLabel("Peers");
            Spark::Ui::MonoText("%u", stats.peerCount);
            Spark::Ui::PropertyLabel("Active Locks");
            Spark::Ui::MonoText("%u", stats.activeLocks);
            Spark::Ui::PropertyLabel("Edits Sent");
            Spark::Ui::MonoText("%u", stats.editsBroadcast);
            Spark::Ui::PropertyLabel("Edits Received");
            Spark::Ui::MonoText("%u", stats.editsReceived);
            Spark::Ui::PropertyLabel("Session Duration");
            Spark::Ui::MonoText("%.0fs", stats.sessionDuration);
            Spark::Ui::EndPropertyGrid();
        }
    }

} // namespace SparkEditor
