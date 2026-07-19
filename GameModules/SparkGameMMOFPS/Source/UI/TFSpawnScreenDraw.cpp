/**
 * @file TFSpawnScreenDraw.cpp
 * @brief TFSpawnScreen rendering half: the full-viewport deploy modal
 *        (RenderUI), faction splash (first join), class picker (classes.json,
 *        Colossus greyed out), spawn point list (skyanchor + CanSpawnAt
 *        regions + W3 deployed-Aegis + W11 squad-leader entries), and the
 *        footer with respawn countdown, LOADOUT/OPEN MAP shortcuts and DEPLOY.
 *
 * Split from TFSpawnScreen.cpp per the repo file-size rule (TFLoginFlowDraw.cpp
 * pattern — same class, feature-owned translation units). TFSpawnScreen.cpp
 * keeps the lifecycle, open/close state, death mirror, and sends.
 */
#include "UI/TFSpawnScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"  // PawnInfo, kTFRespawnDelaySec
#include "Game/TFSocialSystem.h"  // W11 squad-v2: leader name on the squad spawn entry
#include "Game/TFSquadSystem.h"   // W11 squad-v2: squad-leader spawn entry (spawnKind=3)
#include "Game/TFVehicleSystem.h" // W3 shared-edit: deployed-Aegis spawn entries
#include "Net/TFClientNet.h"
#include "UI/TFHUD.h"
#include "UI/TFLoadoutScreen.h" // loadout-depth wave: deploy-panel LOADOUT button
#include "UI/TFMapScreen.h"
#include "UI/TFUiCommon.h"
#include "World/TFRegionSystem.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    void TFSpawnScreen::RenderUI()
    {
        if (!m_open || !m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        if (m_ctx->map && m_ctx->map->IsOpen())
            return; // continent map is on top; we resume when it closes
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("##TFSpawnScreen", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(6, 8, 12, 215));

        const float panelW = std::min(960.0f, vp->Size.x * 0.92f);
        const float panelH = std::min(600.0f, vp->Size.y * 0.88f);
        const float panelX = vp->Pos.x + (vp->Size.x - panelW) * 0.5f;
        const float panelY = vp->Pos.y + (vp->Size.y - panelH) * 0.5f;
        dl->AddRectFilled(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH), IM_COL32(16, 19, 25, 235),
                          6.0f);
        dl->AddRect(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH),
                    TFUi::FactionCol(m_ctx->localFaction, 0.55f), 6.0f, 0, 2.0f);

        if (m_ctx->localFaction == FactionId::None)
            DrawFactionSplash(panelX, panelY, panelW, panelH);
        else
            DrawDeployPanel(panelX, panelY, panelW, panelH);

        ImGui::End();
    }

    void TFSpawnScreen::DrawFactionSplash(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        AddTextCentered(dl, 30.0f, ImVec2(panelX + panelW * 0.5f, panelY + 44.0f), IM_COL32(235, 235, 235, 235),
                        "CHOOSE YOUR FACTION");
        AddTextCentered(dl, 15.0f, ImVec2(panelX + panelW * 0.5f, panelY + 76.0f), IM_COL32(170, 174, 180, 210),
                        "Veyra is shattered. Three powers war over its flux wells.");

        const float pad = 22.0f;
        const float cardW = (panelW - pad * 4.0f) / 3.0f;
        const float cardH = panelH - 160.0f;
        int i = 0;
        for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
        {
            const float x = panelX + pad + static_cast<float>(i) * (cardW + pad);
            ImGui::SetCursorScreenPos(ImVec2(x, panelY + 104.0f));

            float c[4];
            FactionColor(f, c);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c[0] * 0.45f, c[1] * 0.45f, c[2] * 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c[0] * 0.70f, c[1] * 0.70f, c[2] * 0.70f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c[0], c[1], c[2], 1.0f));

            char label[96];
            std::snprintf(label, sizeof(label), "%s\n[%s]##fac%d", FactionName(f), FactionTag(f), i);
            if (ImGui::Button(label, ImVec2(cardW, cardH * 0.5f)))
                SendFactionSelect(f);
            ImGui::PopStyleColor(3);

            const FactionDef* fd = m_ctx->data->GetFaction(f);
            ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, panelY + 112.0f + cardH * 0.5f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW - 8.0f); // window-local
            ImGui::TextWrapped("%s", fd && !fd->blurb.empty() ? fd->blurb.c_str() : "");
            ImGui::PopTextWrapPos();
            ++i;
        }
    }

    void TFSpawnScreen::DrawDeployPanel(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        TFRegionSystem* rs = m_ctx->regions;
        const FactionId myFaction = m_ctx->localFaction;
        const auto& regions = m_ctx->data->GetContinent().regions;

        char buf[224];
        std::snprintf(buf, sizeof(buf), "DEPLOY  -  %s", FactionName(myFaction));
        AddTextCentered(dl, 26.0f, ImVec2(panelX + panelW * 0.5f, panelY + 32.0f), FactionCol(myFaction, 1.0f), buf);

        // W6: last-death summary from the HUD (valid until the next local spawn).
        if (m_ctx->hud && m_ctx->hud->LastDeath().valid)
        {
            const TFHUD::DeathSummary& d = m_ctx->hud->LastDeath();
            if (d.distanceM >= 0.0f)
                std::snprintf(buf, sizeof(buf), "Killed by %s   [%s]   %.0f m%s", d.killerName.c_str(),
                              d.weaponName.c_str(), d.distanceM, d.headshot ? "   HEADSHOT" : "");
            else
                std::snprintf(buf, sizeof(buf), "Killed by %s   [%s]%s", d.killerName.c_str(), d.weaponName.c_str(),
                              d.headshot ? "   HEADSHOT" : "");
            AddTextCentered(dl, 14.0f, ImVec2(panelX + panelW * 0.5f, panelY + 52.0f),
                            d.headshot ? IM_COL32(245, 130, 110, 225) : IM_COL32(185, 188, 194, 210), buf);
        }

        const float pad = 20.0f;
        const float topY = panelY + 62.0f;
        const float listH = panelH - 62.0f - 76.0f;
        const float classW = panelW * 0.38f - pad * 1.5f;
        const float spawnW = panelW - classW - pad * 3.0f;

        // ---- class picker ------------------------------------------------------
        ImGui::SetCursorScreenPos(ImVec2(panelX + pad, topY));
        ImGui::BeginChild("##tf_classes", ImVec2(classW, listH), true);
        ImGui::TextDisabled("CLASS");
        ImGui::Separator();
        for (const ClassDef& cd : m_ctx->data->AllClasses())
        {
            const bool isColossus = (cd.id == ClassId::Colossus);
            const bool selected = (cd.id == m_selClass);
            std::snprintf(buf, sizeof(buf), "%s\n  %s%s##cls%u", cd.name.c_str(), cd.role.c_str(),
                          isColossus ? "  (exosuit - via terminal)" : "", static_cast<unsigned>(cd.id));
            if (ImGui::Selectable(buf, selected, isColossus ? ImGuiSelectableFlags_Disabled : 0, ImVec2(0.0f, 38.0f)))
                m_selClass = cd.id;
            if (!isColossus && ImGui::IsItemHovered() && !cd.ability.name.empty())
                ImGui::SetTooltip("%s - %s", cd.ability.name.c_str(), cd.ability.desc.c_str());
            ImGui::Spacing();
        }
        ImGui::EndChild();

        // ---- spawn point list ----------------------------------------------------
        // Keep the region selection valid against live territory state.
        if (m_selKind == 1 && (!rs || m_selRegion == kInvalidRegion || !rs->CanSpawnAt(m_selRegion, myFaction)))
        {
            m_selKind = 0;
            m_selRegion = kInvalidRegion;
        }
        // W3 shared-edit (vehicles agent): keep the Aegis selection valid against
        // live vehicle state (undeployed / destroyed / captured mid-screen).
        if (m_selKind == 2)
        {
            TFVehicleInfo vi;
            const bool ok = m_ctx->vehicles && m_selAegis != 0 && m_ctx->vehicles->GetVehicleInfo(m_selAegis, vi) &&
                            vi.deployed && vi.hp > 0.0f && vi.faction == myFaction;
            if (!ok)
            {
                m_selKind = 0;
                m_selAegis = 0;
            }
        }

        // W11 squad-v2 lane: keep the squad-leader selection valid (needs a
        // squad and a leader who isn't you; the server re-validates
        // leader-alive + the 15 s cooldown on request).
        if (m_selKind == 3)
        {
            const auto squadView = m_ctx->squads ? m_ctx->squads->GetLocalSquadView() : TFSquadSystem::LocalSquadView{};
            if (squadView.squad == kInvalidSquad || squadView.leader == kInvalidPlayer ||
                squadView.leader == m_ctx->localPlayer)
                m_selKind = 0;
        }

        auto distTo = [&](const RegionDef& rd)
        {
            const float dx = rd.centerX - m_deathPos[0];
            const float dz = rd.centerZ - m_deathPos[2];
            return std::sqrt(dx * dx + dz * dz);
        };

        ImGui::SetCursorScreenPos(ImVec2(panelX + pad * 2.0f + classW, topY));
        ImGui::BeginChild("##tf_spawns", ImVec2(spawnW, listH), true);
        ImGui::TextDisabled("SPAWN POINT");
        ImGui::Separator();

        // Skyanchor is always available (spawnKind 0; the server picks the pads).
        {
            const RegionDef* home = nullptr;
            for (const RegionDef& rd : regions)
                if (rd.tier == "skyanchor" && rd.homeFaction == myFaction)
                {
                    home = &rd;
                    break;
                }
            if (home)
                std::snprintf(buf, sizeof(buf), "%s   -   HOME   %.0fm##sky", home->name.c_str(), distTo(*home));
            else
                std::snprintf(buf, sizeof(buf), "Skyanchor   -   HOME##sky");
            if (ImGui::Selectable(buf, m_selKind == 0))
            {
                m_selKind = 0;
                m_selRegion = kInvalidRegion;
            }
        }

        for (const RegionDef& rd : regions)
        {
            if (rd.tier == "skyanchor")
                continue; // covered by the HOME entry
            if (!rs || !rs->CanSpawnAt(rd.id, myFaction))
                continue;
            std::snprintf(buf, sizeof(buf), "%s   -   %s   %.0fm##rg%u", rd.name.c_str(), TierTag(rd.tier), distTo(rd),
                          static_cast<unsigned>(rd.id));
            if (ImGui::Selectable(buf, m_selKind == 1 && m_selRegion == rd.id))
            {
                m_selKind = 1;
                m_selRegion = rd.id;
            }
        }

        // W3 shared-edit (vehicles agent): deployed friendly Aegis mobile spawns
        // (spawnKind=2). Range-gated to the Aegis deploy radius around the death
        // spot (skipped on first deploy, when no death position exists yet); the
        // server independently validates deployed/friendly/alive on request.
        if (m_ctx->vehicles)
        {
            float deployRadius = 600.0f;
            if (const VehicleDef* aegisDef = m_ctx->data->GetVehicle(VehicleId::Aegis))
                if (aegisDef->deployRadiusM > 0.0f)
                    deployRadius = aegisDef->deployRadiusM;
            const bool haveDeathPos = m_deathPos[0] != 0.0f || m_deathPos[1] != 0.0f || m_deathPos[2] != 0.0f;

            m_ctx->vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& vi)
                {
                    if (vi.vehId != VehicleId::Aegis || !vi.deployed || vi.hp <= 0.0f || vi.faction != myFaction)
                        return;
                    const float dx = vi.pos[0] - m_deathPos[0];
                    const float dz = vi.pos[2] - m_deathPos[2];
                    const float dist = std::sqrt(dx * dx + dz * dz);
                    if (haveDeathPos && dist > deployRadius)
                        return;
                    std::snprintf(buf, sizeof(buf), "Aegis (mobile spawn)   -   VEH   %.0fm##ae%u", dist, vi.entity);
                    if (ImGui::Selectable(buf, m_selKind == 2 && m_selAegis == vi.entity))
                    {
                        m_selKind = 2;
                        m_selAegis = vi.entity;
                        m_selRegion = kInvalidRegion;
                    }
                });
        }

        // W11 squad-v2 lane: squad-leader spawn (spawnKind=3). The server
        // enforces leader-alive + the 15 s per-requester cooldown
        // (TFSquadSystem::GetSquadLeaderSpawn) and denies with reason=timer +
        // respawnDelay, so this entry only advertises; DEPLOY stays generic.
        if (m_ctx->squads)
        {
            const auto squadView = m_ctx->squads->GetLocalSquadView();
            if (squadView.squad != kInvalidSquad && squadView.leader != kInvalidPlayer &&
                squadView.leader != m_ctx->localPlayer)
            {
                char leaderName[32];
                std::string rosterName;
                if (m_ctx->social && m_ctx->social->NameOfPlayer(squadView.leader, rosterName))
                    std::snprintf(leaderName, sizeof(leaderName), "%s", rosterName.c_str());
                else
                    std::snprintf(leaderName, sizeof(leaderName), "P%u", squadView.leader);

                PawnInfo leaderPawn{};
                const bool leaderAlive =
                    m_ctx->players && m_ctx->players->GetPawnByPlayer(squadView.leader, leaderPawn) && leaderPawn.alive;
                if (leaderAlive)
                {
                    const float dx = leaderPawn.pos[0] - m_deathPos[0];
                    const float dz = leaderPawn.pos[2] - m_deathPos[2];
                    std::snprintf(buf, sizeof(buf), "Squad leader: %s   -   SQD   %.0fm##sq", leaderName,
                                  std::sqrt(dx * dx + dz * dz));
                    if (ImGui::Selectable(buf, m_selKind == 3))
                    {
                        m_selKind = 3;
                        m_selRegion = kInvalidRegion;
                        m_selAegis = 0;
                    }
                }
                else
                {
                    std::snprintf(buf, sizeof(buf), "Squad leader: %s   -   DOWN##sq", leaderName);
                    ImGui::Selectable(buf, false, ImGuiSelectableFlags_Disabled);
                }
            }
        }
        ImGui::EndChild();

        // ---- footer: countdown + DEPLOY + map shortcut -----------------------------
        // W3 shared-edit (vehicles agent): Aegis spawns run a SHORTER respawn
        // timer (DESIGN §4: 5 s vs 8 s) — mirror the server's discount locally so
        // the button unlocks when the server would actually accept.
        float effRespawnLeft = m_respawnLeft;
        if (m_selKind == 2 && m_ctx->vehicles)
            effRespawnLeft = std::max(
                0.0f, m_respawnLeft - std::max(0.0f, kTFRespawnDelaySec - m_ctx->vehicles->AegisRespawnDelaySec()));

        // W10 sanctuary-v2: class-terminal mode while alive — the server denies
        // TF_SpawnRequest for a live pawn, so DEPLOY is disabled and the picked
        // class simply persists (m_selClass) until the next real deploy.
        const bool aliveTerminal = m_terminalMode && LocalPawnAlive();

        const float footY = panelY + panelH - 60.0f;
        ImGui::SetCursorScreenPos(ImVec2(panelX + pad, footY + 8.0f));
        if (aliveTerminal)
            ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f),
                               "Class terminal - selection applies on your next deploy");
        else if (effRespawnLeft > 0.0f)
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Respawn in %.1fs", effRespawnLeft);
        else
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Ready to deploy");

        ImGui::SetCursorScreenPos(ImVec2(panelX + pad, footY + 28.0f));
        if (aliveTerminal)
            ImGui::TextDisabled("E or CLOSE - leave the terminal");
        else
            ImGui::TextDisabled("M - continent map (click a linked region to deploy)");

        if (m_terminalMode)
        {
            ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 465.0f, footY));
            if (ImGui::Button("CLOSE", ImVec2(120.0f, 44.0f)))
                Close();
        }

        // loadout-depth wave: reachable from the deploy panel (and the
        // sanctuary class terminal, via terminal mode above — same button).
        // Left of OPEN MAP so it never collides with the terminal CLOSE slot.
        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 600.0f, footY));
        if (ImGui::Button("LOADOUT", ImVec2(120.0f, 44.0f)) && m_ctx->loadoutUI)
            m_ctx->loadoutUI->Open();

        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 330.0f, footY));
        if (ImGui::Button("OPEN MAP", ImVec2(120.0f, 44.0f)) && m_ctx->map)
            m_ctx->map->Open();

        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 195.0f, footY));
        const bool blocked = aliveTerminal || (effRespawnLeft > 0.0f) || (m_debounce > 0.0f) || !m_ctx->clientNet ||
                             !m_ctx->clientNet->IsConnected();
        ImGui::BeginDisabled(blocked);
        if (effRespawnLeft > 0.0f)
            std::snprintf(buf, sizeof(buf), "DEPLOY (%.1fs)", effRespawnLeft);
        else
            std::snprintf(buf, sizeof(buf), "DEPLOY");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.56f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.68f, 0.30f, 1.0f));
        if (ImGui::Button(buf, ImVec2(175.0f, 44.0f)))
            SendSpawnRequest();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }

#else // !SPARK_HAS_IMGUI — headless: screen is state-only

    void TFSpawnScreen::RenderUI() {}
    void TFSpawnScreen::DrawFactionSplash(float, float, float, float) {}
    void TFSpawnScreen::DrawDeployPanel(float, float, float, float) {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
