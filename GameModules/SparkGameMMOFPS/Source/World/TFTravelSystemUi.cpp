/**
 * @file TFTravelSystemUi.cpp
 * @brief TFTravelSystem presentation: the [E] terminal prompt, sanctuary
 *        hint, continent-select menu and the tf_travel_debug panel (plus the
 *        headless no-op stubs). Split from TFTravelSystem.cpp (same class,
 *        split per the repo file-size rules — mirrors the TFVehicleSystem
 *        split).
 */
#include "World/TFTravelSystem.h"

#include "Game/TFTargetRange.h" // sanctuary-v2 lane (W10): cosmetic firing range
#include "Game/TFTutorial.h"    // tutorial-flow lane (W12): first-join guided flow
#include "UI/TFLoginFlow.h"     // W13 multimap follow-up: live LAN endpoint preference (see RenderUI)
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFRegionSystem.h"
#include "World/TFSanctuaryDecor.h" // sanctuary-v2 lane (W10): decor + class terminal

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <cstdio>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    void TFTravelSystem::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        // sanctuary-v2 lane (W10): world-anchored range readouts + the class-
        // terminal prompt draw before this system's own early returns; each
        // subsystem gates itself on open menus / pawn state internally.
        if (m_sanctuaryDecor)
            m_sanctuaryDecor->RenderUI();
        if (m_targetRange)
            m_targetRange->RenderUI();
        if (m_tutorial) // tutorial-flow lane (W12): checklist + hint markers
            m_tutorial->RenderUI();

        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive) || !alive)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 promptAt(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.58f);

        if (!m_menuOpen)
        {
            if (m_nearTerminal)
                TFUi::AddTextCentered(fg, 17.0f, promptAt, IM_COL32(230, 230, 230, 220), "[E] Travel terminal");
            // Ambient sanctuary hint so new players know where they are.
            if (TFTravel_IsInSanctuary(pos[0], pos[2]))
            {
                char hint[96];
                std::snprintf(hint, sizeof(hint), "%s  -  safe zone. Use the travel terminal to deploy.",
                              m_sanctuaryDisplayName.c_str());
                TFUi::AddTextCentered(fg, 14.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 26.0f),
                                      IM_COL32(180, 220, 255, 200), hint);
            }
            return;
        }

        // ---- continent select menu -------------------------------------------
        const float w = 420.0f, h = 320.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + (vp->Size.y - h) * 0.5f),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::Begin("Travel Terminal##tf_travel", &open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::TextUnformatted("Select destination:");
            ImGui::Separator();

            ImGui::Text("%s", m_continentDisplayName.c_str());
            ImGui::TextDisabled("%s", m_continentBlurb.c_str());

            // Territory summary straight off the region mirror (works on every role).
            if (m_ctx->regions)
            {
                const uint32_t total = m_ctx->regions->RegionCount();
                ImGui::Text("Territory: ");
                for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
                {
                    float c[4];
                    FactionColor(f, c);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "%s %u/%u", FactionTag(f),
                                       m_ctx->regions->RegionsHeld(f), total);
                }
            }
            // Population from the server summary (authority builds it locally).
            if (m_hasInfo)
            {
                ImGui::Text("Population: ");
                for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
                {
                    float c[4];
                    FactionColor(f, c);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "%s %u", FactionTag(f),
                                       m_lastInfo.pop[static_cast<size_t>(f)]);
                }
            }
            else
            {
                ImGui::TextDisabled("Population: (querying...)");
            }

            ImGui::Spacing();
            const bool inSanctuary = TFTravel_IsInSanctuary(pos[0], pos[2]);
            // The live continent always travels as positional mapId 1 ("the
            // continent this process loaded" — see TFSanctuaryZone.h mapId trick);
            // only the label follows the active continents.json entry.
            char deployLbl[96];
            std::snprintf(deployLbl, sizeof(deployLbl), "Deploy to %s", m_continentDisplayName.c_str());
            ImGui::BeginDisabled(!inSanctuary);
            if (ImGui::Button(deployLbl, ImVec2(-1.0f, 0.0f)))
                ClientRequestTravel(kTFMapCindralWastes);
            ImGui::EndDisabled();
            if (!inSanctuary)
                ImGui::TextDisabled("(already deployed - use the map to redeploy)");

            // W12 continent-2-data + W13 multimap-plumbing: continents hosted by
            // OTHER server processes are listed; joinable ONLY when this client
            // is connected to a real remote server (role==Client) AND
            // continents.json configured a host/port for that continent (see
            // docs/TERRAFRONT_MULTIMAP.md). Otherwise honest and disabled: "no
            // server hosting X" (no second server is running by default).
            for (const ContinentMeta& c : m_continentList)
            {
                if (c.active)
                    continue;
                ImGui::Spacing();
                ImGui::Text("%s", c.name.c_str());
                if (!c.blurb.empty())
                    ImGui::TextDisabled("%s", c.blurb.c_str());

                // W13 multimap follow-up (docs/TERRAFRONT_MULTIMAP.md section
                // 5.1): prefer a live LAN-discovered endpoint over the static
                // continents.json host/port when both are present. TFLoginFlow
                // keeps its scanner armed past the login screen while InWorld
                // on NetRole::Client (see TFLoginFlow::Update), so this is
                // just a read of its already-fresh, already-deduped results —
                // matched by beacon map name against this continent's
                // display name or continents.json key. Falls back to the
                // static entry, exactly as before, when no beacon matches.
                ContinentMeta effective = c;
                bool viaLan = false;
                if (m_ctx->loginFlow)
                {
                    for (const TFLanServerEntry& s : m_ctx->loginFlow->LanServers())
                    {
                        if (s.map == c.name || s.map == c.key)
                        {
                            effective.host = s.ip;
                            effective.port = s.gamePort;
                            viaLan = true;
                            break;
                        }
                    }
                }

                // server-authoritative follow-up (W13, §2.2): host/port here
                // are display hints only now (this client's local guess of
                // whether a hop is worth offering) — ClientRequestContinentHop
                // no longer trusts them for the actual connect. It re-resolves
                // `effective.mapId` against the CURRENT server's own registry
                // over the wire and only ever connects to what THAT reply
                // says, so a stale/edited local continents.json (or a bogus
                // LAN beacon) can misinform this label but can't misdirect
                // the connection.
                const bool haveEndpoint = !effective.host.empty() && effective.port != 0;
                const bool canHop = haveEndpoint && m_ctx->role == NetRole::Client;
                char otherLbl[160];
                if (haveEndpoint)
                    std::snprintf(otherLbl, sizeof(otherLbl), "Travel to %s (%s:%u)%s##tfcont%d", c.name.c_str(),
                                  effective.host.c_str(), static_cast<unsigned>(effective.port), viaLan ? " [LAN]" : "",
                                  c.mapId);
                else
                    std::snprintf(otherLbl, sizeof(otherLbl), "%s - no server hosting this continent##tfcont%d",
                                  c.name.c_str(), c.mapId);
                ImGui::BeginDisabled(!canHop);
                if (ImGui::Button(otherLbl, ImVec2(-1.0f, 0.0f)) && canHop)
                    ClientRequestContinentHop(effective);
                ImGui::EndDisabled();
                if (haveEndpoint && !canHop)
                    ImGui::TextDisabled("(server-hop needs a live client connection, not a local host)");
            }

            if (m_lastTravelMsg[0] != '\0')
            {
                ImGui::Separator();
                ImGui::TextWrapped("%s", m_lastTravelMsg);
            }
            ImGui::Separator();
            ImGui::TextDisabled("[E] close");
        }
        ImGui::End();
        if (!open)
            SetMenuOpen(false);
    }

    void TFTravelSystem::RenderDebugUI()
    {
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Travel", &m_showDebug))
        {
            ImGui::Text("local mapId     : %u (%s)", LocalMapId(), TFTravel_MapName(LocalMapId()));
            ImGui::Text("tracked players : %zu   pending placements: %zu", m_mapOf.size(), m_pendingPlace.size());
            ImGui::Text("travels         : %u (rejected %u)   sanctuary placements: %u", m_travels, m_travelsRejected,
                        m_placements);
            ImGui::Text("bad packets     : %u", m_badPackets);
            ImGui::Text("near terminal   : %s   menu: %s", m_nearTerminal ? "yes" : "no",
                        m_menuOpen ? "open" : "closed");
            for (const ContinentMeta& c : m_continentList)
            {
                if (c.active)
                {
                    ImGui::Text("continent %d    : %s [%s]  (ACTIVE this process)", c.mapId, c.name.c_str(),
                                c.key.c_str());
                    continue;
                }
                // multimap-plumbing lane (W13): surface the configured endpoint
                // (or its absence) for the other registered continents.
                if (!c.host.empty() && c.port != 0)
                    ImGui::Text("continent %d    : %s [%s]  (endpoint %s:%u)", c.mapId, c.name.c_str(), c.key.c_str(),
                                c.host.c_str(), static_cast<unsigned>(c.port));
                else
                    ImGui::Text("continent %d    : %s [%s]  (no endpoint configured)", c.mapId, c.name.c_str(),
                                c.key.c_str());
            }
            if (m_hasInfo)
                ImGui::Text("last info       : pop MRA %u / AUC %u / HLX %u", m_lastInfo.pop[1], m_lastInfo.pop[2],
                            m_lastInfo.pop[3]);
            ImGui::Separator();
            for (const auto& [pid, mapId] : m_mapOf)
                ImGui::Text("player %u -> %s", pid, TFTravel_MapName(mapId));
        }
        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI — headless builds keep the state machine, drop the pixels

    void TFTravelSystem::RenderUI() {}
    void TFTravelSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
