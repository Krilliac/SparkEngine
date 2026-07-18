/**
 * @file TFSpawnScreen.cpp
 * @brief W2 death/spawn screen: faction splash (first join), class picker
 *        (classes.json, Colossus greyed out), spawn point list (skyanchor +
 *        every CanSpawnAt region with distance from the death spot), DEPLOY
 *        with respawn-countdown gating, and HUD-overlay coordination.
 *        W6: deploy header shows the last-death summary (killer / weapon /
 *        distance / headshot) sourced from TFHUD::LastDeath().
 *
 * Consumes the W2 TFRegionSystem contract (CanSpawnAt) on both server and
 * client mirror. Sends TF_FactionSelect / TF_SpawnRequest through
 * TFClientNet::SendMsg (which loops back in-process on authority roles).
 *
 * Split per the repo file-size rule (TFLoginFlowDraw.cpp pattern — same
 * class, feature-owned translation units): this file keeps the lifecycle,
 * open/close state, death mirror, and sends; TFSpawnScreenDraw.cpp keeps
 * the ImGui rendering (RenderUI / DrawFactionSplash / DrawDeployPanel).
 */
#include "UI/TFSpawnScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFHUD.h"

#include "Input/InputManager.h" // W10 sanctuary-v2: terminal mode owns the mouse
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace Terrafront
{

    namespace
    {

        constexpr float kOpenDelaySec = 2.0f;       // death -> screen (HUD overlay window)
        constexpr float kRequestDebounceSec = 1.0f; // DEPLOY re-enable after a request

    } // namespace

    TFSpawnScreen::TFSpawnScreen() = default;
    TFSpawnScreen::~TFSpawnScreen()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSpawnScreen::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvLocalPlayerDied>([this](const EvLocalPlayerDied& e) { OnLocalPlayerDied(e); });
        // Authority roles fire EvPlayerSpawned on the shared bus; pure clients are
        // covered by the alive-poll in Update().
        events.Subscribe<EvPlayerSpawned>(
            [this](const EvPlayerSpawned& e)
            {
                if (m_ctx && e.player == m_ctx->localPlayer)
                {
                    m_pendingOpen = 0.0f;
                    Close();
                }
            });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSpawnScreen initialized");
        return true;
    }

    void TFSpawnScreen::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFSpawnScreen::Shutdown()
    {
        m_open = false;
        m_initialized = false;
    }

    void TFSpawnScreen::RenderDebugUI() {}

    // ---------------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------------

    bool TFSpawnScreen::LocalPawnAlive() const
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

    void TFSpawnScreen::OnLocalPlayerDied(const EvLocalPlayerDied& ev)
    {
        if (!m_ctx || ev.player != m_ctx->localPlayer)
            return;
        m_respawnLeft = std::max(0.0f, ev.respawnDelay);
        if (!m_open)
            m_pendingOpen = kOpenDelaySec; // HUD owns the first 2 s ("YOU ARE DOWN")

        PawnInfo p{};
        if (m_ctx->players && m_ctx->players->GetPawnByPlayer(ev.player, p))
        {
            m_deathPos[0] = p.pos[0];
            m_deathPos[1] = p.pos[1];
            m_deathPos[2] = p.pos[2];
        }
    }

    void TFSpawnScreen::Open()
    {
        if (m_open || !m_initialized)
            return;
        m_open = true;
        m_terminalMode = false; // normal opens (death/boot) are never terminal mode
        m_pendingOpen = 0.0f;
        // Suppress the HUD death overlay while this screen owns the frame.
        if (m_ctx && m_ctx->hud)
            m_ctx->hud->SetRespawnState(false, 0.0f);
    }

    void TFSpawnScreen::OpenClassTerminal()
    {
        // W10 sanctuary-v2: same panel, alive-friendly mode (see header note).
        if (m_open || !m_initialized)
            return;
        Open();
        m_terminalMode = m_open;
        // Unlike death opens (pawn already dead, mouse free), the terminal
        // opens while ALIVE with mouse-look captured — release it so the
        // panel is clickable (travel/vehicle-shop menu pattern).
        InputManager* input = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetInput() : nullptr;
        if (m_terminalMode && input && input->IsMouseCaptured())
            input->CaptureMouse(false);
    }

    void TFSpawnScreen::Close()
    {
        if (!m_open)
            return;
        const bool wasTerminal = m_terminalMode; // W10 sanctuary-v2
        m_open = false;
        m_terminalMode = false;
        // Leaving the class terminal with a live pawn: hand the mouse back to
        // mouse-look (mirror of the release in OpenClassTerminal).
        if (wasTerminal && LocalPawnAlive())
        {
            InputManager* input = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetInput() : nullptr;
            if (input)
                input->CaptureMouse(true);
        }
        // Closed while still dead (e.g. via console): hand the remaining countdown
        // back to the HUD overlay so the player is never without death feedback.
        if (m_ctx && m_ctx->hud && !LocalPawnAlive())
            m_ctx->hud->SetRespawnState(true, m_respawnLeft);
    }

    void TFSpawnScreen::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        // W5 onboarding (Task 6): the deploy/spawn screen is gated behind having
        // entered the world (TFLoginFlow owns the pre-world menu). Without this,
        // the boot auto-open below would race TFLoginFlow's login/char-select
        // screens the instant a fresh connection lands.
        if (!m_ctx->InWorld())
            return;

        m_respawnLeft = std::max(0.0f, m_respawnLeft - deltaTime);
        m_debounce = std::max(0.0f, m_debounce - deltaTime);

        // Death -> delayed auto-open.
        if (m_pendingOpen > 0.0f)
        {
            m_pendingOpen -= deltaTime;
            if (m_pendingOpen <= 0.0f && !LocalPawnAlive())
                Open();
        }

        // First-deploy auto-open: once, when a session is up and no pawn exists
        // (fresh boot / fresh connect). Covers the faction pick + first spawn.
        if (!m_bootOpened)
        {
            if (LocalPawnAlive())
            {
                m_bootOpened = true; // spawned some other way (tf_spawn)
            }
            else if (m_ctx->clientNet && m_ctx->clientNet->IsConnected() && m_ctx->data && m_ctx->data->IsLoaded())
            {
                m_bootOpenDelay -= deltaTime;
                if (m_bootOpenDelay <= 0.0f)
                {
                    m_bootOpened = true;
                    Open();
                }
            }
        }

        if (m_open)
        {
            if (LocalPawnAlive())
            {
                // W10 sanctuary-v2: terminal mode is MEANT to be open while
                // alive (class pre-select at the sanctuary class terminal).
                if (!m_terminalMode)
                    Close(); // spawn went through (any path, incl. pure client)
            }
            else
            {
                // Dying with the terminal panel up converts it into the normal
                // death screen (otherwise the alive auto-close above would stay
                // suppressed after the next respawn).
                m_terminalMode = false;
                if (m_ctx->hud)
                {
                    // Keep the HUD overlay suppressed even if a late TF_SpawnReply
                    // (reason=timer) re-armed it via TFClientNet.
                    m_ctx->hud->SetRespawnState(false, 0.0f);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Sends
    // ---------------------------------------------------------------------------

    void TFSpawnScreen::SendFactionSelect(FactionId f)
    {
        if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
            return;
        TF_FactionSelect sel{};
        sel.faction = static_cast<uint8_t>(f);
        m_ctx->clientNet->SendMsg(TFMsg::FactionSelect, &sel, sizeof(sel));
        // Optimistic local mirror (the loopback route sets it too; the server
        // remains authoritative for remote sessions).
        m_ctx->localFaction = f;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] faction selected: %s", FactionTag(f));
    }

    void TFSpawnScreen::SendSpawnRequest()
    {
        if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
            return;
        TF_SpawnRequest rq{};
        rq.classId = static_cast<uint8_t>(m_selClass);
        rq.spawnKind = m_selKind;
        rq.regionId = (m_selKind == 1) ? m_selRegion : 0;
        rq.aegisEntity = (m_selKind == 2) ? m_selAegis : 0; // W3: deployed-Aegis spawns
        m_ctx->clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
        m_debounce = kRequestDebounceSec;
    }

} // namespace Terrafront
