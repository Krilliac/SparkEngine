/**
 * @file TFAlertSystemUi.cpp
 * @brief TFAlertSystem presentation: the tf_alert status report, the
 *        top-center HUD banner (countdown + per-faction score bars), the
 *        VICTORY/DEFEAT/DRAW end splash and the ImGui debug panel (plus the
 *        headless no-op stubs). Split from TFAlertSystem.cpp (same class,
 *        split per the repo file-size rules — mirrors the TFTravelSystem
 *        split).
 */
#include "World/TFAlertSystem.h"

#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "World/TFAlertSystemInternal.h" // AlertDetail: FactionOfIdx, PlayableFaction

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>

namespace Terrafront
{

    using namespace AlertDetail;

    // ---------------------------------------------------------------------------
    // Console / status
    // ---------------------------------------------------------------------------

    std::string TFAlertSystem::StatusString() const
    {
        TF_AlertState st{};
        GetView(st);
        std::ostringstream os;
        os << "[TF] alerts: ";
        switch (static_cast<TFAlertPhase>(st.phase))
        {
        case TFAlertPhase::Running:
        {
            os << TFAlertName(static_cast<TFAlertType>(st.type)) << " RUNNING, " << static_cast<int>(st.secondsLeft)
               << "s left";
            if (static_cast<TFAlertType>(st.type) == TFAlertType::FacilityControl)
                os << " (target region " << st.regionId << ")";
            os << "\n  scores: MRA=" << st.score[0] << " AUC=" << st.score[1] << " HLX=" << st.score[2];
            break;
        }
        case TFAlertPhase::Ended:
            os << TFAlertName(static_cast<TFAlertType>(st.type))
               << " ENDED, winner=" << FactionTag(static_cast<FactionId>(st.winner)) << "  scores: MRA=" << st.score[0]
               << " AUC=" << st.score[1] << " HLX=" << st.score[2];
            break;
        case TFAlertPhase::Idle:
        default:
            os << "idle";
            if (m_ctx && m_ctx->IsAuthority())
                os << ", next auto alert in " << static_cast<int>(m_idleLeft) << "s (needs alive pawns)";
            break;
        }
        return os.str();
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFAlertSystem::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        TF_AlertState st{};
        GetView(st);
        const TFAlertPhase phase = static_cast<TFAlertPhase>(st.phase);
        if (phase == TFAlertPhase::Idle)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float width = 380.0f;
        // Just under the HUD compass strip (top 12 px + 26 px tall) and above
        // the medal toast band (0.16 * height).
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + 46.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("##tf_alert_banner", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        const char* name = TFAlertName(static_cast<TFAlertType>(st.type));

        if (phase == TFAlertPhase::Running)
        {
            const int total = static_cast<int>(st.secondsLeft);
            char head[96];
            std::snprintf(head, sizeof(head), "ALERT: %s  %d:%02d", name, total / 60, total % 60);
            const float headW = ImGui::CalcTextSize(head).x;
            ImGui::SetCursorPosX((width - headW) * 0.5f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "%s", head);

            if (static_cast<TFAlertType>(st.type) == TFAlertType::FacilityControl && m_ctx->data &&
                m_ctx->data->IsLoaded())
            {
                const auto& regions = m_ctx->data->GetContinent().regions;
                if (st.regionId < regions.size())
                {
                    const std::string& rn = regions[st.regionId].name;
                    const float rw = ImGui::CalcTextSize(rn.c_str()).x;
                    ImGui::SetCursorPosX((width - rw) * 0.5f);
                    ImGui::TextDisabled("%s", rn.c_str());
                }
            }

            // Per-faction score bars, normalized to the current leader.
            uint32_t top = 1;
            for (uint32_t s : st.score)
                top = std::max(top, s);
            for (size_t i = 0; i < 3; ++i)
            {
                const FactionId f = FactionOfIdx(i);
                float col[4];
                FactionColor(f, col);
                char label[48];
                std::snprintf(label, sizeof(label), "%s %u", FactionTag(f), st.score[i]);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(col[0], col[1], col[2], 0.90f));
                ImGui::ProgressBar(static_cast<float>(st.score[i]) / static_cast<float>(top), ImVec2(-1.0f, 15.0f),
                                   label);
                ImGui::PopStyleColor();
            }
        }
        else // Ended: victory / defeat / draw splash
        {
            const FactionId winner = static_cast<FactionId>(st.winner);
            const FactionId mine = m_ctx->localFaction;
            const char* verdict = "ALERT OVER - DRAW"; // ASCII only: default ImGui font has no em-dash glyph
            ImVec4 color(0.75f, 0.75f, 0.75f, 1.0f);
            char buf[96];
            if (winner != FactionId::None)
            {
                if (PlayableFaction(mine) && mine == winner)
                {
                    std::snprintf(buf, sizeof(buf), "VICTORY - %s", name);
                    verdict = buf;
                    color = ImVec4(0.35f, 0.95f, 0.40f, 1.0f);
                }
                else if (PlayableFaction(mine))
                {
                    std::snprintf(buf, sizeof(buf), "DEFEAT - %s wins %s", FactionTag(winner), name);
                    verdict = buf;
                    color = ImVec4(0.95f, 0.30f, 0.25f, 1.0f);
                }
                else
                {
                    std::snprintf(buf, sizeof(buf), "%s wins %s", FactionName(winner), name);
                    verdict = buf;
                }
            }
            const float w = ImGui::CalcTextSize(verdict).x;
            ImGui::SetCursorPosX((width - w) * 0.5f);
            ImGui::TextColored(color, "%s", verdict);

            char scores[96];
            std::snprintf(scores, sizeof(scores), "MRA %u  |  AUC %u  |  HLX %u", st.score[0], st.score[1],
                          st.score[2]);
            const float sw = ImGui::CalcTextSize(scores).x;
            ImGui::SetCursorPosX((width - sw) * 0.5f);
            ImGui::TextDisabled("%s", scores);
        }

        ImGui::End();
    }

    void TFAlertSystem::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("Alerts (TFAlertSystem)"))
            return;
        TF_AlertState st{};
        GetView(st);
        ImGui::Text("phase=%u type=%s target=%u secondsLeft=%.1f", static_cast<unsigned>(st.phase),
                    TFAlertName(static_cast<TFAlertType>(st.type)), static_cast<unsigned>(st.regionId),
                    static_cast<double>(st.secondsLeft));
        ImGui::Text("scores: MRA=%u AUC=%u HLX=%u", st.score[0], st.score[1], st.score[2]);
        if (m_ctx && m_ctx->IsAuthority())
            ImGui::Text("idleLeft=%.0fs participants=%zu/%zu/%zu", static_cast<double>(m_idleLeft),
                        m_participants[0].size(), m_participants[1].size(), m_participants[2].size());
        ImGui::Text("started=%u ended=%u xpPaid=%u sent=%u rx=%u bad=%u", m_alertsStarted, m_alertsEnded, m_xpPaid,
                    m_statesSent, m_statesRx, m_badPackets);
    }

#else

    void TFAlertSystem::RenderUI() {}
    void TFAlertSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
