/**
 * @file TFMapScreen.cpp
 * @brief W2 continent map: fullscreen hex-grid overlay of the Cindral Wastes
 *        lattice — ownership fills, contested pulses, capture rings, conduit
 *        lines, legend with per-faction region counts, hover tooltips, and
 *        click-to-deploy (dead) / deploy-hint (alive) interaction.
 *        W6: friendly-presence badges per region (replicated pawn data,
 *        own faction only) and a you-are-here marker on the local region.
 *
 * Consumes the W2 TFRegionSystem contract (OwnerOf / IsCapturable /
 * CaptureProgress / RegionsHeld / CanSpawnAt) — identical accessors on server
 * and on the client mirror fed by TF_RegionState / TF_CaptureTick.
 */
#include "UI/TFMapScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFUiCommon.h"
#include "World/TFRegionSystem.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace Terrafront
{

    namespace
    {

        // Windows virtual-key codes (numeric so no <windows.h> dependency here —
        // TFClientNet.cpp convention).
        constexpr int kVkM = 'M';
        constexpr int kVkEscape = 0x1B;

#ifdef SPARK_HAS_IMGUI
        constexpr float kHexSizeMin = 26.0f; // corner radius, px
        constexpr float kHexSizeMax = 52.0f; // ~90 px hex width at the cap
#endif

    } // namespace

    TFMapScreen::TFMapScreen() = default;
    TFMapScreen::~TFMapScreen()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFMapScreen::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFMapScreen initialized");
        return true;
    }

    void TFMapScreen::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFMapScreen::Shutdown()
    {
        m_open = false;
        m_initialized = false;
    }

    void TFMapScreen::RenderDebugUI() {}

    // ---------------------------------------------------------------------------
    // Open/close + input
    // ---------------------------------------------------------------------------

    bool TFMapScreen::LocalPawnAlive() const
    {
        if (!m_ctx || !m_ctx->players)
            return false;
        PlayerId pid = m_ctx->localPlayer;
        if (pid == kInvalidPlayer && m_ctx->clientNet)
            pid = m_ctx->clientNet->LocalPlayerId();
        if (pid == kInvalidPlayer)
            return false;
        PawnInfo p{};
        return m_ctx->players->GetPawnByPlayer(pid, p) && p.alive;
    }

    void TFMapScreen::Open()
    {
        if (m_open || !m_initialized)
            return;
        m_open = true;
        // Free the cursor so regions are clickable; TFClientNet re-captures on the
        // next dead->alive transition, we re-capture on Close() while alive.
        if (m_ctx && m_ctx->engine)
        {
            if (InputManager* in = m_ctx->engine->GetInput())
            {
                if (in->IsMouseCaptured())
                    in->CaptureMouse(false);
            }
        }
    }

    void TFMapScreen::Close()
    {
        if (!m_open)
            return;
        m_open = false;
        if (m_ctx && m_ctx->engine && LocalPawnAlive())
        {
            if (InputManager* in = m_ctx->engine->GetInput())
                in->CaptureMouse(true);
        }
    }

    void TFMapScreen::Toggle()
    {
        if (m_open)
            Close();
        else
            Open();
    }

    void TFMapScreen::Update(float deltaTime)
    {
        m_time += deltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        InputManager* in = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!in)
            return;
        if (in->WasKeyPressed(kVkM))
            Toggle();
        if (m_open && in->WasKeyPressed(kVkEscape))
            Close();
    }

    void TFMapScreen::SendRegionSpawnRequest(RegionId region)
    {
        if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
            return;
        TF_SpawnRequest rq{};
        ClassId cls = ClassId::Striker;
        if (m_ctx->spawnUI)
            cls = m_ctx->spawnUI->SelectedClass();
        rq.classId = static_cast<uint8_t>(cls);
        rq.spawnKind = 1; // region spawn
        rq.regionId = region;
        m_ctx->clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] map deploy request -> region %u", static_cast<unsigned>(region));
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFMapScreen::RenderUI()
    {
        if (!m_open || !m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
        {
            return; // no tables -> nothing to draw; stay open, data may hot-load
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("##TFMapScreen", nullptr, flags))
        {
            ImGui::End();
            return;
        }
        DrawMapContents();
        ImGui::End();
    }

    void TFMapScreen::DrawMapContents()
    {
        using namespace TFUi;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const auto& regions = m_ctx->data->GetContinent().regions;
        TFRegionSystem* rs = m_ctx->regions;
        const FactionId myFaction = m_ctx->localFaction;
        const bool dead = !LocalPawnAlive();

        // Backdrop
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(8, 10, 14, 235));

        // ---- layout: auto-fit the axial bounds into the viewport ---------------
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (const RegionDef& rd : regions)
        {
            const ImVec2 p = AxialToPixel(rd.hexQ, rd.hexR, 1.0f);
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        if (regions.empty())
        {
            AddTextCentered(dl, 24.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            IM_COL32(220, 220, 220, 220), "NO REGION DATA");
            return;
        }
        const float availW = vp->Size.x * 0.78f;
        const float availH = vp->Size.y * 0.66f;
        m_hexSize = std::clamp(std::min(availW / (maxX - minX + 2.6f), availH / (maxY - minY + 2.8f)), kHexSizeMin,
                               kHexSizeMax);
        const ImVec2 mapCenter(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.52f);
        m_originX = mapCenter.x - (minX + maxX) * 0.5f * m_hexSize;
        m_originY = mapCenter.y - (minY + maxY) * 0.5f * m_hexSize;

        auto screenOf = [&](const RegionDef& rd)
        {
            const ImVec2 p = AxialToPixel(rd.hexQ, rd.hexR, m_hexSize);
            return ImVec2(m_originX + p.x, m_originY + p.y);
        };

        // ---- hover: mouse -> axial -> region ------------------------------------
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hq = 0, hr = 0;
        PixelToAxial(mouse.x - m_originX, mouse.y - m_originY, m_hexSize, hq, hr);
        m_hovered = kInvalidRegion;
        const RegionDef* hoveredDef = nullptr;
        for (const RegionDef& rd : regions)
        {
            if (rd.hexQ == hq && rd.hexR == hr)
            {
                m_hovered = rd.id;
                hoveredDef = &rd;
                break;
            }
        }

        // ---- conduit lines (under the hexes) ------------------------------------
        for (const RegionDef& rd : regions)
        {
            const ImVec2 a = screenOf(rd);
            for (RegionId nb : rd.neighbors)
            {
                if (nb <= rd.id)
                    continue; // each pair once
                const RegionDef* nd = m_ctx->data->GetRegion(nb);
                if (!nd)
                    continue;
                const ImVec2 b = screenOf(*nd);
                ImU32 col = IM_COL32(120, 128, 140, 90);
                if (rs)
                {
                    const FactionId fa = rs->OwnerOf(rd.id);
                    const FactionId fb = rs->OwnerOf(nb);
                    if (fa != FactionId::None && fa == fb)
                        col = FactionCol(fa, 0.45f); // linked friendly conduit
                }
                dl->AddLine(a, b, col, 3.0f);
            }
        }

        // ---- live intel (W6): friendly presence per region + you-are-here -------
        // Pawns are replicated for every player on all roles (TFPlayerSystem
        // client records mirror the RemotePawn store), so this is real
        // client-visible data — friendly counts only, no enemy intel.
        std::unordered_map<RegionId, uint32_t> friendlyCount;
        RegionId myRegion = kInvalidRegion;
        if (m_ctx->players && myFaction != FactionId::None && !regions.empty())
        {
            PlayerId local = m_ctx->localPlayer;
            if (local == kInvalidPlayer && m_ctx->clientNet)
                local = m_ctx->clientNet->LocalPlayerId();
            const auto nearestRegion = [&](const float pos[3])
            {
                RegionId best = kInvalidRegion;
                float bestD2 = 1e18f;
                for (const RegionDef& rd : regions)
                {
                    const float dx = rd.centerX - pos[0];
                    const float dz = rd.centerZ - pos[2];
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        best = rd.id;
                    }
                }
                return best;
            };
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    if (p.faction != myFaction)
                        return;
                    const RegionId r = nearestRegion(p.pos);
                    if (r == kInvalidRegion)
                        return;
                    ++friendlyCount[r];
                    if (p.owner == local)
                        myRegion = r;
                });
        }

        // ---- hexes ---------------------------------------------------------------
        for (const RegionDef& rd : regions)
        {
            const ImVec2 c = screenOf(rd);
            const FactionId owner = rs ? rs->OwnerOf(rd.id) : rd.homeFaction;

            FactionId capturing = FactionId::None;
            bool contested = false;
            const float progress = rs ? rs->CaptureProgress(rd.id, capturing, contested) : 0.0f;

            ImVec2 corners[6];
            HexCorners(c, m_hexSize * 0.92f, corners);

            float fillA = (owner == FactionId::None) ? 0.28f : 0.55f;
            if (contested)
                fillA = 0.40f + 0.20f * std::sin(m_time * 6.0f); // contested pulse
            dl->AddConvexPolyFilled(corners, 6, FactionCol(owner, fillA));

            // Outline: hovered bright; deploy hint gold; default faint.
            ImU32 outline = IM_COL32(210, 214, 220, 70);
            float thick = 1.5f;
            if (rd.id == m_deployHint)
            {
                outline = IM_COL32(255, 200, 60, 220);
                thick = 3.0f;
            }
            if (rd.id == m_hovered)
            {
                outline = IM_COL32(255, 255, 255, 230);
                thick = 3.0f;
            }
            dl->AddPolyline(corners, 6, outline, ImDrawFlags_Closed, thick);

            // Capture progress ring in the capturing faction's color.
            if (progress > 0.001f && capturing != FactionId::None)
            {
                dl->PathArcTo(c, m_hexSize * 0.62f, -kPi * 0.5f, -kPi * 0.5f + progress * 2.0f * kPi, 24);
                dl->PathStroke(FactionCol(capturing, contested ? 0.55f : 0.95f), 0, 4.5f);
            }

            // Skyanchor home marker: triangle above center.
            if (rd.tier == "skyanchor")
            {
                const float s = m_hexSize * 0.24f;
                dl->AddTriangleFilled(ImVec2(c.x, c.y - s * 1.5f), ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y),
                                      FactionCol(rd.homeFaction, 0.95f));
            }

            // Tier glyph inside, name (truncated) below.
            AddTextCentered(dl, 12.0f, ImVec2(c.x, c.y + m_hexSize * 0.38f), IM_COL32(235, 235, 235, 190),
                            TierTag(rd.tier));
            char name[20];
            std::snprintf(name, sizeof(name), "%.16s%s", rd.name.c_str(), rd.name.size() > 16 ? ".." : "");
            AddTextCentered(dl, 12.0f, ImVec2(c.x, c.y + m_hexSize * 1.06f), IM_COL32(210, 210, 214, 200), name);

            // W6: friendly presence badge (top-left of the hex) + you-are-here.
            if (auto it = friendlyCount.find(rd.id); it != friendlyCount.end())
            {
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), "%u", it->second);
                const ImVec2 bp(c.x - m_hexSize * 0.52f, c.y - m_hexSize * 0.52f);
                dl->AddCircleFilled(bp, 8.0f, FactionCol(myFaction, 0.85f));
                dl->AddCircle(bp, 8.0f, IM_COL32(15, 17, 22, 200), 0, 1.5f);
                AddTextCentered(dl, 12.0f, bp, IM_COL32(250, 250, 250, 240), cnt);
            }
            if (rd.id == myRegion)
            {
                dl->AddCircle(c, m_hexSize * 0.30f, IM_COL32(255, 255, 255, 220), 0, 2.5f);
                AddTextCentered(dl, 11.0f, ImVec2(c.x, c.y - m_hexSize * 0.12f), IM_COL32(255, 255, 255, 230), "YOU");
            }

            // Deployable highlight while dead.
            if (dead && rs && myFaction != FactionId::None && rs->CanSpawnAt(rd.id, myFaction))
            {
                ImVec2 big[6];
                HexCorners(c, m_hexSize * 1.02f, big);
                const float a = 0.55f + 0.35f * std::sin(m_time * 4.0f);
                dl->AddPolyline(big, 6, ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f, 0.95f, 0.55f, a)),
                                ImDrawFlags_Closed, 2.0f);
            }
        }

        // ---- title + legend -------------------------------------------------------
        AddTextCentered(dl, 30.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 34.0f),
                        IM_COL32(235, 235, 235, 235), "CINDRAL WASTES  -  TERRITORY");
        AddTextCentered(dl, 14.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 60.0f),
                        IM_COL32(170, 174, 180, 200),
                        dead ? "Click a highlighted region to deploy  |  M / Esc closes"
                             : "Click a region to set a rally hint  |  M / Esc closes");

        {
            const ImVec2 base(vp->Pos.x + 28.0f, vp->Pos.y + vp->Size.y - 118.0f);
            dl->AddRectFilled(ImVec2(base.x - 10.0f, base.y - 10.0f), ImVec2(base.x + 250.0f, base.y + 96.0f),
                              IM_COL32(14, 16, 20, 200), 4.0f);
            float y = base.y;
            for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
            {
                dl->AddRectFilled(ImVec2(base.x, y + 2.0f), ImVec2(base.x + 14.0f, y + 14.0f), FactionCol(f, 0.95f),
                                  2.0f);
                char line[80];
                std::snprintf(line, sizeof(line), "%s  %s  -  %u regions", FactionTag(f), FactionName(f),
                              rs ? rs->RegionsHeld(f) : 0u);
                dl->AddText(ImVec2(base.x + 22.0f, y), IM_COL32(225, 225, 228, 220), line);
                y += 22.0f;
            }
            if (m_deployHint != kInvalidRegion)
            {
                const RegionDef* hd = m_ctx->data->GetRegion(m_deployHint);
                char line[80];
                std::snprintf(line, sizeof(line), "Rally hint: %s", hd ? hd->name.c_str() : "?");
                dl->AddText(ImVec2(base.x, y + 4.0f), IM_COL32(255, 200, 60, 220), line);
            }
        }

        // ---- hover tooltip ---------------------------------------------------------
        if (hoveredDef)
        {
            const RegionDef& rd = *hoveredDef;
            const FactionId owner = rs ? rs->OwnerOf(rd.id) : rd.homeFaction;
            FactionId capturing = FactionId::None;
            bool contested = false;
            const float progress = rs ? rs->CaptureProgress(rd.id, capturing, contested) : 0.0f;

            ImGui::BeginTooltip();
            ImGui::Text("%s", rd.name.c_str());
            ImGui::Text("%s  |  %s", TierTag(rd.tier), FactionName(owner));
            ImGui::Text("Flux: %d / tick", rd.fluxPerTick);
            if (auto it = friendlyCount.find(rd.id); it != friendlyCount.end())
                ImGui::Text("Friendlies here: %u", it->second);
            if (progress > 0.001f && capturing != FactionId::None)
                ImGui::Text("Capturing: %s %d%%%s", FactionTag(capturing), static_cast<int>(progress * 100.0f),
                            contested ? "  (CONTESTED)" : "");
            if (rs && myFaction != FactionId::None)
            {
                if (dead && rs->CanSpawnAt(rd.id, myFaction))
                    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "DEPLOY AVAILABLE - click");
                else if (rs->IsCapturable(rd.id, myFaction))
                    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "Conduit-linked: capturable");
            }
            ImGui::EndTooltip();
        }

        // ---- click ------------------------------------------------------------------
        if (hoveredDef && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered())
        {
            if (dead)
            {
                if (rs && myFaction != FactionId::None && rs->CanSpawnAt(hoveredDef->id, myFaction))
                {
                    SendRegionSpawnRequest(hoveredDef->id);
                    Close();
                }
            }
            else
            {
                m_deployHint = hoveredDef->id; // W3 redeploy hint
            }
        }
    }

#else // !SPARK_HAS_IMGUI — headless: map is state-only

    void TFMapScreen::RenderUI() {}
    void TFMapScreen::DrawMapContents() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
