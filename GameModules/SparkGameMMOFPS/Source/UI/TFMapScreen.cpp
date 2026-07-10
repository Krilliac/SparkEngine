/**
 * @file TFMapScreen.cpp
 * @brief W2 continent map: fullscreen hex-grid overlay of the Cindral Wastes
 *        lattice — ownership fills, contested pulses, capture rings, conduit
 *        lines, legend with per-faction region counts, hover tooltips, and
 *        click-to-deploy (dead) / deploy-hint (alive) interaction.
 *        W6: friendly-presence badges per region (replicated pawn data,
 *        own faction only) and a you-are-here marker on the local region.
 *        W7 (ui-map-keys): shared keybind table, squad-member badges,
 *        click-to-inspect side panel, and the server-validated redeploy flow
 *        (TFRedeployRules + TF_RedeployRequest/Reply, reserved 0x5430 block).
 *
 * Consumes the W2 TFRegionSystem contract (OwnerOf / IsCapturable /
 * CaptureProgress / RegionsHeld / CanSpawnAt) — identical accessors on server
 * and on the client mirror fed by TF_RegionState / TF_CaptureTick.
 */
#include "UI/TFMapScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFRedeployRules.h"
#include "Game/TFSquadSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFHUD.h"
#include "UI/TFKeybinds.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFUiCommon.h"
#include "World/TFRegionSystem.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Terrafront
{

    namespace
    {

#ifdef SPARK_HAS_IMGUI
        constexpr float kHexSizeMin = 26.0f; // corner radius, px
        constexpr float kHexSizeMax = 52.0f; // ~90 px hex width at the cap
#endif

        constexpr float kStatusTTLSec = 4.0f; // redeploy status line lifetime

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

        // W7: exec-harness/headless entry into the same redeploy state machine
        // the inspect-panel button drives (SimpleConsole pattern, TFRegionSystem).
        auto& console = Spark::SimpleConsole::GetInstance();
        if (m_ctx->HasLocalPlayer() && !console.HasCommand("tf_redeploy"))
        {
            console.RegisterCommand(
                "tf_redeploy",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized || !m_ctx)
                        return "[TF] map screen not ready";
                    if (args.empty())
                        return "[TF] usage: tf_redeploy <regionId>";
                    const RegionId region = static_cast<RegionId>(std::strtoul(args[0].c_str(), nullptr, 10));
                    const uint8_t reason = CanRedeployTo(region);
                    if (reason != kTFRedeployOk)
                        return std::string("[TF] redeploy refused: ") + TFRedeployRules::ReasonText(reason);
                    StartRedeploy(region);
                    return "[TF] redeploy countdown started -> region " + std::string(args[0]);
                },
                "Redeploy to a friendly non-contested region (server validated)", "TERRAFRONT",
                "tf_redeploy <regionId>");
            m_redeployCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFMapScreen initialized");
        return true;
    }

    void TFMapScreen::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFMapScreen::Shutdown()
    {
        if (m_redeployCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_redeploy");
            m_redeployCmd = false;
        }
        m_open = false;
        m_redeployPending = false;
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

        TickRedeploy(deltaTime);

        if (!m_ctx->InWorld())
            return; // pre-onboarding (login/char-select): no map hotkeys

        InputManager* in = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!in)
            return;
        // Chat owns the keyboard while its input line is focused — typing an
        // 'm' must not toggle the map (TFKeybinds Action route reads raw VKs).
        const bool chatOpen = m_ctx->hud && m_ctx->hud->IsChatOpen();
        if (!chatOpen && TFKeys::WasActionPressed(*in, TFKeys::Action::OpenMap))
            Toggle();
        // Esc closes the chat first (TFHUD handles that); only an unfocused map
        // consumes CloseOverlay here.
        if (m_open && !chatOpen && TFKeys::WasActionPressed(*in, TFKeys::Action::CloseOverlay))
            Close();
    }

    // ---------------------------------------------------------------------------
    // W7 redeploy state machine (state-only — works headless; ImGui only draws it)
    // ---------------------------------------------------------------------------

    bool TFMapScreen::LocalSeated() const
    {
        if (!m_ctx || !m_ctx->vehicles)
            return false;
        PlayerId pid = m_ctx->localPlayer;
        if (pid == kInvalidPlayer && m_ctx->clientNet)
            pid = m_ctx->clientNet->LocalPlayerId();
        return pid != kInvalidPlayer && m_ctx->vehicles->IsSeated(pid);
    }

    uint8_t TFMapScreen::CanRedeployTo(RegionId region) const
    {
        if (!m_ctx)
            return kTFRedeployBadRegion;
        if (m_cooldownLeft > 0.0f)
            return kTFRedeployCooldown;
        PlayerId pid = m_ctx->localPlayer;
        if (pid == kInvalidPlayer && m_ctx->clientNet)
            pid = m_ctx->clientNet->LocalPlayerId();
        return TFRedeployRules::CanRedeploy(*m_ctx, pid, m_ctx->localFaction, region);
    }

    void TFMapScreen::SetStatus(const char* text, bool error)
    {
        std::snprintf(m_status, sizeof(m_status), "%s", text ? text : "");
        m_statusTTL = kStatusTTLSec;
        m_statusIsError = error;
    }

    void TFMapScreen::StartRedeploy(RegionId region)
    {
        m_redeployPending = true;
        m_redeployTarget = region;
        m_redeployCountdown = TFRedeployRules::kTFRedeployCountdownSec;
        const RegionDef* rd = (m_ctx && m_ctx->data) ? m_ctx->data->GetRegion(region) : nullptr;
        char line[96];
        std::snprintf(line, sizeof(line), "Redeploying to %s...", rd ? rd->name.c_str() : "?");
        SetStatus(line, false);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] redeploy countdown started -> region %u",
                       static_cast<unsigned>(region));
    }

    void TFMapScreen::CancelRedeploy(const char* why)
    {
        if (!m_redeployPending)
            return;
        m_redeployPending = false;
        m_redeployTarget = kInvalidRegion;
        m_redeployCountdown = 0.0f;
        char line[96];
        std::snprintf(line, sizeof(line), "Redeploy cancelled: %s", why ? why : "");
        SetStatus(line, true);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] redeploy cancelled (%s)", why ? why : "");
    }

    void TFMapScreen::SendRedeployRequest(RegionId region)
    {
        if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
        {
            SetStatus("Redeploy failed: not connected", true);
            return;
        }
        TF_RedeployRequest rq{};
        rq.regionId = region;
        m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsg_RedeployRequest), &rq, sizeof(rq));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] redeploy request -> region %u", static_cast<unsigned>(region));
    }

    void TFMapScreen::TickRedeploy(float dt)
    {
        m_cooldownLeft = std::max(0.0f, m_cooldownLeft - dt);
        m_statusTTL = std::max(0.0f, m_statusTTL - dt);

        if (!m_redeployPending)
            return;

        // Death routes through the spawn screen instead; a vehicle seat makes
        // the teleport unsound (the pawn is driven by the vehicle transform).
        if (!LocalPawnAlive())
        {
            CancelRedeploy("you went down");
            return;
        }
        if (LocalSeated())
        {
            CancelRedeploy("entered a vehicle");
            return;
        }

        m_redeployCountdown -= dt;
        if (m_redeployCountdown > 0.0f)
            return;

        const RegionId target = m_redeployTarget;
        m_redeployPending = false;
        m_redeployTarget = kInvalidRegion;
        m_redeployCountdown = 0.0f;

        // Re-check eligibility at send time (ownership can flip mid-countdown).
        const uint8_t reason = CanRedeployTo(target);
        if (reason != kTFRedeployOk)
        {
            SetStatus(TFRedeployRules::ReasonText(reason), true);
            return;
        }
        SendRedeployRequest(target);
    }

    void TFMapScreen::OnRedeployReply(const TF_RedeployReply& rep)
    {
        if (rep.accepted)
        {
            m_cooldownLeft = TFRedeployRules::kTFRedeployIntervalSec; // local mirror of the server interval
            const RegionDef* rd = (m_ctx && m_ctx->data) ? m_ctx->data->GetRegion(rep.regionId) : nullptr;
            char line[96];
            std::snprintf(line, sizeof(line), "Redeployed to %s", rd ? rd->name.c_str() : "region");
            SetStatus(line, false);
            m_deployHint = kInvalidRegion; // rally hint served
            Close();                       // you are there now — back to the gun
            return;
        }
        if (rep.reason == kTFRedeployCooldown)
            m_cooldownLeft = std::max(m_cooldownLeft, rep.cooldownSec);
        SetStatus(TFRedeployRules::ReasonText(rep.reason), true);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] redeploy denied: reason %u", static_cast<unsigned>(rep.reason));
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

        // W7: input-receiving side panel for the selected region (own window,
        // TFHUD::DrawDeathActions pattern — buttons need a non-NoInputs window).
        DrawInspectPanel();
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
        std::unordered_map<RegionId, uint32_t> squadCount; // W7: squadmate markers
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
                    else if (m_ctx->squads && m_ctx->squads->IsLocalSquadMember(p.owner))
                        ++squadCount[r]; // W7: squadmates (self excluded — YOU marker covers it)
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

            // Outline: hovered bright; selected cyan; deploy hint gold; default faint.
            ImU32 outline = IM_COL32(210, 214, 220, 70);
            float thick = 1.5f;
            if (rd.id == m_deployHint)
            {
                outline = IM_COL32(255, 200, 60, 220);
                thick = 3.0f;
            }
            if (rd.id == m_selected)
            {
                outline = IM_COL32(90, 220, 255, 230); // W7 inspect selection
                thick = 3.0f;
            }
            if (rd.id == m_hovered)
            {
                outline = IM_COL32(255, 255, 255, 230);
                thick = 3.0f;
            }
            dl->AddPolyline(corners, 6, outline, ImDrawFlags_Closed, thick);

            // W7: pending-redeploy target gets a gold spinner ring.
            if (m_redeployPending && rd.id == m_redeployTarget)
            {
                const float a0 = m_time * 3.0f;
                dl->PathArcTo(c, m_hexSize * 0.78f, a0, a0 + kPi * 1.4f, 24);
                dl->PathStroke(IM_COL32(255, 200, 60, 230), 0, 3.5f);
            }

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
            // W7: squad-member badge (top-right, green diamond) — replicated
            // pawn data filtered through TFSquadSystem::IsLocalSquadMember.
            if (auto it = squadCount.find(rd.id); it != squadCount.end())
            {
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), "%u", it->second);
                const ImVec2 bp(c.x + m_hexSize * 0.52f, c.y - m_hexSize * 0.52f);
                const float s = 9.0f;
                dl->AddQuadFilled(ImVec2(bp.x, bp.y - s), ImVec2(bp.x + s, bp.y), ImVec2(bp.x, bp.y + s),
                                  ImVec2(bp.x - s, bp.y), IM_COL32(96, 220, 140, 220));
                dl->AddQuad(ImVec2(bp.x, bp.y - s), ImVec2(bp.x + s, bp.y), ImVec2(bp.x, bp.y + s),
                            ImVec2(bp.x - s, bp.y), IM_COL32(15, 17, 22, 200), 1.5f);
                AddTextCentered(dl, 12.0f, bp, IM_COL32(12, 24, 16, 255), cnt);
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
                             : "Click a region to inspect / redeploy  |  M / Esc closes");

        // W7: redeploy countdown + status line (center-bottom).
        if (m_redeployPending)
        {
            const RegionDef* td = m_ctx->data->GetRegion(m_redeployTarget);
            char line[96];
            std::snprintf(line, sizeof(line), "REDEPLOYING TO %s IN %.1f s", td ? td->name.c_str() : "?",
                          std::max(0.0f, m_redeployCountdown));
            AddTextCentered(dl, 22.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.88f),
                            IM_COL32(255, 200, 60, 235), line);
        }
        else if (m_statusTTL > 0.0f && m_status[0] != '\0')
        {
            AddTextCentered(dl, 16.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.88f),
                            m_statusIsError ? IM_COL32(255, 120, 110, 230) : IM_COL32(140, 235, 160, 230), m_status);
        }
        else if (m_cooldownLeft > 0.0f && !dead)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "Redeploy ready in %.0f s", std::ceil(m_cooldownLeft));
            AddTextCentered(dl, 14.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.88f),
                            IM_COL32(180, 184, 190, 200), line);
        }

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
                else if (!dead && CanRedeployTo(rd.id) == kTFRedeployOk)
                    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "REDEPLOY AVAILABLE - click to inspect");
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
                // W7: alive click selects for the inspect panel AND doubles as
                // the W3 rally hint (previous behavior preserved).
                m_selected = (m_selected == hoveredDef->id) ? kInvalidRegion : hoveredDef->id;
                m_deployHint = hoveredDef->id;
            }
        }
    }

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

    void TFMapScreen::RenderUI() {}
    void TFMapScreen::DrawMapContents() {}
    void TFMapScreen::DrawInspectPanel() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
