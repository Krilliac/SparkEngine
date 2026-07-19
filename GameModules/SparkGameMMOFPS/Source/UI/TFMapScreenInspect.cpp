/**
 * @file TFMapScreenInspect.cpp
 * @brief TFMapScreen W7 selected-region inspect panel: owner / occupants /
 *        capture progress from replicated state, DEPLOY HERE while dead, and
 *        the REDEPLOY / CANCEL buttons driving the redeploy state machine.
 *
 * Split from TFMapScreen.cpp per the repo file-size rule (TFHUDCombat.cpp
 * pattern — same class, feature-owned translation units). TFMapScreen.cpp
 * keeps the lifecycle, open/close input, and the W7 redeploy state machine;
 * TFMapScreenDraw.cpp keeps the fullscreen hex-grid overlay.
 */
#include "UI/TFMapScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFRedeployRules.h"
#include "Game/TFSquadSystem.h"
#include "UI/TFUiCommon.h"
#include "World/TFRegionSystem.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    // ---------------------------------------------------------------------------
    // W7: selected-region inspect panel (owner / occupants / progress / redeploy)
    // ---------------------------------------------------------------------------

    void TFMapScreen::DrawInspectPanel()
    {
        using namespace TFUi;

        if (!m_open || m_selected == kInvalidRegion || !m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const RegionDef* rd = m_ctx->data->GetRegion(m_selected);
        if (!rd)
        {
            m_selected = kInvalidRegion;
            return;
        }

        TFRegionSystem* rs = m_ctx->regions;
        const FactionId myFaction = m_ctx->localFaction;
        const FactionId owner = rs ? rs->OwnerOf(rd->id) : rd->homeFaction;
        FactionId capturing = FactionId::None;
        bool contested = false;
        const float progress = rs ? rs->CaptureProgress(rd->id, capturing, contested) : 0.0f;

        // Occupants (own-faction intel only — same replicated source as the badges).
        uint32_t friendlies = 0, squadmates = 0;
        if (m_ctx->players && myFaction != FactionId::None)
        {
            const RegionDef* self = rd;
            const auto& regions = m_ctx->data->GetContinent().regions;
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    if (p.faction != myFaction)
                        return;
                    // Nearest-region test against the selected hex only: cheaper
                    // than a full map and identical for "is he here?".
                    float bestD2 = 1e18f;
                    RegionId best = kInvalidRegion;
                    for (const RegionDef& other : regions)
                    {
                        const float dx = other.centerX - p.pos[0];
                        const float dz = other.centerZ - p.pos[2];
                        const float d2 = dx * dx + dz * dz;
                        if (d2 < bestD2)
                        {
                            bestD2 = d2;
                            best = other.id;
                        }
                    }
                    if (best != self->id)
                        return;
                    ++friendlies;
                    if (m_ctx->squads && m_ctx->squads->IsLocalSquadMember(p.owner))
                        ++squadmates;
                });
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float panelW = 300.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - panelW - 24.0f, vp->Pos.y + vp->Size.y * 0.24f));
        ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        if (!ImGui::Begin("##TFMapInspect", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted(rd->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", TierTag(rd->tier));
        ImGui::Separator();

        float oc[4];
        FactionColor(owner, oc);
        ImGui::Text("Owner:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(oc[0], oc[1], oc[2], 1.0f), "%s", FactionName(owner));
        ImGui::Text("Flux: %d / tick", rd->fluxPerTick);
        ImGui::Text("Friendlies here: %u", friendlies);
        if (squadmates > 0)
            ImGui::TextColored(ImVec4(0.42f, 0.88f, 0.56f, 1.0f), "Squadmates here: %u", squadmates);

        if (progress > 0.001f && capturing != FactionId::None)
        {
            float cc[4];
            FactionColor(capturing, cc);
            ImGui::TextColored(ImVec4(cc[0], cc[1], cc[2], 1.0f), "Capturing: %s %d%%%s", FactionTag(capturing),
                               static_cast<int>(progress * 100.0f), contested ? "  (CONTESTED)" : "");
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 6.0f), "");
        }
        else if (contested)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "CONTESTED");
        }

        ImGui::Separator();

        const bool dead = !LocalPawnAlive();
        if (dead)
        {
            if (rs && myFaction != FactionId::None && rs->CanSpawnAt(rd->id, myFaction))
            {
                if (ImGui::Button("DEPLOY HERE", ImVec2(-1.0f, 32.0f)))
                {
                    SendRegionSpawnRequest(rd->id);
                    Close();
                }
            }
            else
            {
                ImGui::TextDisabled("Not a spawn option");
            }
        }
        else if (m_redeployPending && m_redeployTarget == rd->id)
        {
            char label[48];
            std::snprintf(label, sizeof(label), "REDEPLOYING IN %.1f s", std::max(0.0f, m_redeployCountdown));
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.24f, 1.0f), "%s", label);
            if (ImGui::Button("CANCEL", ImVec2(-1.0f, 28.0f)))
                CancelRedeploy("by request");
        }
        else
        {
            const uint8_t reason = CanRedeployTo(rd->id);
            if (reason == kTFRedeployOk)
            {
                char label[48];
                std::snprintf(label, sizeof(label), "REDEPLOY (%.0f s)", TFRedeployRules::kTFRedeployCountdownSec);
                if (ImGui::Button(label, ImVec2(-1.0f, 32.0f)))
                    StartRedeploy(rd->id);
            }
            else
            {
                ImGui::BeginDisabled();
                ImGui::Button("REDEPLOY", ImVec2(-1.0f, 32.0f));
                ImGui::EndDisabled();
                if (reason == kTFRedeployCooldown)
                    ImGui::TextDisabled("Ready in %.0f s", std::ceil(m_cooldownLeft));
                else
                    ImGui::TextDisabled("%s", TFRedeployRules::ReasonText(reason));
            }
        }

        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI — headless: map is state-only

    void TFMapScreen::DrawInspectPanel() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
