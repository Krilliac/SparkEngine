/**
 * @file TFSocialPanel.cpp
 * @brief Social window implementation (see TFSocialPanel.h).
 */
#include "UI/TFSocialPanel.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Game/TFSquadSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFSocialProtocol.h"
#include "UI/TFUiCommon.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

    TFSocialPanel::TFSocialPanel() = default;
    TFSocialPanel::~TFSocialPanel()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSocialPanel::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Self-register with the client net layer so its UI-open input
        // suppression covers the panel (TFChatWindow does the same).
        if (ctx.clientNet)
            ctx.clientNet->SetSocialPanel(this);

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_social"))
        {
            console.RegisterCommand(
                "tf_social",
                [this](const std::vector<std::string>&) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] social panel not ready";
                    Toggle();
                    return m_open ? "[TF] social panel opened" : "[TF] social panel closed";
                },
                "Toggle the social panel (friends / recent / blocked / squad); hotkey O", "TERRAFRONT", "tf_social");
            m_socialCmd = true;
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSocialPanel initialized");
        return true;
    }

    void TFSocialPanel::Update(float deltaTime)
    {
        (void)deltaTime;
    }

    void TFSocialPanel::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFSocialPanel::Shutdown()
    {
        if (!m_initialized)
            return;
        if (m_socialCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_social");
            m_socialCmd = false;
        }
        m_open = false;
        m_initialized = false;
    }

    bool TFSocialPanel::LocalPawnAlive() const
    {
        if (!m_ctx || !m_ctx->players || m_ctx->localPlayer == kInvalidPlayer)
            return false;
        PawnInfo pawn{};
        return m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) && pawn.alive;
    }

    void TFSocialPanel::Toggle()
    {
        if (!m_open)
        {
            m_open = true;
            if (m_ctx && m_ctx->engine && m_ctx->engine->GetInput())
            {
                InputManager* input = m_ctx->engine->GetInput();
                m_releasedMouse = input->IsMouseCaptured();
                if (m_releasedMouse)
                    input->CaptureMouse(false);
            }
            // Panel open == fresh data is nice-to-have: ask for a resync.
            if (m_social)
                m_social->ClientSendOp(SocialOp::RequestLists, "");
        }
        else
        {
            m_open = false;
            if (m_releasedMouse && m_ctx && m_ctx->engine && m_ctx->engine->GetInput() && LocalPawnAlive())
                m_ctx->engine->GetInput()->CaptureMouse(true);
            m_releasedMouse = false;
        }
    }

    std::string TFSocialPanel::RosterNameOf(PlayerId player) const
    {
        std::string name;
        if (m_social && m_social->NameOfPlayer(player, name))
            return name;
        if (player == kTFLocalHostPlayer)
            return "HOST";
        char label[16];
        std::snprintf(label, sizeof(label), "p%u", player);
        return label;
    }

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        void OnlineDot(bool online)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float r = ImGui::GetTextLineHeight() * 0.28f;
            const ImVec2 c(pos.x + r + 1.0f, pos.y + ImGui::GetTextLineHeight() * 0.55f);
            dl->AddCircleFilled(c, r, online ? IM_COL32(90, 220, 110, 255) : IM_COL32(110, 110, 110, 255));
            ImGui::Dummy(ImVec2(r * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
        }

    } // namespace

    void TFSocialPanel::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;

        // 'O' toggles (guarded exactly like TFHUD's ENTER-chat: never while a
        // text field owns the keyboard).
        if (!ImGui::GetIO().WantTextInput && !ImGui::GetIO().WantCaptureKeyboard &&
            ImGui::IsKeyPressed(ImGuiKey_O, false))
            Toggle();
        if (m_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput)
            Toggle();

        if (!m_open)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float width = std::min(440.0f, vp->Size.x * 0.45f);
        const float height = std::min(520.0f, vp->Size.y * 0.72f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - width - 24.0f, vp->Pos.y + vp->Size.y * 0.12f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);

        bool open = true;
        if (!ImGui::Begin("Social##TFSocialPanel", &open, ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::End();
            if (!open)
                Toggle();
            return;
        }

        if (ImGui::BeginTabBar("##TFSocialTabs"))
        {
            if (ImGui::BeginTabItem("Friends"))
            {
                DrawFriendsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Recent"))
            {
                DrawRecentTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Blocked"))
            {
                DrawBlockedTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Squad"))
            {
                DrawSquadTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        // Status row: last op feedback from the server.
        if (m_social && !m_social->LastOpFeedback().empty())
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 140, 255));
            ImGui::TextWrapped("%s", m_social->LastOpFeedback().c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
        if (!open)
            Toggle();
    }

    void TFSocialPanel::DrawFriendsTab()
    {
        if (!m_social)
        {
            ImGui::TextUnformatted("social system not wired");
            return;
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputTextWithHint("##TFFriendAdd", "character name", m_friendInput, sizeof(m_friendInput));
        ImGui::SameLine();
        if (ImGui::Button("Add friend") && m_friendInput[0] != '\0')
        {
            m_social->ClientSendOp(SocialOp::FriendAdd, m_friendInput);
            m_friendInput[0] = '\0';
        }
        ImGui::Separator();

        const auto& friends = m_social->Friends();
        if (friends.empty())
            ImGui::TextDisabled("no friends yet — add one by character name");

        int rowId = 0;
        for (const auto& f : friends)
        {
            ImGui::PushID(rowId++);
            OnlineDot(f.online);
            ImGui::PushStyleColor(ImGuiCol_Text, TFUi::FactionCol(f.faction));
            ImGui::TextUnformatted(f.name.c_str());
            ImGui::PopStyleColor();

            if (f.online && m_ctx->squads)
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 118.0f);
                if (ImGui::SmallButton("Invite"))
                {
                    const PlayerId pid = m_social->PlayerIdByName(f.name);
                    if (pid != kInvalidPlayer)
                    {
                        // No squad yet: create one first, then invite next click.
                        const auto view = m_ctx->squads->GetLocalSquadView();
                        if (view.squad == kInvalidSquad)
                            m_ctx->squads->UiSendOp(SquadOp::Create, kInvalidPlayer);
                        m_ctx->squads->UiSendOp(SquadOp::Invite, pid);
                    }
                }
                ImGui::SameLine();
            }
            else
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 62.0f);
            }
            if (ImGui::SmallButton("Remove"))
                m_social->ClientSendOp(SocialOp::FriendRemove, f.name);
            ImGui::PopID();
        }
    }

    void TFSocialPanel::DrawRecentTab()
    {
        if (!m_social)
        {
            ImGui::TextUnformatted("social system not wired");
            return;
        }
        const auto& recent = m_social->Recent();
        if (recent.empty())
            ImGui::TextDisabled("players you fight with or against show up here");

        int rowId = 0;
        for (const auto& r : recent)
        {
            ImGui::PushID(rowId++);
            OnlineDot(r.online);
            ImGui::PushStyleColor(ImGuiCol_Text, TFUi::FactionCol(r.faction));
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 132.0f);
            if (ImGui::SmallButton("Add friend"))
                m_social->ClientSendOp(SocialOp::FriendAdd, r.name);
            ImGui::SameLine();
            if (ImGui::SmallButton("Block"))
                m_social->ClientSendOp(SocialOp::BlockAdd, r.name);
            ImGui::PopID();
        }
    }

    void TFSocialPanel::DrawBlockedTab()
    {
        if (!m_social)
        {
            ImGui::TextUnformatted("social system not wired");
            return;
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputTextWithHint("##TFBlockAdd", "character name", m_blockInput, sizeof(m_blockInput));
        ImGui::SameLine();
        if (ImGui::Button("Block") && m_blockInput[0] != '\0')
        {
            m_social->ClientSendOp(SocialOp::BlockAdd, m_blockInput);
            m_blockInput[0] = '\0';
        }
        ImGui::TextDisabled("blocked players' chat is hidden; blocking removes them from friends");
        ImGui::Separator();

        int rowId = 0;
        for (const auto& b : m_social->Blocked())
        {
            ImGui::PushID(rowId++);
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
            if (ImGui::SmallButton("Unblock"))
                m_social->ClientSendOp(SocialOp::BlockRemove, b.name);
            ImGui::PopID();
        }
    }

    void TFSocialPanel::DrawSquadTab()
    {
        if (!m_ctx->squads)
        {
            ImGui::TextUnformatted("squad system not available");
            return;
        }
        const auto view = m_ctx->squads->GetLocalSquadView();
        const PlayerId me = m_ctx->localPlayer;

        if (view.squad == kInvalidSquad)
        {
            ImGui::TextDisabled("you are not in a squad");
            if (ImGui::Button("Create squad"))
                m_ctx->squads->UiSendOp(SquadOp::Create, kInvalidPlayer);

            if (view.inviteSquad != kInvalidSquad)
            {
                const std::string fromName = RosterNameOf(view.inviteFrom);
                // Respect the block list: don't advertise invites from blocked players.
                if (!m_social || !m_social->IsBlockedName(fromName))
                {
                    ImGui::Separator();
                    ImGui::Text("Squad invite from %s", fromName.c_str());
                    if (ImGui::Button("Accept invite"))
                        m_ctx->squads->UiSendOp(SquadOp::Accept, kInvalidPlayer);
                }
            }
            return;
        }

        const bool isLeader = view.leader == me;
        ImGui::Text("Squad %u  (%zu/%u)", view.squad, view.members.size(), static_cast<unsigned>(kMaxSquadSize));
        ImGui::Separator();

        int rowId = 0;
        for (const PlayerId member : view.members)
        {
            ImGui::PushID(rowId++);
            const std::string name = RosterNameOf(member);
            const FactionId faction = m_social ? m_social->RosterFactionOf(member) : FactionId::None;
            ImGui::PushStyleColor(ImGuiCol_Text, TFUi::FactionCol(faction));
            ImGui::Text("%s %s%s", member == view.leader ? "*" : " ", name.c_str(), member == me ? " (you)" : "");
            ImGui::PopStyleColor();

            if (isLeader && member != me)
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 118.0f);
                if (ImGui::SmallButton("Kick"))
                    m_ctx->squads->UiSendOp(SquadOp::Kick, member);
                ImGui::SameLine();
                if (ImGui::SmallButton("Promote"))
                    m_ctx->squads->UiSendOp(SquadOp::Promote, member);
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("Leave squad"))
            m_ctx->squads->UiSendOp(SquadOp::Leave, kInvalidPlayer);

        if (isLeader)
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
            ImGui::InputTextWithHint("##TFSquadInvite", "invite by name", m_inviteInput, sizeof(m_inviteInput));
            ImGui::SameLine();
            if (ImGui::Button("Invite##ByName") && m_inviteInput[0] != '\0' && m_social)
            {
                const PlayerId pid = m_social->PlayerIdByName(m_inviteInput);
                if (pid != kInvalidPlayer)
                {
                    m_ctx->squads->UiSendOp(SquadOp::Invite, pid);
                    m_inviteInput[0] = '\0';
                }
            }
        }
    }

#else // !SPARK_HAS_IMGUI

    void TFSocialPanel::RenderUI() {}
    void TFSocialPanel::DrawFriendsTab() {}
    void TFSocialPanel::DrawRecentTab() {}
    void TFSocialPanel::DrawBlockedTab() {}
    void TFSocialPanel::DrawSquadTab() {}

#endif // SPARK_HAS_IMGUI

    void TFSocialPanel::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Social Panel"))
            return;
        ImGui::Text("open: %d", m_open ? 1 : 0);
#endif
    }

} // namespace Terrafront
