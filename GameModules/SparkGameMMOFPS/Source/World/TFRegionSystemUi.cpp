/**
 * @file TFRegionSystemUi.cpp
 * @brief TFRegionSystem viewer-facing surfaces: capture-point landmarks +
 *        W13 capture-fx feed, the local capture-bar HUD feed, the
 *        tf_capture_debug console report and the ImGui debug panel.
 *        Lifecycle + contract accessors live in TFRegionSystem.cpp, the
 *        capture loop in TFRegionSystemCapture.cpp, wire + persistence in
 *        TFRegionSystemNet.cpp.
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFCaptureFx.h" // W13 capture-fx lane: tower beam/burst/standing-ring
#include "Game/TFPlayerSystem.h"
#include "Game/TFVisualUtils.h"           // FactionStructureMaterial for capture-point banners
#include "World/TFRegionDecor.h"          // W9: per-tier building-kit decor (owned member)
#include "World/TFRegionSystemInternal.h" // RegionDetail: IsPlayableFaction, InCaptureRadius
#include "World/TFWorldSetup.h"           // TerrainHeightAt for landmark placement
#include "Engine/ECS/Components.h"        // Transform, MeshRenderer for capture landmarks
#include "UI/TFHUD.h"

#include "Spark/IEngineContext.h" // tf_capture_debug: GetWorld() null-probe (headless ECS gap)

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace Terrafront
{

    using namespace RegionDetail;

    void TFRegionSystem::UpdateCaptureVisuals(float dt)
    {
        // Viewer-only decorative landmarks; needs the ECS world, terrain, and data.
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.empty())
            return;

        // W13 capture-fx lane: advance/redraw the pooled owner-flip burst
        // particles + rising flares (independent of the spawn-once/retint work
        // below — pure pool bookkeeping, safe to call every frame regardless of
        // whether any region actually changed).
        TFCaptureFx::Get().Update(*m_ctx, dt);

        constexpr float kBannerHeightM = 11.5f; // just under the 12.7m cap tower top
        constexpr float kBannerSpinDegPerSec = 24.0f;

        if (!m_capVisualsSpawned)
        {
            m_capBannerEnt.assign(regions.size(), 0u);
            m_capBannerOwner.assign(regions.size(), -2);
            for (size_t i = 0; i < regions.size(); ++i)
            {
                const RegionDef& r = regions[i];
                const float x = r.centerX, z = r.centerZ;
                const float y = m_ctx->world->TerrainHeightAt(x, z);

                // Skyanchors are the faction warpgates: place the big faction
                // gate structure and no capture tower/banner (they never flip).
                if (r.tier == "skyanchor")
                {
                    const char* gate = nullptr;
                    switch (r.homeFaction)
                    {
                    case FactionId::MRA:
                        gate = "Assets/Models/MMOFPS/buildings/warpgate_mra.obj";
                        break;
                    case FactionId::AUC:
                        gate = "Assets/Models/MMOFPS/buildings/warpgate_auc.obj";
                        break;
                    case FactionId::HLX:
                        gate = "Assets/Models/MMOFPS/buildings/warpgate_hlx.obj";
                        break;
                    default:
                        break;
                    }
                    if (gate)
                    {
                        const auto wg = world->CreateEntity("TF_Warpgate");
                        Transform& wt = world->AddComponent<Transform>(wg);
                        wt.position = {x, y, z};
                        MeshRenderer& wmr = world->AddComponent<MeshRenderer>(wg);
                        wmr.meshPath = gate;
                        wmr.materialPath = FactionStructureMaterial(*m_ctx, r.homeFaction);
                        wmr.castShadows = true;
                        wmr.emissive = 0.45f; // warpgate reads as a lit faction beacon
                    }
                    continue;
                }

                const auto tower = world->CreateEntity("TF_CapTower");
                Transform& tt = world->AddComponent<Transform>(tower);
                tt.position = {x, y, z};
                MeshRenderer& tmr = world->AddComponent<MeshRenderer>(tower);
                tmr.meshPath = "Assets/Models/MMOFPS/buildings/cap_tower.obj";
                tmr.materialPath = FactionStructureMaterial(*m_ctx, FactionId::None);
                tmr.castShadows = true;

                const auto banner = world->CreateEntity("TF_CapBanner");
                Transform& bt = world->AddComponent<Transform>(banner);
                bt.position = {x, y + kBannerHeightM, z};
                MeshRenderer& bmr = world->AddComponent<MeshRenderer>(banner);
                bmr.meshPath = "Assets/Models/MMOFPS/buildings/cap_banner_ring.obj";
                bmr.materialPath = FactionStructureMaterial(*m_ctx, FactionId::None);
                bmr.castShadows = false;
                bmr.emissive = 0.55f; // owner-colored banner glows so it reads at range
                m_capBannerEnt[i] = static_cast<uint32_t>(banner);
            }
            m_capVisualsSpawned = true;
        }

        // Per-frame: spin each banner, retint it when its region owner changes,
        // and drive the W13 capture-fx lane's tower progress beam + one-shot
        // owner-flip burst off the same banner entity (capture-fx section of
        // this file; TFCaptureFx.h/.cpp own the actual FX).
        auto& registry = world->GetRegistry();
        for (size_t i = 0; i < m_capBannerEnt.size() && i < m_state.size(); ++i)
        {
            if (m_capBannerEnt[i] == 0u)
                continue;
            const auto e = static_cast<EntityID>(m_capBannerEnt[i]);
            if (!registry.valid(e))
                continue;
            Transform* t = registry.try_get<Transform>(e);
            if (t)
                t->rotation.y += kBannerSpinDegPerSec * dt;
            const int owner = static_cast<int>(m_state[i].owner);
            if (owner != m_capBannerOwner[i])
            {
                const bool firstPaint = (m_capBannerOwner[i] == -2); // spawn-time sentinel, not a real flip
                m_capBannerOwner[i] = owner;
                if (MeshRenderer* mr = registry.try_get<MeshRenderer>(e))
                    mr->materialPath = FactionStructureMaterial(*m_ctx, m_state[i].owner);
                if (!firstPaint && t)
                {
                    const float towerBase[3] = {t->position.x, t->position.y - kBannerHeightM, t->position.z};
                    TFCaptureFx::Get().SpawnOwnerFlipBurst(*m_ctx, towerBase, m_state[i].owner);
                }
            }
            if (t)
            {
                const float towerBase[3] = {t->position.x, t->position.y - kBannerHeightM, t->position.z};
                TFCaptureFx::Get().SubmitTowerBeam(*m_ctx, towerBase, m_state[i].progress, m_state[i].capturing,
                                                   m_state[i].contested);
            }
        }

        // Standing-in-point ground ring under the local player: does its own
        // pawn-position + capturable-point lookup off the public accessors
        // (capture-fx section), so it needs nothing else from this loop.
        TFCaptureFx::Get().UpdateStandingRing(*m_ctx);
    }

    // ---------------------------------------------------------------------------
    // Local player capture-bar feed (all roles with a local player)
    // ---------------------------------------------------------------------------

    void TFRegionSystem::FeedLocalCaptureHUD()
    {
        if (!m_ctx->hud || !m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        PawnInfo pawn{};
        const bool alive = m_ctx->localPlayer != kInvalidPlayer &&
                           m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) && pawn.alive;
        // Same eligibility predicate as the server tick (TickCapture): only pawns
        // with a playable faction are counted on a point, so only they may see the
        // capture indicator — a faction-less player showed a forever-frozen bar in
        // the 2026-07-10 play-test. pawn.faction is the replicated server truth;
        // localFaction covers the frames before the local pawn record mirrors in.
        const FactionId f = alive && IsPlayableFaction(pawn.faction) ? pawn.faction : m_ctx->localFaction;
        if (alive && IsPlayableFaction(f))
        {
            const auto& regions = m_ctx->data->GetContinent().regions;
            const size_t n = std::min(regions.size(), m_state.size());
            for (size_t i = 0; i < n; ++i)
            {
                const RegionDef& def = regions[i];
                if (def.tier == "skyanchor" || def.capturePoints.empty())
                    continue;
                if (!InCaptureRadius(def, pawn.pos))
                    continue;
                const RegionState& st = m_state[i];
                // Show the bar only when the point can react to this player:
                // a capture is live (progress/capturing/contested), or the
                // player's faction could start one here (lattice-linked, not
                // already theirs). Standing on an owned or unlinked point shows
                // nothing — matching exactly what the server tick would do.
                const bool live = st.progress > 0.0f || st.capturing != FactionId::None || st.contested;
                if (!live && !IsCapturable(def.id, f))
                    continue;
                m_ctx->hud->SetCaptureProgress(st.progress, st.capturing, true);
                return;
            }
        }
        m_ctx->hud->SetCaptureProgress(0.0f, FactionId::None, false);
    }

    // ---------------------------------------------------------------------------
    // tf_capture_debug (console) — makes a frozen capture bar diagnosable
    // ---------------------------------------------------------------------------

    std::string TFRegionSystem::DebugCaptureReport() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded() || m_state.empty())
            return "[TF] capture debug: no region data";

        const auto& regions = m_ctx->data->GetContinent().regions;
        const size_t n = std::min(regions.size(), m_state.size());

        // Occupancy sweep — the same pawn set + radius test the 1 Hz tick uses.
        struct Occ
        {
            uint32_t byFaction[static_cast<size_t>(FactionId::COUNT)]{};
            uint32_t noFaction{0};
        };
        std::vector<Occ> occ(n);
        if (m_ctx->players)
        {
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        const RegionDef& def = regions[i];
                        if (def.tier == "skyanchor" || def.capturePoints.empty())
                            continue;
                        if (!InCaptureRadius(def, p.pos))
                            continue;
                        if (IsPlayableFaction(p.faction))
                            ++occ[i].byFaction[static_cast<size_t>(p.faction)];
                        else
                            ++occ[i].noFaction; // the capture tick skips these pawns
                    }
                });
        }

        std::ostringstream os;
        os << "[TF] capture debug (" << (m_ctx->IsAuthority() ? "authority truth" : "client mirror") << "):";

        // 2026-07-10 root-cause tripwire: on an authority with no engine ECS
        // World every pawn position reads (0,0,0), so occupancy is impossible
        // and capture silently never ticks (the headless boot once registered
        // no World service). Say so instead of printing all-zero occupancy.
        if (m_ctx->IsAuthority() && (!m_ctx->engine || m_ctx->engine->GetWorld() == nullptr))
            os << "\n  !! NO ENGINE ECS WORLD: pawn positions are frozen at the origin — occupancy/capture "
                  "CANNOT work (headless boot missing SetWorld?)";
        char buf[96];
        for (size_t i = 0; i < n; ++i)
        {
            const RegionDef& def = regions[i];
            if (def.tier == "skyanchor" || def.capturePoints.empty())
                continue;
            const RegionState& st = m_state[i];
            os << "\n  [" << i << "] " << def.name << " owner=" << FactionTag(st.owner)
               << "  occ MRA:" << occ[i].byFaction[static_cast<size_t>(FactionId::MRA)]
               << " AUC:" << occ[i].byFaction[static_cast<size_t>(FactionId::AUC)]
               << " HLX:" << occ[i].byFaction[static_cast<size_t>(FactionId::HLX)];
            if (occ[i].noFaction > 0)
                os << " none:" << occ[i].noFaction << "(IGNORED)";
            std::snprintf(buf, sizeof(buf), "  capturing=%s %.0f%%%s", FactionTag(st.capturing), st.progress * 100.0f,
                          st.contested ? " CONTESTED" : "");
            os << buf;
            if (!m_ctx->IsAuthority())
            {
                if (st.lastNetAt >= 0.0)
                {
                    std::snprintf(buf, sizeof(buf), "  repAge=%.1fs", std::max(0.0, m_time - st.lastNetAt));
                    os << buf;
                }
                else
                {
                    os << "  repAge=never";
                }
            }
        }

        // Local-player eligibility line: was the tester's pawn even counted?
        if (m_ctx->HasLocalPlayer() && m_ctx->players && m_ctx->localPlayer != kInvalidPlayer)
        {
            PawnInfo pawn{};
            if (m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            {
                char line[160];
                std::snprintf(line, sizeof(line), "\n  local pawn: faction=%s%s %s pos=(%.0f %.0f %.0f)",
                              FactionTag(pawn.faction), IsPlayableFaction(pawn.faction) ? "" : " (NOT counted)",
                              pawn.alive ? "alive" : "dead", pawn.pos[0], pawn.pos[1], pawn.pos[2]);
                os << line;
            }
            else
            {
                os << "\n  local pawn: none (deploy first)";
            }
        }
        return os.str();
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFRegionSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Regions", &m_showDebug))
        {
            ImGui::Text("held: MRA %u  AUC %u  HLX %u  neutral %u   hash 0x%08X", RegionsHeld(FactionId::MRA),
                        RegionsHeld(FactionId::AUC), RegionsHeld(FactionId::HLX), RegionsHeld(FactionId::None),
                        TerritoryHash());
            ImGui::Text("flips %u   ticksTx %u   stateRx %u   tickRx %u   bad %u", m_flips, m_ticksSent, m_stateRx,
                        m_tickRx, m_badPackets);
            if (m_decor)
                ImGui::Text("decor: visible %u   culled %u", m_decor->VisibleDecorCount(), m_decor->CulledDecorCount());
            if (m_domActive)
            {
                float col[4];
                FactionColor(m_domFaction, col);
                ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]), "DOMINION: %s — reset in %.0fs",
                                   FactionName(m_domFaction), std::max(0.0, m_domEndsAt - m_time));
            }
            ImGui::Separator();

            const bool haveData = m_ctx && m_ctx->data && m_ctx->data->IsLoaded();
            for (size_t i = 0; i < m_state.size(); ++i)
            {
                const RegionState& st = m_state[i];
                const RegionDef* def = haveData ? m_ctx->data->GetRegion(static_cast<RegionId>(i)) : nullptr;
                float col[4];
                FactionColor(st.owner, col);
                ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]), "%-3s", FactionTag(st.owner));
                ImGui::SameLine();
                ImGui::Text("%2zu %-20s %-9s", i, def ? def->name.c_str() : "?", def ? def->tier.c_str() : "?");
                if (st.capturing != FactionId::None || st.progress > 0.0f)
                {
                    ImGui::SameLine();
                    char overlay[48];
                    std::snprintf(overlay, sizeof(overlay), "%s %.0f%%%s", FactionTag(st.capturing),
                                  st.progress * 100.0f, st.contested ? " CONTESTED" : "");
                    ImGui::ProgressBar(st.progress, ImVec2(140.0f, 0.0f), overlay);
                }
                else if (st.contested)
                {
                    ImGui::SameLine();
                    ImGui::TextUnformatted("CONTESTED");
                }
            }
        }
        ImGui::End();
#endif
    }

} // namespace Terrafront
