/**
 * @file TFOutfitPanel.cpp
 * @brief Outfit UI implementation (see header for the embedding contract).
 */
#include "UI/TFOutfitPanel.h"

#include "Game/TFOutfitSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cstring>

namespace Terrafront
{

    TFOutfitPanel::TFOutfitPanel() = default;

    TFOutfitPanel::~TFOutfitPanel()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFOutfitPanel::Initialize(TFGameContext& ctx, TFEventBus& events, TFOutfitSystem& outfits)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_outfits = &outfits;
        m_initialized = true;

        if (!m_cmdRegistered)
        {
            Spark::SimpleConsole::GetInstance().RegisterCommand(
                "tf_outfit",
                [this](const std::vector<std::string>&) -> std::string
                {
                    Toggle();
                    return m_open ? "[TF] outfit panel opened" : "[TF] outfit panel closed";
                },
                "Toggle the outfit (clan) panel", "TERRAFRONT", "tf_outfit");
            m_cmdRegistered = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFOutfitPanel initialized (tf_outfit to toggle)");
        return true;
    }

    void TFOutfitPanel::Update(float) {}
    void TFOutfitPanel::FixedUpdate(float) {}

    void TFOutfitPanel::Shutdown()
    {
        if (!m_initialized)
            return;
        if (m_cmdRegistered)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_outfit");
            m_cmdRegistered = false;
        }
        m_initialized = false;
    }

    void TFOutfitPanel::RenderDebugUI() {}

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

    void TFOutfitPanel::RenderUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_initialized || !m_open || !m_ctx || !m_outfits)
            return;
        if (!m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;

        ImGui::SetNextWindowSize(ImVec2(420.0f, 380.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Outfit", &m_open))
            RenderContents();
        ImGui::End();
#endif
    }

    void TFOutfitPanel::RenderContents()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_initialized || !m_outfits)
            return;

        // Nested tab bar (fine inside the social panel's tab item too).
        if (!ImGui::BeginTabBar("##outfittabs"))
            return;

        if (ImGui::BeginTabItem("Outfit"))
        {
            DrawStatusLine();

            if (m_outfits->LocalMirror().hasInvite)
                DrawInviteBox();

            if (!m_outfits->InOutfit())
                DrawCreateForm();
            else
                DrawRoster();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Leaderboard"))
        {
            DrawLeaderboard();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
#endif
    }

#ifdef SPARK_HAS_IMGUI

    void TFOutfitPanel::DrawStatusLine()
    {
        const TFOutfitSystem::Mirror& mir = m_outfits->LocalMirror();
        if (!mir.hasResult)
            return;
        if (mir.lastResult == TFOutfitResult::Ok)
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "OK: %s", OutfitResultText(mir.lastResult));
        else
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Failed: %s", OutfitResultText(mir.lastResult));
        ImGui::Separator();
    }

    void TFOutfitPanel::DrawInviteBox()
    {
        const TFOutfitSystem::Mirror& mir = m_outfits->LocalMirror();
        ImGui::TextWrapped("Invite: [%s] %s (from %s)", mir.inviteTag.c_str(), mir.inviteName.c_str(),
                           mir.inviteFrom.c_str());
        if (ImGui::Button("Accept##inv"))
            m_outfits->ClientRequestAccept();
        ImGui::SameLine();
        if (ImGui::Button("Decline##inv"))
            m_outfits->ClientRequestDecline();
        ImGui::Separator();
    }

    void TFOutfitPanel::DrawCreateForm()
    {
        ImGui::TextUnformatted("You are not in an outfit.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Create one:");
        ImGui::PushItemWidth(220.0f);
        ImGui::InputText("Name (3-24)", m_nameBuf, sizeof(m_nameBuf));
        ImGui::PopItemWidth();
        ImGui::PushItemWidth(90.0f);
        ImGui::InputText("Tag (2-5)", m_tagBuf, sizeof(m_tagBuf));
        ImGui::PopItemWidth();

        const std::string name(m_nameBuf);
        const std::string tag(m_tagBuf);
        const bool nameOk = TFOutfitSystem::ValidateOutfitName(name);
        const bool tagOk = TFOutfitSystem::ValidateOutfitTag(tag);
        if (!name.empty() && !nameOk)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Name: 3-24 letters/digits, single spaces");
        if (!tag.empty() && !tagOk)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Tag: 2-5 letters/digits");

        ImGui::BeginDisabled(!nameOk || !tagOk);
        if (ImGui::Button("Create Outfit"))
        {
            m_outfits->ClientRequestCreate(name, tag);
            m_nameBuf[0] = '\0';
            m_tagBuf[0] = '\0';
        }
        ImGui::EndDisabled();
    }

    void TFOutfitPanel::DrawRoster()
    {
        const TFOutfitSystem::Mirror& mir = m_outfits->LocalMirror();
        const TFOutfitRank myRank = m_outfits->LocalRank();

        ImGui::Text("[%s] %s", mir.tag.c_str(), mir.name.c_str());
        ImGui::Text("Members: %u   Your rank: %s", mir.totalMembers, OutfitRankName(myRank));
        ImGui::Separator();

        // --- invite by character name (Officer+) --------------------------------
        if (myRank != TFOutfitRank::Member)
        {
            ImGui::PushItemWidth(200.0f);
            ImGui::InputText("##invitename", m_inviteBuf, sizeof(m_inviteBuf));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            const bool canInvite = m_inviteBuf[0] != '\0';
            ImGui::BeginDisabled(!canInvite);
            if (ImGui::Button("Invite"))
            {
                m_outfits->ClientRequestInvite(std::string(m_inviteBuf));
                m_inviteBuf[0] = '\0';
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(online character name)");
            ImGui::Separator();
        }

        // --- roster --------------------------------------------------------------
        const bool isLeader = myRank == TFOutfitRank::Leader;
        const bool isOfficer = myRank == TFOutfitRank::Officer;

        ImGui::BeginChild("##outfitroster", ImVec2(0.0f, -70.0f), true);
        for (const TFOutfitSystem::MirrorMember& m : mir.members)
        {
            ImGui::PushID(static_cast<int>(m.charId & 0x7FFFFFFF));

            const bool self = m.charId == mir.yourCharId;
            if (m.online)
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "%s", m.name.c_str());
            else
                ImGui::TextDisabled("%s", m.name.c_str());
            ImGui::SameLine(180.0f);
            ImGui::TextUnformatted(OutfitRankName(m.rank));
            if (self)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(you)");
            }

            // rank/kick controls, permission-gated to mirror the server's rules
            if (!self)
            {
                const bool canKick =
                    (isLeader && m.rank != TFOutfitRank::Leader) || (isOfficer && m.rank == TFOutfitRank::Member);
                if (canKick)
                {
                    ImGui::SameLine(260.0f);
                    if (ImGui::SmallButton("Kick"))
                        m_outfits->ClientRequestKick(m.charId);
                }
                if (isLeader && m.rank == TFOutfitRank::Member)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Promote"))
                        m_outfits->ClientRequestSetRank(m.charId, TFOutfitRank::Officer);
                }
                if (isLeader && m.rank == TFOutfitRank::Officer)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Demote"))
                        m_outfits->ClientRequestSetRank(m.charId, TFOutfitRank::Member);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Make Leader"))
                        m_outfits->ClientRequestSetRank(m.charId, TFOutfitRank::Leader);
                }
            }

            ImGui::PopID();
        }
        ImGui::EndChild();

        // --- leave / disband (two-click confirm) ----------------------------------
        if (!m_confirmLeave)
        {
            if (ImGui::Button("Leave Outfit"))
                m_confirmLeave = true;
        }
        else
        {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "Leave '%s'?", mir.name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Confirm Leave"))
            {
                m_outfits->ClientRequestLeave();
                m_confirmLeave = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##leave"))
                m_confirmLeave = false;
        }

        if (isLeader)
        {
            ImGui::SameLine(0.0f, 30.0f);
            if (!m_confirmDisband)
            {
                if (ImGui::Button("Disband"))
                    m_confirmDisband = true;
            }
            else
            {
                if (ImGui::Button("CONFIRM DISBAND"))
                {
                    m_outfits->ClientRequestDisband();
                    m_confirmDisband = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##disband"))
                    m_confirmDisband = false;
            }
        }
    }

    void TFOutfitPanel::DrawLeaderboard()
    {
        const TFOutfitSystem::LeaderboardMirror& lb = m_outfits->LocalLeaderboard();

        // Request while the tab is visible: every kTFOutfitLbRefreshSec once a
        // snapshot exists, with a short 2 s retry until the first one arrives.
        const double now = ImGui::GetTime();
        const double due = lb.valid ? static_cast<double>(kTFOutfitLbRefreshSec) : 2.0;
        if (now - m_lbLastRequest >= due)
        {
            m_outfits->ClientRequestLeaderboard();
            m_lbLastRequest = now;
        }

        if (!lb.valid)
        {
            ImGui::TextDisabled("Fetching leaderboard...");
            return;
        }

        ImGui::Text("Top outfits - ISO week %u   (%u outfit%s total)", lb.weekKey, lb.totalOutfits,
                    lb.totalOutfits == 1 ? "" : "s");
        ImGui::TextDisabled("Score: kill +1, capture +10, alert win +100. Refreshes every %.0f s.",
                            kTFOutfitLbRefreshSec);
        ImGui::Separator();

        if (lb.rows.empty())
        {
            ImGui::TextDisabled("No outfits yet — create one on the Outfit tab.");
            return;
        }

        if (ImGui::BeginTable("##outfitlb", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 42.0f);
            ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Weekly", ImGuiTableColumnFlags_WidthFixed, 74.0f);
            ImGui::TableSetupColumn("All-time", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < lb.rows.size(); ++i)
            {
                const TFOutfitSystem::LbRow& r = lb.rows[i];

                // Gap marker between the top 10 and an own row beyond it.
                if (i > 0 && r.rank > lb.rows[i - 1].rank + 1)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("...");
                }

                const bool you = static_cast<int>(i) == lb.yourIndex;
                ImGui::TableNextRow();
                if (you)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           ImGui::GetColorU32(ImVec4(0.18f, 0.42f, 0.24f, 0.55f)));
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", r.rank);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("[%s]", r.tag.c_str());
                ImGui::TableSetColumnIndex(2);
                if (you)
                    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.60f, 1.0f), "%s (yours)", r.name.c_str());
                else
                    ImGui::TextUnformatted(r.name.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", static_cast<unsigned long long>(r.weekly));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(r.allTime));
            }
            ImGui::EndTable();
        }

        if (lb.yourIndex < 0 && m_outfits->InOutfit())
            ImGui::TextDisabled("Your outfit has not scored yet this week.");
    }

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
