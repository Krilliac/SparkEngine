/**
 * @file TFHUD.cpp
 * @brief TFHUD core: lifecycle, feed-in setters, and the per-frame pawn-state
 *        gather. Sibling feature-owned translation units (TFPlayerSystemClient
 *        pattern, repo file-size rule): the overlay drawing (vitals, weapon
 *        box, crosshair + hitmarker, capture bar, compass, ability slot) lives
 *        in TFHUDDraw.cpp, the chat window in TFHUDChat.cpp, and the W6 combat
 *        half (killfeed, damage pings, death panel + actions, minimap v2) in
 *        TFHUDCombat.cpp. Shared internals live in TFHUDInternal.h.
 *
 * Rendered as one borderless, transparent, input-transparent overlay window
 * covering the main viewport (plus small input-receiving windows for chat and
 * the death-panel buttons — safe since the engine now gates gameplay mouse
 * capture on ImGui's WantCaptureMouse). All state is fed either by frozen
 * setters (called from TFClientNet / TFWeaponSystem), bus events, or read
 * from the frozen TFPlayerSystem pawn API each frame.
 */
#include "UI/TFHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "UI/TFHUDInternal.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    using namespace HudDetail; // TFHUDInternal.h: kHitmarkerDuration shared with TFHUDDraw.cpp

    namespace
    {

        constexpr float kOctantDecayPerSec = 1.6f; // damage flash fade speed
        constexpr float kPingDecayPerSec = 0.55f;  // world ping fade (~1.8 s)
        constexpr float kCaptureBarTTL = 3.0f;     // hide if no tick refreshes it

    } // namespace

    TFHUD::TFHUD() = default;
    TFHUD::~TFHUD()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFHUD::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Belt-and-braces alongside the direct TFClientNet -> setter calls:
        // the frozen bus events also drive the respawn overlay and rank text.
        events.Subscribe<EvLocalPlayerDied>(
            [this](const EvLocalPlayerDied& e)
            {
                if (m_ctx && e.player == m_ctx->localPlayer)
                    SetRespawnState(true, e.respawnDelay);
            });
        events.Subscribe<EvPlayerSpawned>(
            [this](const EvPlayerSpawned& e)
            {
                if (m_ctx && e.player == m_ctx->localPlayer)
                    SetRespawnState(false, 0.0f);
            });
        events.Subscribe<EvRankUp>(
            [this](const EvRankUp& e)
            {
                if (m_ctx && e.player == m_ctx->localPlayer)
                    m_rank = e.newRank;
            });
        // W6 combat HUD (TFHUDCombat.cpp): headshot/id annotation for the
        // killfeed + death panel, and world-anchored damage pings. Both events
        // fire on authority roles; pure clients are covered by the TFClientNet
        // handler paths (PushKillfeed / ShowDamageFrom + the EvPlayerDamaged
        // re-fire in OnDamageEvent).
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& e) { OnPlayerKilledBus(e); });
        events.Subscribe<EvPlayerDamaged>([this](const EvPlayerDamaged& e) { OnPlayerDamagedBus(e); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFHUD initialized");
        return true;
    }

    void TFHUD::Update(float dt)
    {
        if (!m_initialized)
            return;

        m_hitTimer = std::max(0.0f, m_hitTimer - dt);
        for (float& o : m_octant)
            o = std::max(0.0f, o - kOctantDecayPerSec * dt);
        for (DamagePing& p : m_pings)
            p.intensity = std::max(0.0f, p.intensity - kPingDecayPerSec * dt);
        m_octantSuppress = std::max(0.0f, m_octantSuppress - dt);
        m_lastOctantAge += dt;

        for (auto& e : m_killfeed)
            e.ttl -= dt;
        while (!m_killfeed.empty() && m_killfeed.back().ttl <= 0.0f)
            m_killfeed.pop_back();

        if (m_pendingKill.valid)
        {
            m_pendingKill.ttl -= dt;
            if (m_pendingKill.ttl <= 0.0f)
                m_pendingKill = PendingKill{};
        }

        m_captureTTL = std::max(0.0f, m_captureTTL - dt);
        m_deployCooldown = std::max(0.0f, m_deployCooldown - dt);
        if (m_dead)
            m_respawnLeft = std::max(0.0f, m_respawnLeft - dt);

        // Death-summary lifecycle + last-alive anchor run off the pawn poll so
        // they also work headless (RenderUI is compiled out without ImGui).
        GatherPawnView();
    }

    void TFHUD::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFHUD::Shutdown()
    {
        if (m_chatOpen)
            CloseChat();
        m_killfeed.clear();
        m_chatInput[0] = '\0';
        m_initialized = false;
    }

    void TFHUD::RenderDebugUI() {} // HUD state is visible on screen; no debug panel needed W1

    // ---------------------------------------------------------------------------
    // Public feed-ins
    // ---------------------------------------------------------------------------

    void TFHUD::ShowHitmarker(bool killed)
    {
        m_hitTimer = killed ? kHitmarkerDuration * 2.0f : kHitmarkerDuration;
        m_hitKilled = killed;
    }

    // PushKillfeed (both overloads) lives in TFHUDCombat.cpp with the rest of
    // the killfeed/death-panel logic.

    void TFHUD::SetCaptureProgress(float progress01, FactionId capturing, bool visible)
    {
        m_captureProgress = std::clamp(progress01, 0.0f, 1.0f);
        m_captureFaction = capturing;
        m_captureVisible = visible;
        m_captureTTL = visible ? kCaptureBarTTL : 0.0f;
    }

    void TFHUD::SetRespawnState(bool dead, float secondsLeft)
    {
        m_dead = dead;
        m_respawnLeft = dead ? std::max(0.0f, secondsLeft) : 0.0f;
    }

    void TFHUD::ShowDamageFrom(uint8_t dirOctant)
    {
        if (dirOctant >= 8)
            return;
        if (m_octantSuppress > 0.0f)
            return; // a world-anchored ping already covers this hit
        m_octant[dirOctant] = 1.0f;
        m_lastOctant = dirOctant;
        m_lastOctantAge = 0.0f; // AddWorldPing clears this flash if the same
                                // hit resolves to a ping right after (bus
                                // subscriber order is not guaranteed)
    }

    void TFHUD::SetWeaponStatus(const char* name, int mag, int reserve, bool reloading)
    {
        m_weapName = name ? name : "";
        m_weapMag = mag;
        m_weapReserve = reserve;
        m_weapReloading = reloading;
    }

    void TFHUD::SetRank(uint16_t rank)
    {
        m_rank = rank;
    }

    // ---------------------------------------------------------------------------
    // Pawn state gather (no ImGui)
    // ---------------------------------------------------------------------------

    void TFHUD::GatherPawnView()
    {
        m_view = PawnView{};
        if (!m_ctx || !m_ctx->players)
            return;

        PlayerId pid = m_ctx->localPlayer;
        if (pid == kInvalidPlayer && m_ctx->clientNet)
            pid = m_ctx->clientNet->LocalPlayerId();
        if (pid == kInvalidPlayer)
            return;

        PawnInfo p{};
        if (!m_ctx->players->GetPawnByPlayer(pid, p))
        {
            m_wasViewAlive = false;
            return;
        }

        m_view.valid = true;
        m_view.alive = p.alive;
        m_view.health = p.health;
        m_view.shield = p.shield;
        m_view.speed = std::sqrt(p.vel[0] * p.vel[0] + p.vel[2] * p.vel[2]);
        m_view.pos[0] = p.pos[0];
        m_view.pos[1] = p.pos[1];
        m_view.pos[2] = p.pos[2];
        m_view.cls = p.cls;
        m_lastClass = p.cls;

        if (m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const ClassDef* cd = m_ctx->data->GetClass(p.cls))
            {
                m_view.maxHealth = cd->health;
                m_view.maxShield = cd->shield;
            }
        }

        // Death-summary lifecycle: the last-alive position anchors the killer
        // distance; the dead->alive edge clears the panel (W6 contract: LastDeath
        // is valid from the killing blow until the next local spawn).
        if (m_view.alive)
        {
            if (!m_wasViewAlive)
                m_death = DeathSummary{};
            m_lastAlivePos[0] = p.pos[0];
            m_lastAlivePos[1] = p.pos[1];
            m_lastAlivePos[2] = p.pos[2];
            m_wasViewAlive = true;
        }
        else
        {
            m_wasViewAlive = false;
        }
    }

    // CloseChat + DrawChat live in TFHUDChat.cpp; RenderUI and the other
    // drawing members (vitals, weapon box, crosshair + hitmarker, capture bar,
    // compass, ability slot) live in TFHUDDraw.cpp.

} // namespace Terrafront
