/**
 * @file TFMapScreen.cpp
 * @brief W2 continent map, lifecycle + input half: open/close/toggle with
 *        cursor capture handoff, the M / Esc keybind route (chat-focus
 *        suppression), the tf_redeploy console command, and the W7
 *        (ui-map-keys) server-validated redeploy state machine
 *        (TFRedeployRules + TF_RedeployRequest/Reply, reserved 0x5430 block)
 *        — state-only, works headless.
 *
 * Split per the repo file-size rule (TFHUDCombat.cpp pattern — same class,
 * feature-owned translation units): the fullscreen hex-grid overlay lives in
 * TFMapScreenDraw.cpp, the W7 selected-region inspect panel in
 * TFMapScreenInspect.cpp.
 *
 * Consumes the W2 TFRegionSystem contract (OwnerOf / IsCapturable /
 * CaptureProgress / RegionsHeld / CanSpawnAt) — identical accessors on server
 * and on the client mirror fed by TF_RegionState / TF_CaptureTick.
 */
#include "UI/TFMapScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFRedeployRules.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFHUD.h"
#include "UI/TFKeybinds.h"
#include "UI/TFSpawnScreen.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Terrafront
{

    namespace
    {

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

} // namespace Terrafront
