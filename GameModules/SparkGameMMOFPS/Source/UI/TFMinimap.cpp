/**
 * @file TFMinimap.cpp
 * @brief Corner minimap v3 implementation (W13 minimap-v2 lane) — see
 *        UI/TFMinimap.h for the OWNERSHIP note and the drawing contract.
 *
 * Ports the W6/W10/W11/W13 inline minimap that used to live directly in
 * TFHUD::DrawMinimap (UI/TFHUDCombat.cpp) behind this header's free
 * function, so DrawMinimap becomes a thin delegating call. Visual formulas
 * (hex shape, contested pulse, capture-progress ring, conduit lattice,
 * markers) are carried over verbatim from that v2 code; the additions here
 * are the header's zoom table (kTFMinimapZoomSpanM / kTFMinimapRingM, with a
 * per-zoom range ring instead of one fixed radius) and a transient zoom
 * label (TFMinimapState::zoomLabelTtl) instead of an always-on readout.
 *
 * Alert feed: TFGameContext::alerts (Core/TFTypes.h) already publishes the
 * running TFAlertSystem — Main.cpp wires it at boot next to every other
 * ctx-> system pointer this module reads ("minimap-v2 lane (W13): corner-
 * minimap alert overlay"), and every reader here already guards with
 * `if (ctx.alerts)`. That predates this file and does the same job the
 * header's TFMinimapFeed inline-pointer seam was designed for back when
 * TFGameContext had no alert pointer at all — so this function reads
 * ctx.alerts directly and does not touch TFMinimapFeed. The header's
 * TFMinimapFeed seam is left in place (harmless, self-contained) as a
 * fallback path for a future TU that draws the alert flash without a
 * TFGameContext in hand; TFMinimapDraw here has one, so it uses it.
 */
#include "UI/TFMinimap.h"

#include "Data/TFDataTables.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFPingSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFSquadSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "UI/TFKeybinds.h"
#include "UI/TFUiCommon.h"
#include "World/TFAlertSystem.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        constexpr float kMinimapSizePx = 168.0f;
        constexpr float kMinimapPadPx = 16.0f;
        constexpr float kZoomLabelShowSec = 1.6f; ///< how long "SPANm" stays up after a zoom cycle
        constexpr float kZoomLabelFadeSec = 0.4f; ///< tail of the above spent fading out

        ImU32 MinimapFactionCol(FactionId f, float alpha = 1.0f)
        {
            float c[4];
            FactionColor(f, c);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
        }

    } // namespace

    void TFMinimapDraw(TFGameContext& ctx, TFMinimapState& st, const float playerPos[3], bool inputLocked)
    {
        if (!ctx.data || !ctx.data->IsLoaded())
            return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();

        const float mapSz = kMinimapSizePx, pad = kMinimapPadPx;
        const ImVec2 tl(vp->Pos.x + vp->Size.x - mapSz - pad, vp->Pos.y + pad);
        const ImVec2 br(tl.x + mapSz, tl.y + mapSz);
        const ImVec2 mid(tl.x + mapSz * 0.5f, tl.y + mapSz * 0.5f);
        dl->AddRectFilled(tl, br, IM_COL32(8, 10, 14, 150), 6.0f);

        // Zoom cycle (TFKeys seam, ImGui route so typing in chat never eats the
        // key — TFDirectivePanel's toggleKey precedent). `inputLocked` is the
        // caller's chat/text-input-open flag (TFHUD::IsChatOpen() plus
        // WantTextInput at the call site).
        const ImGuiKey zoomKey = TFKeys::ImGuiKeyFor(TFKeys::Action::CycleMinimapZoom);
        if (zoomKey != ImGuiKey_None && !inputLocked && ImGui::IsKeyPressed(zoomKey, false))
        {
            st.zoomIdx = (st.zoomIdx + 1) % kTFMinimapZoomCount;
            st.zoomLabelTtl = kZoomLabelShowSec;
        }
        st.zoomIdx = std::clamp(st.zoomIdx, 0, kTFMinimapZoomCount - 1);
        const float span = kTFMinimapZoomSpanM[st.zoomIdx];
        st.zoomLabelTtl = std::max(0.0f, st.zoomLabelTtl - ImGui::GetIO().DeltaTime);

        // Player-centered view, north (+Z) up, world frame (no map rotation).
        // Kept north-up rather than switching to player-up: it matches
        // TFMapScreen's static orientation and the compass strip's world-frame
        // headings 1:1 (no separate mental mapping between the two UIs), it's
        // cheaper (every marker blits at its world-delta offset — no per-marker
        // rotation matrix), and it was the W6 baseline this lane extends rather
        // than replaces. The player arrow below is the one element that visibly
        // turns, which is enough to read heading at a glance.
        const float cx = playerPos[0];
        const float cz = playerPos[2];
        const float half = span * 0.5f;
        const float scale = mapSz / span;
        const auto toMini = [&](float wx, float wz) -> ImVec2
        { return ImVec2(mid.x + (wx - cx) * scale, mid.y - (wz - cz) * scale); };
        const float nowT = static_cast<float>(ImGui::GetTime());

        dl->PushClipRect(tl, br, true);

        // Subtle fixed-radius range ring — spatial reference at a glance; fades
        // out once a zoom level makes it too big (or too small) to be useful.
        const float ringR = kTFMinimapRingM[st.zoomIdx] * scale;
        if (ringR > 3.0f && ringR < mapSz * 0.62f)
            dl->AddCircle(mid, ringR, IM_COL32(255, 255, 255, 35), 28, 1.0f);

        TFRegionSystem* rs = ctx.regions;

        // Conduit lattice links (drawn under the hexes — TFMapScreen ordering).
        // Same-faction-linked pairs pop the owner color; everything else is a
        // faint gray thread.
        for (const RegionDef& rd : ctx.data->GetContinent().regions)
        {
            if (std::fabs(rd.centerX - cx) > half + 60.0f || std::fabs(rd.centerZ - cz) > half + 60.0f)
                continue;
            const ImVec2 a = toMini(rd.centerX, rd.centerZ);
            for (RegionId nb : rd.neighbors)
            {
                if (nb <= rd.id)
                    continue; // each pair once
                const RegionDef* nd = ctx.data->GetRegion(nb);
                if (!nd)
                    continue;
                if (std::fabs(nd->centerX - cx) > half + 60.0f || std::fabs(nd->centerZ - cz) > half + 60.0f)
                    continue;
                const ImVec2 b = toMini(nd->centerX, nd->centerZ);
                ImU32 col = IM_COL32(120, 128, 140, 70);
                if (rs)
                {
                    const FactionId fa = rs->OwnerOf(rd.id);
                    const FactionId fb = rs->OwnerOf(nb);
                    if (fa != FactionId::None && fa == fb)
                        col = MinimapFactionCol(fa, 0.5f);
                }
                dl->AddLine(a, b, col, 1.4f);
            }
        }

        // Continent-alert overlay: ctx.alerts is additive-null until Main.cpp's
        // boot order constructs TFAlertSystem, so this degrades to a silent
        // no-op until then (same guard as every other ctx-> system pointer
        // this file reads).
        bool alertActive = false;
        RegionId alertTarget = kInvalidRegion;
        if (ctx.alerts)
        {
            TF_AlertState av{};
            ctx.alerts->GetView(av);
            if (static_cast<TFAlertPhase>(av.phase) == TFAlertPhase::Running)
            {
                alertActive = true;
                alertTarget = av.regionId; // kInvalidRegion for TerritoryRush (no single target)
            }
        }

        // Region hexes (owner-tinted, tier-sized), contested pulse, capture-
        // progress ring — same visual formulas as TFMapScreen's full map so the
        // two views read as one system.
        for (const RegionDef& r : ctx.data->GetContinent().regions)
        {
            if (std::fabs(r.centerX - cx) > half + 30.0f || std::fabs(r.centerZ - cz) > half + 30.0f)
                continue;
            const FactionId owner = rs ? rs->OwnerOf(r.id) : r.homeFaction;
            const ImVec2 p = toMini(r.centerX, r.centerZ);
            const float hexR = (r.tier == "skyanchor") ? 7.0f : (r.tier == "facility" ? 6.0f : 5.0f);

            FactionId capturing = FactionId::None;
            bool contested = false;
            const float progress = rs ? rs->CaptureProgress(r.id, capturing, contested) : 0.0f;

            ImVec2 corners[6];
            TFUi::HexCorners(p, hexR, corners);
            float fillA = (owner == FactionId::None) ? 0.22f : 0.42f;
            if (contested)
                fillA = 0.35f + 0.25f * std::sin(nowT * 6.0f); // capture-point pulse
            dl->AddConvexPolyFilled(corners, 6, MinimapFactionCol(owner, fillA));

            ImU32 outline = IM_COL32(210, 214, 220, 110);
            float outlineThick = 1.2f;
            if (contested)
            {
                outline = IM_COL32(255, 255, 255, static_cast<int>(130.0f + 100.0f * std::sin(nowT * 6.0f)));
                outlineThick = 2.0f;
            }
            dl->AddPolyline(corners, 6, outline, ImDrawFlags_Closed, outlineThick);

            if (progress > 0.001f && capturing != FactionId::None)
            {
                dl->PathArcTo(p, hexR * 0.6f, -TFUi::kPi * 0.5f, -TFUi::kPi * 0.5f + progress * 2.0f * TFUi::kPi, 16);
                dl->PathStroke(MinimapFactionCol(capturing, contested ? 0.6f : 0.95f), 0, 2.0f);
            }

            // FacilityControl alert target ring (TerritoryRush has no single
            // region here — its cue is the border flash after PopClipRect).
            if (alertActive && alertTarget != kInvalidRegion && alertTarget == r.id)
            {
                const float pulse = 0.5f + 0.5f * std::sin(nowT * 5.0f);
                dl->AddCircle(p, hexR + 3.0f + pulse * 3.0f, IM_COL32(255, 175, 40, static_cast<int>(140 + 100 * pulse)),
                              0, 2.2f);
            }
        }

        // Same-faction deployables (client mirror on pure clients).
        const FactionId myFaction = ctx.localFaction;
        if (ctx.deployables)
        {
            ctx.deployables->ForEachDeployable(
                [&](const TFDeployableView& d)
                {
                    if (d.faction != myFaction)
                        return;
                    if (std::fabs(d.pos[0] - cx) > half || std::fabs(d.pos[2] - cz) > half)
                        return;
                    const ImVec2 p = toMini(d.pos[0], d.pos[2]);
                    dl->AddRectFilled(ImVec2(p.x - 2.5f, p.y - 2.5f), ImVec2(p.x + 2.5f, p.y + 2.5f),
                                      MinimapFactionCol(myFaction, 0.95f), 1.0f);
                    dl->AddRect(ImVec2(p.x - 2.5f, p.y - 2.5f), ImVec2(p.x + 2.5f, p.y + 2.5f),
                                IM_COL32(240, 240, 240, 160), 1.0f);
                });
        }

        // Nearby friendly vehicles: yaw-oriented triangle when mobile, a dimmer
        // square when deployed/stationary (ctx.vehicles is already published —
        // TFVehicleInfo serves the same authority/mirror split as every other
        // client-visible system this file reads).
        if (ctx.vehicles && myFaction != FactionId::None)
        {
            ctx.vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                {
                    if (v.faction != myFaction)
                        return;
                    if (std::fabs(v.pos[0] - cx) > half || std::fabs(v.pos[2] - cz) > half)
                        return;
                    const ImVec2 vp2 = toMini(v.pos[0], v.pos[2]);
                    const ImU32 col = MinimapFactionCol(myFaction, 0.92f);
                    if (v.deployed)
                    {
                        dl->AddRectFilled(ImVec2(vp2.x - 3.5f, vp2.y - 3.5f), ImVec2(vp2.x + 3.5f, vp2.y + 3.5f), col,
                                          1.0f);
                        dl->AddRect(ImVec2(vp2.x - 3.5f, vp2.y - 3.5f), ImVec2(vp2.x + 3.5f, vp2.y + 3.5f),
                                    IM_COL32(20, 20, 24, 200), 1.0f);
                    }
                    else
                    {
                        const ImVec2 fwd(std::sin(v.yaw), -std::cos(v.yaw));
                        const ImVec2 right(fwd.y, -fwd.x);
                        const ImVec2 vtip(vp2.x + fwd.x * 5.5f, vp2.y + fwd.y * 5.5f);
                        const ImVec2 backL(vp2.x - fwd.x * 3.5f + right.x * 3.5f, vp2.y - fwd.y * 3.5f + right.y * 3.5f);
                        const ImVec2 backR(vp2.x - fwd.x * 3.5f - right.x * 3.5f, vp2.y - fwd.y * 3.5f - right.y * 3.5f);
                        dl->AddTriangleFilled(vtip, backL, backR, col);
                        dl->AddTriangle(vtip, backL, backR, IM_COL32(20, 20, 24, 200), 1.2f);
                    }
                });
        }

        // Friendly pawns; squadmates pop as bright-green diamonds (distinct
        // from the plain-friendly dot) — IsLocalSquadMember resolves other
        // players on pure clients through the replicated mirror roster.
        if (ctx.players && myFaction != FactionId::None)
        {
            const PlayerId local = ctx.localPlayer;
            ctx.players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    if (p.owner == local || p.faction != myFaction)
                        return;
                    if (std::fabs(p.pos[0] - cx) > half || std::fabs(p.pos[2] - cz) > half)
                        return;
                    const ImVec2 mp = toMini(p.pos[0], p.pos[2]);
                    const bool squadmate = ctx.squads && ctx.squads->IsLocalSquadMember(p.owner);
                    if (squadmate)
                    {
                        const float wr = 3.6f;
                        const ImVec2 dia[4] = {ImVec2(mp.x, mp.y - wr), ImVec2(mp.x + wr, mp.y),
                                               ImVec2(mp.x, mp.y + wr), ImVec2(mp.x - wr, mp.y)};
                        dl->AddConvexPolyFilled(dia, 4, IM_COL32(110, 235, 140, 240));
                        dl->AddPolyline(dia, 4, IM_COL32(10, 40, 15, 200), ImDrawFlags_Closed, 1.2f);
                    }
                    else
                    {
                        dl->AddCircleFilled(mp, 2.5f, MinimapFactionCol(myFaction, 0.9f));
                    }
                });
        }

        // Squad waypoint (TFSquadSystem): green diamond + a slow gold pulse
        // ring so it reads over the squadmate diamonds; clamped to the frame
        // edge along the waypoint direction when it falls outside the current
        // zoom span.
        if (ctx.squads)
        {
            float wpPos[3];
            if (ctx.squads->GetLocalWaypoint(wpPos))
            {
                const float dx = wpPos[0] - cx;
                const float dz = wpPos[2] - cz;
                const bool offscreen = std::fabs(dx) > half || std::fabs(dz) > half;
                ImVec2 wp2 = toMini(wpPos[0], wpPos[2]);
                if (offscreen)
                {
                    const float len = std::sqrt(dx * dx + dz * dz);
                    if (len > 0.001f)
                    {
                        const float ux = dx / len, uz = dz / len;
                        const float edge = half - 12.0f;
                        const float tX = edge / std::max(std::fabs(ux), 1e-4f);
                        const float tZ = edge / std::max(std::fabs(uz), 1e-4f);
                        const float t = std::min(tX, tZ);
                        wp2 = toMini(cx + ux * t, cz + uz * t);
                    }
                }
                const float wr = 5.0f;
                const ImVec2 dia[4] = {ImVec2(wp2.x, wp2.y - wr), ImVec2(wp2.x + wr, wp2.y),
                                       ImVec2(wp2.x, wp2.y + wr), ImVec2(wp2.x - wr, wp2.y)};
                dl->AddConvexPolyFilled(dia, 4, IM_COL32(110, 235, 140, offscreen ? 200 : 235));
                dl->AddPolyline(dia, 4, IM_COL32(16, 40, 24, 230), ImDrawFlags_Closed, 1.6f);
                dl->AddCircle(wp2, wr + 2.5f + 1.5f * std::sin(nowT * 5.0f), IM_COL32(255, 215, 90, 150), 0, 1.3f);
            }
        }

        // Squad pings: type-colored diamonds.
        if (ctx.pings)
        {
            ctx.pings->ForEachActivePing(
                [&](const TFPingView& pg)
                {
                    if (std::fabs(pg.pos[0] - cx) > half || std::fabs(pg.pos[2] - cz) > half)
                        return;
                    const ImVec2 p = toMini(pg.pos[0], pg.pos[2]);
                    float c[4];
                    PingColor(pg.type, c);
                    const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], 0.95f));
                    const ImVec2 pts[4] = {ImVec2(p.x, p.y - 4.5f), ImVec2(p.x + 4.5f, p.y), ImVec2(p.x, p.y + 4.5f),
                                           ImVec2(p.x - 4.5f, p.y)};
                    dl->AddConvexPolyFilled(pts, 4, col);
                    dl->AddPolyline(pts, 4, IM_COL32(15, 15, 20, 220), ImDrawFlags_Closed, 1.0f);
                });
        }

        // Local player: filled directional arrow from the camera heading (see
        // the north-up justification above — this is the one marker that turns).
        SparkEngineCamera* cam = ctx.world ? ctx.world->GetCamera() : nullptr;
        const float yaw = cam ? cam->GetRotation().y : 0.0f;
        const ImVec2 fwd(std::sin(yaw), -std::cos(yaw));
        const ImVec2 right(fwd.y, -fwd.x);
        const ImVec2 tip(mid.x + fwd.x * 10.0f, mid.y + fwd.y * 10.0f);
        const ImVec2 baseL(mid.x - fwd.x * 4.0f + right.x * 3.5f, mid.y - fwd.y * 4.0f + right.y * 3.5f);
        const ImVec2 baseR(mid.x - fwd.x * 4.0f - right.x * 3.5f, mid.y - fwd.y * 4.0f - right.y * 3.5f);
        dl->AddTriangleFilled(tip, baseL, baseR, IM_COL32(255, 255, 255, 240));
        dl->AddTriangle(tip, baseL, baseR, IM_COL32(15, 15, 20, 220), 1.5f);
        dl->AddCircleFilled(mid, 2.0f, IM_COL32(255, 255, 255, 255));

        dl->PopClipRect();

        dl->AddRect(tl, br, IM_COL32(90, 100, 110, 180), 6.0f);
        dl->AddText(ImVec2(mid.x - 4.0f, tl.y + 3.0f), IM_COL32(220, 225, 230, 200), "N");

        // Continent-alert border flash — any running alert pulses the frame
        // (TerritoryRush's only cue, since it has no single target region).
        if (alertActive)
        {
            const float pulse = 0.5f + 0.5f * std::sin(nowT * 5.0f);
            dl->AddRect(tl, br, IM_COL32(255, 175, 40, static_cast<int>(110 + 120 * pulse)), 6.0f, 0,
                        2.0f + pulse * 1.5f);
        }

        // Zoom-span readout: a transient label after a zoom cycle (rather than
        // an always-on one) so the corner stays quiet outside of that moment.
        if (st.zoomLabelTtl > 0.0f)
        {
            const float alpha = std::min(1.0f, st.zoomLabelTtl / kZoomLabelFadeSec);
            char zoomLabel[8];
            std::snprintf(zoomLabel, sizeof(zoomLabel), "%.0fm", span);
            dl->AddText(ImVec2(tl.x + 4.0f, br.y - 14.0f), IM_COL32(200, 205, 210, static_cast<int>(210 * alpha)),
                        zoomLabel);
        }
    }

#else // !SPARK_HAS_IMGUI — headless: minimap is a no-op

    void TFMinimapDraw(TFGameContext&, TFMinimapState&, const float[3], bool) {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
