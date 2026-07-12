/**
 * @file TFBotSystem.cpp
 * @brief Server-side bots driving the REAL game paths: faction select + spawn
 *        request pipeline, TF_ClientInput movement through TFServerSim,
 *        TF_FireEvent combat through TFWeaponSystem, TF_VehicleSeatOp driving
 *        through TFVehicleSystem, 8 s respawn loop.
 *
 * Tactics (all server-side, deterministic given the same tick sequence):
 *  - objective bias: contested / actively-capturing enemy regions and own
 *    regions under attack score far better than quiet frontier regions
 *    (TFRegionSystem CaptureProgress), distance-weighted;
 *  - fireteam cohesion: the first alive bot of a faction is the implicit
 *    leader; the rest adopt its objective and regroup when strung out;
 *  - engage-vs-advance: healthy + loaded bots push the target, hurt or
 *    reloading bots give ground while keeping the target in their sights;
 *  - vehicles: when the objective is a long march, enter the driver seat of a
 *    nearby friendly vehicle (TFVehicleSystem::ServerHandleSeatOp) and drive
 *    it there — the normal TF_ClientInput enqueue steers it because
 *    TFServerSim forwards a seated player's inputs to ServerHandleSeatedInput.
 *
 * Region objectives consume the W2 TFRegionSystem contract (OwnerOf /
 * IsCapturable / CaptureProgress) through a compile-time detection shim: the
 * shim keeps this file building green even if those methods change shape in a
 * parallel lane (static regions.json fallback). No behavior change is needed
 * here when they land.
 */
#include "Game/TFBotSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFChaosHarness.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFMovementModel.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponMath.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFServerSim.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

// W12 bot-navigation: feeler rays against the live Jolt world (GetJoltSystem()
// probe — the no-Jolt stub hands out dummy bodies; TFImpactFx precedent).
#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
#include "Spark/IEngineContext.h"

// W9 bots-v2: the class-abilities lane's system is optional at this lane's
// compile time. When the header exists in the tree, include it so the seam
// concepts below see the full type; when it does not, TFGameContext has no
// `abilities` member either (the integrator lands both together) and every
// ability call in this file collapses to a compile-time no-op.
#if __has_include("Game/TFAbilitySystem.h")
#include "Game/TFAbilitySystem.h"
#endif

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <numbers>
#include <sstream>
#include <type_traits>

namespace Terrafront
{

    namespace
    {

        constexpr float kThinkIntervalSec = 0.20f; // 5 Hz brain
        constexpr float kEngageRangeM = 60.0f;     // acquire targets inside this
        constexpr float kDisengageRangeM = 70.0f;  // keep firing until this (hysteresis)
        constexpr float kSprintBeyondM = 80.0f;    // sprint when objective is far
        constexpr float kHoldFireCloseM = 15.0f;   // stop advancing inside this
        constexpr float kAimErrorDeg = 1.5f;       // +-1.5 deg cone on every shot
        constexpr float kChestHeightM = 1.20f;     // aim point above target feet
        constexpr float kLosStepM = 2.0f;          // terrain march step (TerrainBlocked-style)
        constexpr float kStuckWindowSec = 2.0f;    // pos unchanged this long -> jump
        constexpr float kStuckEpsM = 0.25f;
        constexpr float kSpawnRetrySec = 1.0f;
        constexpr float kRespawnWaitSec = kTFRespawnDelaySec + 0.25f; // outlast the server timer
        constexpr uint32_t kSelectableClasses = 5;                    // Ghost..Bulwark (no Colossus, DESIGN §1)

        // engage-vs-advance (health/ammo fitness)
        constexpr float kLowHealthFrac = 0.35f; // below this pool fraction -> give ground

        // fireteam cohesion
        constexpr float kRegroupBeyondM = 60.0f; // strung out past this -> close on the leader

        // vehicle use (driver seat 0 only)
        constexpr float kVehSeekBeyondM = 120.0f; // objective farther than this -> want wheels
        constexpr float kVehScanRadiusM = 60.0f;  // hull must be within this to walk to it
        constexpr float kVehExitWithinM = 60.0f;  // objective closer than this -> dismount
        constexpr float kVehEnterTryM = 3.0f;     // issue the seat op inside this XZ range
        constexpr float kVehStuckSec = 3.0f;      // wedged ride pose this long -> recover
        constexpr double kVehRetrySec = 10.0;     // cooldown after a failed/abandoned plan
        constexpr uint8_t kVehMaxEnterTries = 3;

        // driving v2 (W9 bots-v2)
        constexpr float kVehProbeAheadM = 12.0f;    // terrain look-ahead probe distance
        constexpr float kVehProbeSideRad = 0.7f;    // ~40 deg side probes around a wall
        constexpr float kVehClimbLimitM = 3.0f;     // rise beyond this ahead = treat as a wall
        constexpr double kVehReverseSec = 1.6;      // reverse-out recovery phase length
        constexpr double kVehWedgeWindowSec = 20.0; // 2nd wedge inside this -> dismount
        constexpr float kVehHardTurnRad = 1.5f;     // ease throttle past this yaw error

        // Vulture VTOL flight (W9 bots-v2; lift axis = TFB_Jump/TFB_Crouch)
        constexpr float kVulCruiseMinAglM = 25.0f; // climb below this while cruising
        constexpr float kVulCruiseMaxAglM = 45.0f; // descend above this while cruising
        constexpr float kVulLandWithinM = 60.0f;   // start the landing descent inside this
        constexpr float kVulExitAglM = 1.5f;       // try the (landed-gated) exit below this
        constexpr float kVulStuckSec = 6.0f;       // hover wedge window (yaw-in-place is slow)

        // class abilities (W9 bots-v2)
        constexpr double kAbilityCheckSec = 2.5; // situational trigger rate limit
        constexpr float kMedtechHealRadiusM = 15.0f;
        constexpr float kMedtechHurtFrac = 0.7f; // friendly pool below this -> heal
        constexpr float kStrikerGapM = 25.0f;    // target farther than this -> jets

        // local obstacle avoidance (W12 bot-navigation)
        constexpr float kFeelerLenM = 3.0f;         // chest-height feeler ray length
        constexpr float kFeelerSideRad = 0.5236f;   // ±30 deg side feelers
        constexpr float kAvoidSteerRad = 0.9f;      // detour angle past a blocked forward
        constexpr float kBackTurnRad = 2.0944f;     // 120 deg pocket escape turn
        constexpr double kBackTurnSec = 1.0;        // pocket escape leg length
        constexpr double kBlockedMemorySec = 5.0;   // remember a blocked heading this long
        constexpr float kUnstickMinMoveM = 1.5f;    // < this progress inside the window = stalled
        constexpr double kUnstickWindowSec = 3.0;   // progress measurement window
        constexpr double kUnstickSteerSec = 2.0;    // random-heading escape leg length
        constexpr uint8_t kUnstickScatterAfter = 3; // consecutive unsticks -> chaos scatter

        // chaos pilots (W9 bots-v2): deterministic vehicle exercise for tf_validate
        constexpr uint32_t kChaosVulturePilotSlot = 1; // AUC; flies a Vulture
        constexpr uint32_t kChaosDriverPilotSlot = 2;  // HLX; drives a Drifter
        constexpr double kPilotBuyRetrySec = 4.0;
        constexpr uint32_t kBotChaosFluxGrant = 200; // Drifter money for every bot
        constexpr uint16_t kPilotXPGrant = 45000;    // rank >= 15 (500 * n^1.6): Vulture gate

        // ---------------------------------------------------------------------------
        // W2 TFRegionSystem contract detection (kept from the regions-parallel wave).
        // The `if constexpr` discard only works inside a template, so the helpers
        // deduce R from the ctx pointer instead of naming TFRegionSystem directly.
        // ---------------------------------------------------------------------------

        template <typename R>
        concept TFHasRegionQueries = requires(const R& r, RegionId id, FactionId f) {
            { r.OwnerOf(id) } -> std::convertible_to<FactionId>;
            { r.IsCapturable(id, f) } -> std::convertible_to<bool>;
        };

        template <typename R>
        concept TFHasCaptureProgress = requires(const R& r, RegionId id, FactionId& f, bool& c) {
            { r.CaptureProgress(id, f, c) } -> std::convertible_to<float>;
        };

        template <typename R> FactionId QueryRegionOwner(const R* regions, RegionId id, FactionId fallback)
        {
            if constexpr (TFHasRegionQueries<R>)
            {
                if (regions)
                    return regions->OwnerOf(id);
            }
            (void)regions;
            (void)id;
            return fallback;
        }

        template <typename R>
        bool QueryRegionCapturable(const R* regions, RegionId id, FactionId attacker, bool fallback)
        {
            if constexpr (TFHasRegionQueries<R>)
            {
                if (regions)
                    return regions->IsCapturable(id, attacker);
            }
            (void)regions;
            (void)id;
            (void)attacker;
            return fallback;
        }

        /// Capture progress toward `outCapturing` (0 when the region system is not
        /// queryable — bots then fall back to pure nearest-region marching).
        template <typename R>
        float QueryCaptureProgress(const R* regions, RegionId id, FactionId& outCapturing, bool& outContested)
        {
            if constexpr (TFHasCaptureProgress<R>)
            {
                if (regions)
                    return regions->CaptureProgress(id, outCapturing, outContested);
            }
            (void)regions;
            (void)id;
            outCapturing = FactionId::None;
            outContested = false;
            return 0.0f;
        }

        // ---------------------------------------------------------------------------
        // W9 class-ability seam detection (same philosophy as the region shim
        // above): the class-abilities lane lands TFAbilitySystem + a
        // TFGameContext::abilities pointer together (integrator). Until both
        // exist in this tree these helpers compile to constant-false no-ops;
        // once they land, bots drive the real public CanUseAbility/UseAbility
        // seam with no change needed here. The `if constexpr` discard only works
        // inside a template, so C is deduced from the ctx pointer.
        // ---------------------------------------------------------------------------

        template <typename C>
        concept TFHasAbilityCtxPtr = requires(C& c) { c.abilities; };

        template <typename A>
        concept TFHasAbilityApi = requires(A& a, PlayerId p) {
            { a.CanUseAbility(p) } -> std::convertible_to<bool>;
            a.UseAbility(p);
        };

        /// True when the seam is compiled in AND published at runtime.
        template <typename C> bool AbilitySeamPresent(C* ctx)
        {
            if constexpr (TFHasAbilityCtxPtr<C>)
            {
                if (ctx && ctx->abilities)
                {
                    using A = std::remove_pointer_t<std::remove_cvref_t<decltype(ctx->abilities)>>;
                    if constexpr (TFHasAbilityApi<A>)
                        return true;
                }
            }
            (void)ctx;
            return false;
        }

        /// CanUseAbility -> UseAbility through the seam. True only when an
        /// activation actually happened.
        template <typename C> bool TryUseAbilitySeam(C* ctx, PlayerId player)
        {
            if constexpr (TFHasAbilityCtxPtr<C>)
            {
                if (ctx && ctx->abilities)
                {
                    using A = std::remove_pointer_t<std::remove_cvref_t<decltype(ctx->abilities)>>;
                    if constexpr (TFHasAbilityApi<A>)
                    {
                        if (ctx->abilities->CanUseAbility(player))
                        {
                            ctx->abilities->UseAbility(player);
                            return true;
                        }
                    }
                }
            }
            (void)ctx;
            (void)player;
            return false;
        }

        /// Static ownership guess when the live region system is not queryable yet:
        /// regions.json initial owner (indexed by RegionId), else the home faction.
        FactionId FallbackOwner(const ContinentDef& cont, const RegionDef& r)
        {
            if (r.id < cont.initialOwner.size())
                return cont.initialOwner[r.id];
            return r.homeFaction;
        }

        float WrapPi(float a)
        {
            constexpr float kPi = std::numbers::pi_v<float>;
            constexpr float kTau = 2.0f * kPi;
            while (a > kPi)
                a -= kTau;
            while (a < -kPi)
                a += kTau;
            return a;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    TFBotSystem::TFBotSystem() = default;
    TFBotSystem::~TFBotSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFBotSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_bots.reserve(kTFMaxBots);

        // Weapon stats are cached per bot; refresh them on tf_reload_data.
        events.Subscribe<EvDataReloaded>(
            [this](const EvDataReloaded&)
            {
                for (Bot& b : m_bots)
                    ResolveLoadout(b);
            });

        m_initialized = true;

        // Chaos validation harness (bots-chaos lane): kill/flip observers live
        // for the module lifetime, counters reset per chaos run.
        m_chaos = std::make_unique<TFChaosHarness>();
        m_chaos->Initialize(ctx, events);

        // Console wiring inside the owning system (the TFRegionSystem
        // tf_capture_debug pattern) — TFCommands.cpp is a contended file.
        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_chaos"))
        {
            console.RegisterCommand(
                "tf_chaos",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized || !m_ctx)
                        return "[TF] bot system not ready";
                    if (!m_ctx->IsAuthority())
                        return "[TF] tf_chaos: authority only (tf_dedicated / tf_host first)";
                    if (args.empty())
                        return std::string("[TF] chaos ") + (m_chaosActive ? "ACTIVE" : "idle") +
                               "  bots=" + std::to_string(BotCount()) + "  (tf_chaos <0-32> [seconds])";
                    const int n = std::atoi(args[0].c_str());
                    if (n < 0 || n > static_cast<int>(kTFMaxBots))
                        return "[TF] tf_chaos: count must be 0-32";
                    float seconds = 300.0f;
                    if (args.size() > 1)
                        seconds = static_cast<float>(std::atof(args[1].c_str()));
                    ServerStartChaos(static_cast<uint32_t>(n), seconds);
                    if (n == 0)
                        return "[TF] chaos stopped, bots despawned";
                    return "[TF] chaos started: " + std::to_string(n) + " bots, " +
                           std::to_string(static_cast<int>(seconds)) + " s (tf_validate for the report)";
                },
                "Chaos exercise mode: spawn N bots across all factions that scatter onto capture "
                "points, fight, capture, place deployables and try vehicles to stress-test the server",
                "TERRAFRONT", "tf_chaos <botcount 0-32> [seconds=300]");
            m_chaosCmds = true;
        }
        if (!console.HasCommand("tf_validate"))
        {
            console.RegisterCommand(
                "tf_validate", [this](const std::vector<std::string>&) -> std::string { return ValidationReport(); },
                "Print machine-greppable [TF-VALIDATE] PASS/FAIL lines for the last/current chaos "
                "run: physics, collision, capture, kills, terrain",
                "TERRAFRONT", "tf_validate");
            m_chaosCmds = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFBotSystem initialized");
        return true;
    }

    void TFBotSystem::Shutdown()
    {
        if (!m_initialized)
            return;
        m_chaosActive = false;
        if (m_chaosCmds)
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            console.UnregisterCommand("tf_chaos");
            console.UnregisterCommand("tf_validate");
            m_chaosCmds = false;
        }
        for (Bot& b : m_bots)
            DespawnBot(b);
        m_bots.clear();
        m_initialized = false;
    }

    double TFBotSystem::Now() const
    {
        return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
    }

    void TFBotSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;

        if (!m_ctx->IsAuthority())
        {
            // Lost authority (e.g. host became a pure client): bots go away.
            if (!m_bots.empty())
            {
                for (Bot& b : m_bots)
                    DespawnBot(b);
                m_bots.clear();
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] no longer authority — all bots despawned");
            }
            return;
        }

        const double now = Now();
        for (Bot& b : m_bots)
        {
            if (now >= b.nextThinkAt)
            {
                b.nextThinkAt = now + kThinkIntervalSec;
                Think(b, now);
            }
        }

        // Chaos run bookkeeping: expire the timer and feed the validation
        // sampler (1 Hz region-progress + below-terrain sweeps).
        if (m_chaosActive)
        {
            if (m_chaos)
                m_chaos->Sample(now);
            if (m_chaosEndsAt > 0.0 && now >= m_chaosEndsAt)
            {
                m_chaosActive = false;
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos run complete — run tf_validate for the report");
            }
        }
    }

    void TFBotSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;

        // Per-tick path: no allocations — POD input copies + weapon fire only.
        // Seated bots ride the same path: TFServerSim forwards their input to
        // TFVehicleSystem::ServerHandleSeatedInput (driver throttle/steer).
        const double now = Now();
        for (Bot& b : m_bots)
        {
            if (!b.wantMove)
                continue;
            b.input.seq = ++b.seq;
            m_ctx->serverSim->EnqueueInput(b.id, b.input);

            if (b.state == BotState::Fighting && b.targetEntity != 0)
                TryFire(b, now);
        }
    }

    // ---------------------------------------------------------------------------
    // W2 cross-agent contract
    // ---------------------------------------------------------------------------

    void TFBotSystem::ServerSetBotCount(uint32_t n)
    {
        if (!m_initialized || !m_ctx)
            return;
        if (!m_ctx->IsAuthority())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] ServerSetBotCount ignored — not the authority");
            return;
        }

        n = std::min(n, kTFMaxBots);

        while (m_bots.size() > n)
        {
            DespawnBot(m_bots.back());
            m_bots.pop_back();
        }
        while (m_bots.size() < n)
            SpawnBotSlot(static_cast<uint32_t>(m_bots.size()));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] bot count -> %u", BotCount());
    }

    void TFBotSystem::SpawnBotSlot(uint32_t slot)
    {
        Bot b;
        b.id = kTFBotIdBase + slot;
        b.faction = static_cast<FactionId>(1 + (slot % 3)); // round-robin MRA/AUC/HLX
        b.cls = static_cast<ClassId>(m_rng() % kSelectableClasses);
        b.state = BotState::Deploying;
        b.strafePhase = static_cast<float>(slot) * 1.7f;
        b.feelerPhase = static_cast<uint8_t>(slot & 3u); // desync the 1-in-4 feeler patterns
        const double now = Now();
        b.nextThinkAt = now + static_cast<double>(slot) * 0.031; // stagger the 5 Hz brains
        b.nextSpawnTryAt = now;
        ResolveLoadout(b);
        m_bots.push_back(b);
    }

    void TFBotSystem::DespawnBot(Bot& bot)
    {
        if (!m_ctx || !m_ctx->players)
            return;
        if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id))
        {
            TF_VehicleSeatOp op{};
            op.vehicleEntity = bot.vehicleEntity;
            m_ctx->vehicles->ServerHandleSeatOp(bot.id, op, false);
        }
        PawnInfo pawn;
        if (m_ctx->players->GetPawnByPlayer(bot.id, pawn) && pawn.alive)
        {
            // Real kill path first so TFServerSim/TFDamageSystem drop their state
            // (EvPlayerKilled), then remove the player record entirely.
            m_ctx->players->ServerKillPawn(pawn.entity, kInvalidPlayer, kInvalidWeapon, false);
        }
        m_ctx->players->ServerHandlePlayerDisconnect(bot.id);
        bot.wantMove = false;
    }

    // ---------------------------------------------------------------------------
    // Loadout
    // ---------------------------------------------------------------------------

    void TFBotSystem::ResolveLoadout(Bot& bot) const
    {
        bot.weapon = kInvalidWeapon;
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        // Faction rifle first; any faction/common shootable primary as fallback.
        const WeaponDef* pick = nullptr;
        for (const WeaponDef& w : m_ctx->data->AllWeapons())
        {
            if (w.kind == "melee" || w.kind == "beam")
                continue;
            if (w.faction == bot.faction && w.slot == "rifle")
            {
                pick = &w;
                break;
            }
            if (!pick && (w.faction == bot.faction || w.faction == FactionId::None) &&
                (w.slot == "rifle" || w.slot == "carbine" || w.slot == "lmg"))
                pick = &w;
        }
        if (!pick)
            return;

        const WeaponDef def = m_ctx->data->ResolveWeapon(pick->id, bot.faction);
        bot.weapon = def.id;
        bot.rofIntervalSec = 60.0f / std::max(1.0f, def.rofRpm);
        bot.magSize = std::max(1, def.magSize);
        bot.reloadSec = std::max(0.5f, def.reloadSec);
        bot.magLeft = bot.magSize;
    }

    // ---------------------------------------------------------------------------
    // Spawn / respawn (the real pipeline)
    // ---------------------------------------------------------------------------

    void TFBotSystem::TrySpawn(Bot& bot, double now)
    {
        bot.nextSpawnTryAt = now + kSpawnRetrySec;
        if (!m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        // Faction select through both registries, exactly like the network path
        // (TFServerSim::HandleFactionSelect -> SetPlayerFaction; TFPlayerSystem
        // accepts either registry in ServerHandleSpawnRequest).
        if (m_ctx->serverSim)
            m_ctx->serverSim->SetPlayerFaction(bot.id, bot.faction);
        m_ctx->players->ServerHandleFactionSelect(bot.id, bot.faction);

        if (bot.weapon == kInvalidWeapon)
            ResolveLoadout(bot);

        // Skyanchor spawn request — the same validation + ServerSpawnPawn +
        // TF_SpawnReply flow clients use. The reply send for a bot id is a safe
        // no-op (verified: NetworkManager::SendToClient drops unknown client ids;
        // NetRole::Standalone skips the send before reaching NetworkManager).
        TF_SpawnRequest req{};
        req.classId = static_cast<uint8_t>(bot.cls);
        req.spawnKind = 0; // skyanchor
        m_ctx->players->ServerHandleSpawnRequest(bot.id, req);
        ++m_spawnRequests;
        // Success is observed on the next Think() via GetPawnByPlayer().alive.
    }

    // ---------------------------------------------------------------------------
    // Brain (5 Hz per bot, staggered)
    // ---------------------------------------------------------------------------

    void TFBotSystem::Think(Bot& bot, double now)
    {
        PawnInfo self;
        const bool alive = m_ctx->players && m_ctx->players->GetPawnByPlayer(bot.id, self) && self.alive;

        if (!alive)
        {
            bot.wantMove = false;
            bot.targetEntity = 0;
            bot.vehicleEntity = 0; // TFVehicleSystem unseats the corpse on EvPlayerKilled
            if (bot.state == BotState::Deploying)
            {
                if (now >= bot.nextSpawnTryAt)
                    TrySpawn(bot, now);
            }
            else if (bot.state != BotState::Dead)
            {
                bot.state = BotState::Dead;
                bot.nextSpawnTryAt = now + kRespawnWaitSec; // 8 s server timer + margin
            }
            else if (now >= bot.nextSpawnTryAt)
            {
                TrySpawn(bot, now);
            }
            return;
        }

        if (bot.state == BotState::Dead || bot.state == BotState::Deploying)
        {
            // Fresh pawn: reset per-life state.
            bot.state = BotState::Moving;
            bot.magLeft = bot.magSize;
            bot.reloadDoneAt = 0.0;
            bot.vehicleEntity = 0;
            bot.enterTries = 0;
            bot.lowHealth = false;
            bot.reverseUntil = 0.0;
            bot.wedgeCount = 0;
            bot.lastWedgeAt = 0.0;
            bot.lastPool = -1.0f;
            bot.underFire = false;
            if (m_chaosActive)
                bot.chaosScatterPending = true; // re-scatter every chaos life for coverage
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
            bot.jumping = false;
            ResetAvoidance(bot, self.pos[0], self.pos[2], now); // W12: fresh nav state per life
            bot.unstickCount = 0;
        }

        ThinkAlive(bot, self, now);
    }

    void TFBotSystem::ThinkAlive(Bot& bot, const PawnInfo& self, double now)
    {
        // W9 bots-v2: a REFUSED exit (airborne Vulture — the seat-op is
        // landed-gated) leaves the bot seated after ExitVehicle already reset
        // its plan. Re-latch into Driving so it keeps flying/landing instead of
        // walking-in-place through the seated-input forwarder. The one-tick
        // exit latch after a SUCCESSFUL exit also reports IsSeated, but its
        // seat slot is already freed — the seat scan below tells them apart.
        if (bot.state != BotState::Driving && m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id))
        {
            EntityId riding = 0;
            m_ctx->vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                {
                    if (riding != 0)
                        return;
                    for (uint8_t i = 0; i < v.seatCount; ++i)
                    {
                        if (v.seats[i] == bot.id)
                        {
                            riding = v.entity;
                            return;
                        }
                    }
                });
            if (riding != 0)
            {
                bot.state = BotState::Driving;
                bot.vehicleEntity = riding;
                bot.stuckSince = now; // fresh wedge window for the retry
            }
        }

        // A seated bot rides the vehicle: infantry combat, stuck-jump and walking
        // objectives do not apply until it dismounts.
        if (bot.state == BotState::Driving)
        {
            ThinkDriving(bot, self, now);
            return;
        }

        // --- chaos: teleport-scatter for coverage (start of run + every life) ---
        if (m_chaosActive && bot.chaosScatterPending)
        {
            bot.chaosScatterPending = false;
            ChaosScatter(bot, now);
            bot.wantMove = false; // rebuild input from the new position next think
            return;
        }

        // --- chaos: periodic deployable / vehicle-purchase exercise ---
        if (m_chaosActive)
        {
            ChaosTryUtility(bot, now);
            // W9 pilots: buy the designated ride at the faction terminal.
            if (ChaosIsPilot(bot) && now >= bot.pilotBuyAt)
                ChaosPilotTryPurchase(bot, self, now);
        }

        // --- stuck detection: pos unchanged > 2 s -> hold jump ---
        const float dsx = self.pos[0] - bot.stuckRefPos[0];
        const float dsy = self.pos[1] - bot.stuckRefPos[1];
        const float dsz = self.pos[2] - bot.stuckRefPos[2];
        if (dsx * dsx + dsy * dsy + dsz * dsz > kStuckEpsM * kStuckEpsM)
        {
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
            bot.jumping = false;
        }
        else if (now - bot.stuckSince > kStuckWindowSec)
        {
            bot.jumping = true;
        }

        TF_ClientInput in{};
        in.weaponSlot = 0;

        // Fitness drives engage-vs-advance below.
        bot.lowHealth = HealthFrac(bot, self) < kLowHealthFrac;

        // Under-fire detection (W9 bots-v2): the pool dropped since last think.
        const float pool = self.health + self.shield;
        bot.underFire = bot.lastPool >= 0.0f && pool < bot.lastPool - 0.5f;
        bot.lastPool = pool;

        // --- combat: nearest enemy alive pawn within 60 m with rough LoS ---
        float targetPos[3];
        if (AcquireTarget(bot, self, bot.targetEntity, targetPos))
        {
            bot.state = BotState::Fighting;
            const float dx = targetPos[0] - self.pos[0];
            const float dz = targetPos[2] - self.pos[2];
            const float dy = (targetPos[1] + kChestHeightM) - (self.pos[1] + kTFEyeHeightM);
            const float dist = std::sqrt(dx * dx + dz * dz);

            in.viewYaw = std::atan2(dx, dz); // TF yaw basis: forward = (sin, 0, cos)
            in.viewPitch = std::atan2(dy, std::max(dist, 0.001f));
            // strafe oscillation either way; push when fit, give ground (target
            // stays in the sights — moveY is body-relative) when hurt/reloading.
            const bool reloading = bot.magLeft <= 0 && now < bot.reloadDoneAt;
            in.moveX = static_cast<int8_t>(std::sin(now * 2.6 + bot.strafePhase) * 110.0);
            if (bot.lowHealth || reloading)
                in.moveY = -90;
            else
                in.moveY = static_cast<int8_t>(dist > kHoldFireCloseM ? 70 : 0);
        }
        else
        {
            bot.targetEntity = 0;

            // --- objective (region march, contested-biased). W9 chaos pilots
            //     keep their scripted objective (terminal pad, then the far
            //     ride target) — the scorer would clobber it every think. ---
            if (!(m_chaosActive && ChaosIsPilot(bot)))
                PickObjective(bot, self.pos);

            // --- chaos: randomized objective override (coverage over optimality);
            //     fireteam cohesion is skipped in chaos to maximize scatter ---
            if (m_chaosActive)
                ChaosMaybeReroll(bot, now);

            // --- fireteam cohesion: followers adopt the leader's objective and
            //     regroup on the leader when strung out ---
            if (const Bot* leader = FireteamLeader(bot); !m_chaosActive && leader && leader != &bot)
            {
                PawnInfo lp;
                if (m_ctx->players->GetPawnByPlayer(leader->id, lp) && lp.alive)
                {
                    if (leader->objectiveRegion != kInvalidRegion)
                    {
                        bot.objectiveRegion = leader->objectiveRegion;
                        bot.objectiveX = leader->objectiveX;
                        bot.objectiveZ = leader->objectiveZ;
                    }
                    const float ldx = lp.pos[0] - self.pos[0];
                    const float ldz = lp.pos[2] - self.pos[2];
                    if (ldx * ldx + ldz * ldz > kRegroupBeyondM * kRegroupBeyondM)
                    {
                        bot.objectiveRegion = kInvalidRegion; // regroup leg, not a capture
                        bot.objectiveX = lp.pos[0];
                        bot.objectiveZ = lp.pos[2];
                    }
                }
            }

            // --- vehicle plan: long march + friendly wheels nearby -> drive ---
            if (!TryUseVehicle(bot, self, now, in))
            {
                bot.state = BotState::Moving;
                const float dx = bot.objectiveX - self.pos[0];
                const float dz = bot.objectiveZ - self.pos[2];
                const float dist = std::sqrt(dx * dx + dz * dz);

                in.viewYaw = std::atan2(dx, dz);
                in.viewPitch = 0.0f;
                in.moveY = 127;
                in.moveX = 0;
                // Chaos: weave while marching so bots hit obstacles at oblique
                // angles and exercise the slide path, not just head-on stops.
                if (m_chaosActive)
                    in.moveX = static_cast<int8_t>(std::sin(now * 1.7 + bot.strafePhase) * 70.0);
                if (dist > kSprintBeyondM)
                    in.buttons |= TFB_Sprint;
            }
        }

        // W12 bot-navigation: walking legs get local obstacle avoidance (feeler
        // steering + no-progress unstick). Combat movement is strafe-driven and
        // keeps the target in the sights, so only the march/approach states
        // steer around geometry; other states just re-arm the progress window.
        if (bot.state == BotState::Moving || bot.state == BotState::ToVehicle)
        {
            ApplyAvoidance(bot, self, now, in);
        }
        else
        {
            bot.moveRefPos[0] = self.pos[0];
            bot.moveRefPos[1] = self.pos[2];
            bot.moveRefAt = now;
        }

        if (bot.jumping)
            in.buttons |= TFB_Jump;

        // W9 bots-v2: situational class ability (rate-limited inside).
        TryClassAbility(bot, self, now, in);

        bot.input = in; // seq is stamped per fixed tick in FixedUpdate
        bot.wantMove = true;
    }

    // ---------------------------------------------------------------------------
    // Local obstacle avoidance (W12 bot-navigation)
    //
    // W11 root cause: bots nosed into watchtower leg-cage OBBs and stalemated
    // (no fights -> the chaos abilities gate failed and the walkable-gates data
    // was reverted). Feeler rays steer marching bots around static geometry
    // BEFORE they wedge; the coarse no-progress detector below catches whatever
    // slips through and breaks it with a random escape leg, escalating to the
    // existing chaos teleport-scatter after three consecutive failures (chaos
    // mode only — the scatter path already resets objectives + stuck state).
    // ---------------------------------------------------------------------------

    void TFBotSystem::ResetAvoidance(Bot& bot, float x, float z, double now)
    {
        bot.moveRefPos[0] = x;
        bot.moveRefPos[1] = z;
        bot.moveRefAt = now;
        bot.unstickUntil = 0.0;
        bot.backTurnUntil = 0.0;
        bot.blockedYawUntil = 0.0;
        bot.lastAvoidSide = 0;
    }

    float TFBotSystem::FeelerClearance(const float origin[3], float yaw) const
    {
        ::PhysicsSystem* physics = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetPhysics() : nullptr;
        if (!physics || !physics->GetJoltSystem())
            return kFeelerLenM; // no live Jolt world: nothing to feel, walk on
        const DirectX::XMFLOAT3 o{origin[0], origin[1], origin[2]};
        const DirectX::XMFLOAT3 d{std::sin(yaw), 0.0f, std::cos(yaw)};
        const RaycastHit hit = physics->RaycastFiltered(o, d, kFeelerLenM, CollisionLayers::WorldStatic);
        return hit.hasHit ? std::min(hit.distance, kFeelerLenM) : kFeelerLenM;
    }

    float TFBotSystem::SteerFeelers(Bot& bot, const PawnInfo& self, double now, float desiredYaw)
    {
        // Pocket escape in progress: hold the back-turn heading, no rays.
        if (now < bot.backTurnUntil)
            return bot.backTurnYaw;

        // Stagger: full pattern 1-in-4 thinks per bot in open field; every
        // think while actively working an obstacle (fresh blocked memory).
        bot.feelerPhase = static_cast<uint8_t>((bot.feelerPhase + 1u) & 3u);
        const bool active = now < bot.blockedYawUntil;
        if (bot.feelerPhase != 0 && !active)
            return desiredYaw;

        // Path memory: while the remembered blocked heading is fresh, bias away
        // from it instead of re-probing head-on every time the scorer re-aims.
        const auto memoryBias = [&](float yaw)
        {
            if (now >= bot.blockedYawUntil)
                return yaw;
            const float delta = WrapPi(yaw - bot.lastBlockedYaw);
            if (std::fabs(delta) >= kFeelerSideRad)
                return yaw;
            float side = static_cast<float>(bot.lastAvoidSide);
            if (side == 0.0f)
                side = delta >= 0.0f ? 1.0f : -1.0f;
            return WrapPi(yaw + side * kFeelerSideRad);
        };

        const float origin[3] = {self.pos[0], self.pos[1] + kChestHeightM, self.pos[2]};
        const float fwd = FeelerClearance(origin, desiredYaw);
        if (fwd >= kFeelerLenM)
            return memoryBias(desiredYaw); // clear ahead

        // Forward blocked: remember the heading and probe the side feelers.
        ++m_feelerBlocked;
        bot.lastBlockedYaw = desiredYaw;
        bot.blockedYawUntil = now + kBlockedMemorySec;

        const float left = FeelerClearance(origin, WrapPi(desiredYaw - kFeelerSideRad));
        const float right = FeelerClearance(origin, WrapPi(desiredYaw + kFeelerSideRad));
        if (left < kFeelerLenM)
            ++m_feelerBlocked;
        if (right < kFeelerLenM)
            ++m_feelerBlocked;

        if (left < kFeelerLenM && right < kFeelerLenM)
        {
            // Boxed in: back-turn 120 deg for a second and re-approach.
            const float sign = (m_rng() & 1u) != 0u ? 1.0f : -1.0f;
            bot.backTurnYaw = WrapPi(desiredYaw + sign * kBackTurnRad);
            bot.backTurnUntil = now + kBackTurnSec;
            bot.lastAvoidSide = 0;
            return bot.backTurnYaw;
        }

        // Steer along the least-blocked feeler; ties break away from the
        // remembered blocked heading via the previous side, else randomly.
        float side;
        if (left > right)
            side = -1.0f;
        else if (right > left)
            side = 1.0f;
        else if (bot.lastAvoidSide != 0)
            side = static_cast<float>(bot.lastAvoidSide);
        else
            side = (m_rng() & 1u) != 0u ? 1.0f : -1.0f;
        bot.lastAvoidSide = side > 0.0f ? int8_t{1} : int8_t{-1};
        return WrapPi(desiredYaw + side * kAvoidSteerRad);
    }

    void TFBotSystem::ApplyAvoidance(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in)
    {
        // --- no-progress unstick detector (coarser than the jump nudge) ---
        const float mdx = self.pos[0] - bot.moveRefPos[0];
        const float mdz = self.pos[2] - bot.moveRefPos[1];
        if (mdx * mdx + mdz * mdz >= kUnstickMinMoveM * kUnstickMinMoveM)
        {
            bot.moveRefPos[0] = self.pos[0];
            bot.moveRefPos[1] = self.pos[2];
            bot.moveRefAt = now;
            bot.unstickCount = 0; // making progress again
        }
        else if (now - bot.moveRefAt >= kUnstickWindowSec)
        {
            bot.moveRefPos[0] = self.pos[0];
            bot.moveRefPos[1] = self.pos[2];
            bot.moveRefAt = now;
            ++m_unsticks;
            if (bot.unstickCount < kUnstickScatterAfter)
                ++bot.unstickCount;
            if (bot.unstickCount >= kUnstickScatterAfter && m_chaosActive)
            {
                // Persistent stuck: fall back to the chaos teleport-scatter
                // (consumed at the top of the next alive think — it re-rolls
                // the drop point and resets objective + stuck state there).
                bot.chaosScatterPending = true;
                bot.unstickCount = 0;
                ++m_stuckTeleports;
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] bot 0x%08X persistently stuck -> chaos scatter", bot.id);
            }
            else
            {
                // Random 90-150 deg escape heading for 2 s, then re-seek.
                const float sign = (m_rng() & 1u) != 0u ? 1.0f : -1.0f;
                const float mag = 1.5708f + static_cast<float>(m_rng() % 61u) * 0.0174533f;
                bot.unstickYaw = WrapPi(in.viewYaw + sign * mag);
                bot.unstickUntil = now + kUnstickSteerSec;
            }
        }

        // Escape leg overrides the objective heading while it runs.
        if (now < bot.unstickUntil)
        {
            in.viewYaw = bot.unstickYaw;
            in.moveY = 127;
        }

        // Feelers steer whatever heading survived the above.
        in.viewYaw = SteerFeelers(bot, self, now, in.viewYaw);
    }

    // ---------------------------------------------------------------------------
    // Class abilities (W9 bots-v2; real seam when the abilities lane is present)
    // ---------------------------------------------------------------------------

    void TFBotSystem::TryClassAbility(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in)
    {
        if (now < bot.nextAbilityAt || !m_ctx->players)
            return;

        bool want = false;
        switch (bot.cls)
        {
        case ClassId::Medtech:
        {
            // Hurt self, or a hurt friendly inside heal reach.
            want = HealthFrac(bot, self) < kMedtechHurtFrac;
            if (!want)
            {
                m_ctx->players->ForEachAlivePawn(
                    [&](const PawnInfo& p)
                    {
                        if (want || p.entity == self.entity || p.faction != bot.faction)
                            return;
                        const float dx = p.pos[0] - self.pos[0];
                        const float dz = p.pos[2] - self.pos[2];
                        if (dx * dx + dz * dz > kMedtechHealRadiusM * kMedtechHealRadiusM)
                            return;
                        float maxPool = 1000.0f; // ClassDef defaults
                        if (m_ctx->data)
                            if (const ClassDef* cd = m_ctx->data->GetClass(p.cls))
                                maxPool = std::max(1.0f, cd->health + cd->shield);
                        if ((p.health + p.shield) / maxPool < kMedtechHurtFrac)
                            want = true;
                    });
            }
            break;
        }
        case ClassId::Bulwark:
            // Overshield when actually taking hits (or brawling on low health).
            want = bot.underFire || (bot.state == BotState::Fighting && bot.lowHealth);
            break;
        case ClassId::Striker:
            // Jets to close a wide gap on the current target.
            if (bot.state == BotState::Fighting && bot.targetEntity != 0)
            {
                PawnInfo tp;
                if (m_ctx->players->GetPawnByEntity(bot.targetEntity, tp) && tp.alive)
                {
                    const float dx = tp.pos[0] - self.pos[0];
                    const float dz = tp.pos[2] - self.pos[2];
                    want = dx * dx + dz * dz > kStrikerGapM * kStrikerGapM;
                }
            }
            break;
        default:
            break; // Ghost/Fabricator: no situational trigger this wave
        }
        if (!want)
            return;

        bot.nextAbilityAt = now + kAbilityCheckSec + static_cast<double>(m_rng() % 10u) * 0.1;

        // Input-driven path: harmless if nothing consumes the bit yet, and the
        // moment an input-handled ability lane lands, bots exercise it for free.
        in.buttons |= TFB_Ability;

        // Direct seam (compile-time optional; see the shim at the top of file).
        if (TryUseAbilitySeam(m_ctx, bot.id))
        {
            ++m_abilityUses;
            if (m_chaos)
                m_chaos->NoteAbilityUse();
        }
    }

    // ---------------------------------------------------------------------------
    // Vehicles (driver seat 0 only; TFServerSim forwards a seated bot's
    // TF_ClientInput to TFVehicleSystem::ServerHandleSeatedInput)
    // ---------------------------------------------------------------------------

    bool TFBotSystem::TryUseVehicle(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in)
    {
        if (!m_ctx->vehicles)
            return false;

        // Only worth driving when the march is long.
        const float odx = bot.objectiveX - self.pos[0];
        const float odz = bot.objectiveZ - self.pos[2];
        if (odx * odx + odz * odz < kVehSeekBeyondM * kVehSeekBeyondM)
        {
            bot.vehicleEntity = 0;
            return false;
        }

        // Scan for a ride (rate-limited after failures).
        if (bot.vehicleEntity == 0)
        {
            if (now < bot.vehicleRetryAt)
                return false;
            EntityId best = 0;
            float bestD2 = kVehScanRadiusM * kVehScanRadiusM;
            m_ctx->vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                {
                    if (v.faction != bot.faction || v.hp <= 0.0f || v.deployed)
                        return; // enemy, dead, or a parked Aegis spawn (leave it deployed)
                    if (v.seatCount == 0 || v.seats[0] != kInvalidPlayer)
                        return; // no driver seat to take
                    const float dx = v.pos[0] - self.pos[0];
                    const float dz = v.pos[2] - self.pos[2];
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        best = v.entity;
                    }
                });
            if (best == 0)
            {
                bot.vehicleRetryAt = now + kVehRetrySec;
                return false;
            }
            bot.vehicleEntity = best;
            bot.enterTries = 0;
        }

        // Validate the plan every think (vehicle may die / get taken meanwhile).
        TFVehicleInfo veh;
        if (!m_ctx->vehicles->GetVehicleInfo(bot.vehicleEntity, veh) || veh.hp <= 0.0f || veh.deployed ||
            veh.seatCount == 0 || veh.seats[0] != kInvalidPlayer)
        {
            bot.vehicleEntity = 0;
            bot.vehicleRetryAt = now + kVehRetrySec;
            return false;
        }

        const float dx = veh.pos[0] - self.pos[0];
        const float dz = veh.pos[2] - self.pos[2];
        const float d2 = dx * dx + dz * dz;

        if (d2 <= kVehEnterTryM * kVehEnterTryM)
        {
            // At the hull: take the driver seat through the real seat-op path.
            TF_VehicleSeatOp op{};
            op.vehicleEntity = veh.entity;
            op.seatIndex = 0;
            m_ctx->vehicles->ServerHandleSeatOp(bot.id, op, true);
            if (m_ctx->vehicles->IsSeated(bot.id))
            {
                bot.state = BotState::Driving;
                bot.stuckRefPos[0] = self.pos[0];
                bot.stuckRefPos[1] = self.pos[1];
                bot.stuckRefPos[2] = self.pos[2];
                bot.stuckSince = now;
                bot.jumping = false;
                bot.reverseUntil = 0.0; // fresh ride: clear wedge-recovery state
                bot.wedgeCount = 0;
                bot.lastWedgeAt = 0.0;
                in = TF_ClientInput{}; // neutral until ThinkDriving steers next think
                return true;
            }
            if (++bot.enterTries >= kVehMaxEnterTries)
            {
                bot.vehicleEntity = 0;
                bot.vehicleRetryAt = now + kVehRetrySec;
                return false;
            }
        }

        // Walk to the hull; this plan owns the movement this think.
        bot.state = BotState::ToVehicle;
        in.viewYaw = std::atan2(dx, dz);
        in.viewPitch = 0.0f;
        in.moveY = 127;
        in.moveX = 0;
        if (d2 > kSprintBeyondM * kSprintBeyondM)
            in.buttons |= TFB_Sprint;
        return true;
    }

    void TFBotSystem::ThinkDriving(Bot& bot, const PawnInfo& self, double now)
    {
        TFVehicleInfo veh;
        const bool seated = m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id) &&
                            m_ctx->vehicles->GetVehicleInfo(bot.vehicleEntity, veh);
        if (!seated || veh.hp <= 0.0f)
        {
            // Ejected / destroyed / despawned underneath us: back on foot.
            bot.state = BotState::Moving;
            bot.vehicleEntity = 0;
            bot.vehicleRetryAt = now + kVehRetrySec;
            bot.wantMove = false;
            return;
        }

        const bool vtol = veh.vehId == VehicleId::Vulture;

        // Keep the objective fresh while riding (ownership can flip mid-drive).
        // In chaos the scatter/pilot objective is kept: re-rolls happen on foot
        // only, and the pilots' far target must survive the whole ride.
        if (!m_chaosActive)
            PickObjective(bot, self.pos);

        const float dx = bot.objectiveX - self.pos[0];
        const float dz = bot.objectiveZ - self.pos[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(dx, dz);
        const float yawErr = WrapPi(desiredYaw - veh.yaw);

        // Wedge detection: ride pose unchanged too long. VTOLs get more slack —
        // a gunship yaws in place at zero ground speed while lining up.
        bool wedged = false;
        const float dsx = self.pos[0] - bot.stuckRefPos[0];
        const float dsz = self.pos[2] - bot.stuckRefPos[2];
        if (dsx * dsx + dsz * dsz > kStuckEpsM * kStuckEpsM)
        {
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
        }
        else if (now - bot.stuckSince > (vtol ? kVulStuckSec : kVehStuckSec))
        {
            wedged = true;
        }

        TF_ClientInput in{};
        in.weaponSlot = 0;
        in.viewYaw = desiredYaw;
        in.viewPitch = 0.0f;

        if (vtol)
        {
            // --- Vulture flight: altitude hold + fly at the objective, then
            //     descend and take the landed-gated exit on arrival. A wedged
            //     hover (world edge / AGL ceiling) also lands and walks. ---
            const float terrain = m_ctx->world ? m_ctx->world->TerrainHeightAt(veh.pos[0], veh.pos[2]) : 0.0f;
            const float agl = veh.pos[1] - terrain;
            in.moveX = static_cast<int8_t>(std::clamp(yawErr * 1.2f, -1.0f, 1.0f) * 127.0f);

            if (dist < kVulLandWithinM || wedged)
            {
                in.buttons |= TFB_Crouch; // descend onto the point
                in.moveY = static_cast<int8_t>((dist > 20.0f && !wedged) ? 45 : 0);
                if (agl <= kVulExitAglM || wedged)
                {
                    // Server's VehicleLanded gate opens at 2 m AGL (clearance-
                    // aware with a Jolt hull, so pads/roofs count even though
                    // this terrain-AGL proxy can't see them). A refused exit
                    // (still airborne) re-latches into Driving next think and
                    // the descent simply continues.
                    ExitVehicle(bot, now);
                    return;
                }
            }
            else
            {
                if (agl < kVulCruiseMinAglM)
                    in.buttons |= TFB_Jump; // climb
                else if (agl > kVulCruiseMaxAglM)
                    in.buttons |= TFB_Crouch; // descend back into the cruise band
                in.moveY = static_cast<int8_t>(std::fabs(yawErr) < 0.9f ? 127 : 40);
            }

            bot.input = in;
            bot.wantMove = true;
            return;
        }

        // --- ground hover-rig ---

        if (dist < kVehExitWithinM)
        {
            ExitVehicle(bot, now);
            return;
        }

        if (wedged)
        {
            // First wedge on this leg: reverse out and try again. A second
            // wedge inside the window means the ride is truly stuck — walk.
            if (bot.wedgeCount > 0 && now - bot.lastWedgeAt < kVehWedgeWindowSec)
            {
                ExitVehicle(bot, now);
                return;
            }
            ++bot.wedgeCount;
            bot.lastWedgeAt = now;
            bot.reverseUntil = now + kVehReverseSec;
            bot.reverseSteer = (m_rng() & 1u) != 0u ? static_cast<int8_t>(90) : static_cast<int8_t>(-90);
            bot.stuckSince = now; // fresh window after the recovery attempt
        }

        // Reverse-out recovery phase: back away swinging the tail.
        if (now < bot.reverseUntil)
        {
            in.viewYaw = veh.yaw; // hold the current heading while backing out
            in.moveY = -100;
            in.moveX = bot.reverseSteer;
            bot.input = in;
            bot.wantMove = true;
            return;
        }

        // Drive: moveY = throttle, moveX = steer (ServerHandleSeatedInput mapping).
        float steer = std::clamp(yawErr * 1.5f, -1.0f, 1.0f);
        float throttle = std::fabs(yawErr) > kVehHardTurnRad ? 60.0f : 127.0f;

        // Terrain look-ahead: a wall-steep rise dead ahead means steer along
        // the lower side probe instead of nosing in and wedging (plateau walls
        // and mesa edges; the Jolt hull can't climb them either).
        if (m_ctx->world)
        {
            const float here = m_ctx->world->TerrainHeightAt(veh.pos[0], veh.pos[2]);
            const auto riseAt = [&](float relYaw)
            {
                const float a = veh.yaw + relYaw;
                return m_ctx->world->TerrainHeightAt(veh.pos[0] + std::sin(a) * kVehProbeAheadM,
                                                     veh.pos[2] + std::cos(a) * kVehProbeAheadM) -
                       here;
            };
            if (riseAt(0.0f) > kVehClimbLimitM)
            {
                const float left = riseAt(-kVehProbeSideRad);
                const float right = riseAt(kVehProbeSideRad);
                steer = left < right ? -1.0f : 1.0f;
                throttle = std::min(left, right) > kVehClimbLimitM ? 40.0f : 80.0f;
            }
        }

        in.moveX = static_cast<int8_t>(steer * 127.0f);
        in.moveY = static_cast<int8_t>(throttle);

        bot.input = in;
        bot.wantMove = true;
    }

    void TFBotSystem::ExitVehicle(Bot& bot, double now)
    {
        if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id))
        {
            TF_VehicleSeatOp op{};
            op.vehicleEntity = bot.vehicleEntity; // ignored by the exit path (leave own seat)
            m_ctx->vehicles->ServerHandleSeatOp(bot.id, op, false);
        }
        bot.state = BotState::Moving;
        bot.vehicleEntity = 0;
        bot.vehicleRetryAt = now + kVehRetrySec;
        bot.wantMove = false; // fresh walking input next think
    }

    // ---------------------------------------------------------------------------
    // Chaos exercise mode (bots-chaos lane, 2026-07-10)
    // ---------------------------------------------------------------------------

    void TFBotSystem::ServerStartChaos(uint32_t botCount, float seconds)
    {
        if (!m_initialized || !m_ctx)
            return;
        if (!m_ctx->IsAuthority())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] ServerStartChaos ignored — not the authority");
            return;
        }

        if (botCount == 0)
        {
            m_chaosActive = false;
            m_chaosEndsAt = 0.0;
            ServerSetBotCount(0);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos stopped, bots despawned");
            return;
        }

        ServerSetBotCount(std::min(botCount, kTFMaxBots));
        const double now = Now();
        m_chaosActive = true;
        m_chaosEndsAt = seconds > 0.0f ? now + static_cast<double>(seconds) : 0.0;
        m_chaosDeployTries = 0;
        m_chaosVehicleTries = 0;
        m_chaosVehiclePurchases = 0;
        for (Bot& b : m_bots)
        {
            b.chaosScatterPending = true; // scatter on the first alive think
            // Hold the first drop point a while (fight/capture there) before
            // the randomized re-rolls kick in; stagger per slot.
            b.chaosRerollAt = now + 15.0 + static_cast<double>(b.id - kTFBotIdBase) * 0.7;
            b.chaosUtilityAt = now + 5.0 + static_cast<double>(m_rng() % 60) * 0.1;
            b.pilotBuyAt = 0.0;
        }

        // W9 pilots: bankroll + rank the designated pilots through the REAL
        // progression paths (grant/award/unlock — the same seams tf_giveflux
        // and gameplay use) so the Vulture/Drifter purchase gates actually
        // open, then let the validated terminal purchase do the rest. Every
        // bot gets Drifter money so incidental terminal purchases can succeed
        // too. The 750-flux wallet cap forces the grant/unlock interleave.
        if (m_ctx->progression)
        {
            for (Bot& b : m_bots)
            {
                m_ctx->progression->ServerGrantFlux(b.id, kBotChaosFluxGrant);
                if (!ChaosIsPilot(b))
                    continue;
                if ((b.id - kTFBotIdBase) == kChaosVulturePilotSlot)
                {
                    m_ctx->progression->ServerAwardXP(b.id, kPilotXPGrant, kXPReasonKill); // rank >= 15
                    m_ctx->progression->ServerGrantFlux(b.id, kFluxWalletCap);
                    m_ctx->progression->ServerTryUnlock(b.id, "veh_ravager"); // Vulture prereq
                    m_ctx->progression->ServerTryUnlock(b.id, "veh_vulture");
                }
                m_ctx->progression->ServerGrantFlux(b.id, kFluxWalletCap); // spawn money
            }
        }

        if (m_chaos)
        {
            m_chaos->OnChaosStart(BotCount(), seconds);
            m_chaos->SetAbilitySeamPresent(AbilitySeamPresent(m_ctx));
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] CHAOS: %u bots for %.0f s (0 = open-ended)", BotCount(),
                       static_cast<double>(seconds));
    }

    std::string TFBotSystem::ValidationReport()
    {
        if (!m_chaos)
            return "[TF-VALIDATE] RESULT: FAIL harness=missing";
        return m_chaos->BuildReport();
    }

    RegionId TFBotSystem::ChaosArenaRegion() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return kInvalidRegion;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        const RegionDef* first = nullptr;
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier == "skyanchor")
                continue;
            if (!first)
                first = &r;
            if (r.tier == "facility")
                return r.id; // biggest brawl space wins
        }
        return first ? first->id : kInvalidRegion;
    }

    void TFBotSystem::ChaosScatter(Bot& bot, double now)
    {
        if (!m_ctx->serverSim || !m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        if (cont.regions.empty())
            return;

        // W9 pilots: drop at the OWN faction skyanchor's vehicle terminal so the
        // purchase gate (friendly terminal within 25 m) deterministically opens.
        const uint32_t slot = bot.id - kTFBotIdBase;
        if (ChaosIsPilot(bot))
        {
            for (const RegionDef& r : cont.regions)
            {
                if (r.tier != "skyanchor" || r.homeFaction != bot.faction || !r.vehicleTerminal.has_value())
                    continue;
                const float tx = (*r.vehicleTerminal)[0];
                const float tz = (*r.vehicleTerminal)[1];
                const float ang = static_cast<float>(m_rng() % 6283u) * 0.001f;
                const float rad = 2.0f + static_cast<float>(m_rng() % 60u) * 0.1f; // 2-8 m off the pad
                const float x = tx + std::sin(ang) * rad;
                const float z = tz + std::cos(ang) * rad;
                const float y = m_ctx->world->TerrainHeightAt(x, z) + 0.5f;
                m_ctx->serverSim->TeleportPawn(bot.id, x, y, z);
                bot.objectiveRegion = r.id;
                bot.objectiveX = tx; // stay on the pad until the purchase lands
                bot.objectiveZ = tz;
                bot.stuckRefPos[0] = x;
                bot.stuckRefPos[1] = y;
                bot.stuckRefPos[2] = z;
                bot.stuckSince = now;
                bot.jumping = false;
                ResetAvoidance(bot, x, z, now); // W12: fresh nav window at the new drop
                bot.unstickCount = 0;
                bot.pilotBuyAt = now + 1.0; // let the teleport settle one tick
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos pilot: bot 0x%08X %s -> terminal r%u (%.0f, %.0f)",
                               bot.id, FactionTag(bot.faction), static_cast<unsigned>(r.id), x, z);
                return;
            }
            // No terminal for this faction (data changed): fall through to the
            // normal scatter below.
        }

        // Even slots -> the shared multi-faction arena (guaranteed contact and
        // contested points). Odd slots -> a random region this bot's faction can
        // actually capture (guaranteed single-attacker progress somewhere).
        const RegionDef* dest = nullptr;
        if ((slot % 2u) == 0u)
        {
            const RegionId arena = ChaosArenaRegion();
            for (const RegionDef& r : cont.regions)
            {
                if (r.id == arena)
                {
                    dest = &r;
                    break;
                }
            }
        }
        if (!dest)
        {
            std::vector<const RegionDef*> candidates;
            candidates.reserve(cont.regions.size());
            for (const RegionDef& r : cont.regions)
            {
                if (r.tier == "skyanchor")
                    continue;
                if (QueryRegionCapturable(m_ctx->regions, r.id, bot.faction, /*fallback*/ true))
                    candidates.push_back(&r);
            }
            if (!candidates.empty())
                dest = candidates[m_rng() % candidates.size()];
        }
        if (!dest)
            return;

        // Drop near a capture point (region center as fallback), on a 4-18 m
        // ring so stacked bots spread out and have to walk THROUGH the plateau
        // structures (collision exercise) to converge on the point.
        float cpX = dest->centerX, cpZ = dest->centerZ;
        if (!dest->capturePoints.empty())
        {
            const auto& cp = dest->capturePoints[m_rng() % dest->capturePoints.size()];
            cpX = cp[0];
            cpZ = cp[1];
        }
        const float ang = static_cast<float>(m_rng() % 6283u) * 0.001f;
        const float rad = 4.0f + static_cast<float>(m_rng() % 140u) * 0.1f;
        const float x = cpX + std::sin(ang) * rad;
        const float z = cpZ + std::cos(ang) * rad;
        const float y = m_ctx->world->TerrainHeightAt(x, z) + 0.5f;
        m_ctx->serverSim->TeleportPawn(bot.id, x, y, z);

        bot.objectiveRegion = dest->id;
        bot.objectiveX = cpX; // converge on the point itself, not the ring drop
        bot.objectiveZ = cpZ;
        bot.stuckRefPos[0] = x;
        bot.stuckRefPos[1] = y;
        bot.stuckRefPos[2] = z;
        bot.stuckSince = now;
        bot.jumping = false;
        ResetAvoidance(bot, x, z, now); // W12: fresh nav window at the new drop
        bot.unstickCount = 0;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos scatter: bot 0x%08X %s -> region %u (%.0f, %.0f)", bot.id,
                       FactionTag(bot.faction), static_cast<unsigned>(dest->id), x, z);
    }

    void TFBotSystem::ChaosMaybeReroll(Bot& bot, double now)
    {
        // W9 pilots keep their scripted objectives (terminal, then far region):
        // a re-roll mid-plan would strand the purchase or shorten the ride.
        if (ChaosIsPilot(bot))
            return;
        if (now < bot.chaosRerollAt)
            return;
        bot.chaosRerollAt = now + 6.0 + static_cast<double>(m_rng() % 100u) * 0.1; // 6-16 s
        if ((m_rng() & 1u) != 0u)
            return; // keep the lattice-scored objective half the time
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        std::vector<const RegionDef*> pool;
        pool.reserve(cont.regions.size());
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier != "skyanchor")
                pool.push_back(&r);
        }
        if (pool.empty())
            return;
        const RegionDef& r = *pool[m_rng() % pool.size()];
        bot.objectiveRegion = r.id;
        bot.objectiveX = r.centerX;
        bot.objectiveZ = r.centerZ;
        if (!r.capturePoints.empty())
        {
            const auto& cp = r.capturePoints[m_rng() % r.capturePoints.size()];
            bot.objectiveX = cp[0];
            bot.objectiveZ = cp[1];
        }
    }

    void TFBotSystem::ChaosTryUtility(Bot& bot, double now)
    {
        if (now < bot.chaosUtilityAt)
            return;
        bot.chaosUtilityAt = now + 8.0 + static_cast<double>(m_rng() % 80u) * 0.1; // 8-16 s

        // Deployables through the real validated entry point (class-gated;
        // refusals — slope/spacing/hostile-region/limit — are free exercise).
        if (m_ctx->deployables)
        {
            if (bot.cls == ClassId::Fabricator)
            {
                const DeployableKind kind =
                    (m_rng() & 1u) != 0u ? DeployableKind::FabTurret : DeployableKind::FabAmmoPack;
                m_ctx->deployables->ServerTryPlaceDeployable(bot.id, kind);
                ++m_chaosDeployTries;
            }
            else if (bot.cls == ClassId::Medtech)
            {
                m_ctx->deployables->ServerTryPlaceDeployable(bot.id, DeployableKind::MedBeacon);
                ++m_chaosDeployTries;
            }
        }

        // Vehicle purchase through the real validated terminal path (needs a
        // friendly terminal in reach + unlock + flux; usually refused — that IS
        // the exercise; success hands nearby bots wheels for TryUseVehicle).
        if (m_ctx->vehicles && (m_rng() % 3u) == 0u)
        {
            if (m_ctx->vehicles->ServerPurchaseVehicle(bot.id, static_cast<VehicleId>(1u + (m_rng() % 3u))))
                ++m_chaosVehiclePurchases;
            ++m_chaosVehicleTries;
        }
    }

    // ---------------------------------------------------------------------------
    // Chaos pilots (W9 bots-v2): deterministic purchase + drive/fly exercise
    // ---------------------------------------------------------------------------

    bool TFBotSystem::ChaosIsPilot(const Bot& bot) const
    {
        const uint32_t slot = bot.id - kTFBotIdBase;
        return slot == kChaosVulturePilotSlot || slot == kChaosDriverPilotSlot;
    }

    void TFBotSystem::SetFarObjective(Bot& bot, const float selfPos[3])
    {
        bot.objectiveRegion = kInvalidRegion;
        bot.objectiveX = bot.objectiveZ = 2048.0f; // map-center fallback
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        const RegionDef* best = nullptr;
        float bestD2 = -1.0f;
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier == "skyanchor")
                continue;
            const float dx = r.centerX - selfPos[0];
            const float dz = r.centerZ - selfPos[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 > bestD2)
            {
                bestD2 = d2;
                best = &r;
            }
        }
        if (!best)
            return;
        bot.objectiveRegion = best->id;
        bot.objectiveX = best->centerX;
        bot.objectiveZ = best->centerZ;
    }

    void TFBotSystem::ChaosPilotTryPurchase(Bot& bot, const PawnInfo& self, double now)
    {
        bot.pilotBuyAt = now + kPilotBuyRetrySec;
        if (!m_ctx->vehicles || m_ctx->vehicles->IsSeated(bot.id))
            return;

        // A free friendly ride already parked nearby (bought last try, or the
        // pilot walked back to it): just make the march long so TryUseVehicle
        // boards it on this same think.
        bool haveRide = false;
        m_ctx->vehicles->ForEachVehicle(
            [&](const TFVehicleInfo& v)
            {
                if (haveRide || v.faction != bot.faction || v.hp <= 0.0f || v.deployed)
                    return;
                if (v.seatCount == 0 || v.seats[0] != kInvalidPlayer)
                    return;
                const float dx = v.pos[0] - self.pos[0];
                const float dz = v.pos[2] - self.pos[2];
                haveRide = dx * dx + dz * dz < kVehScanRadiusM * kVehScanRadiusM;
            });
        if (haveRide)
        {
            SetFarObjective(bot, self.pos);
            bot.vehicleRetryAt = now; // board on this very think, not after cooldown
            return;
        }

        const uint32_t slot = bot.id - kTFBotIdBase;
        const VehicleId want = slot == kChaosVulturePilotSlot ? VehicleId::Vulture : VehicleId::Drifter;
        ++m_chaosVehicleTries;
        if (m_ctx->vehicles->ServerPurchaseVehicle(bot.id, want))
        {
            ++m_chaosVehiclePurchases;
            SetFarObjective(bot, self.pos);
            bot.vehicleRetryAt = now; // walk straight to the fresh hull
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos pilot 0x%08X bought vehicle %u -> far objective r%d",
                           bot.id, static_cast<unsigned>(want),
                           bot.objectiveRegion == kInvalidRegion ? -1 : static_cast<int>(bot.objectiveRegion));
        }
        else if (want == VehicleId::Vulture)
        {
            // Vulture refused (wallet drained by respawns / unlock rejected):
            // fall back to the ungated Drifter so the run still drives.
            ++m_chaosVehicleTries;
            if (m_ctx->vehicles->ServerPurchaseVehicle(bot.id, VehicleId::Drifter))
            {
                ++m_chaosVehiclePurchases;
                SetFarObjective(bot, self.pos);
                bot.vehicleRetryAt = now;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Fireteam & fitness
    // ---------------------------------------------------------------------------

    const TFBotSystem::Bot* TFBotSystem::FireteamLeader(const Bot& bot) const
    {
        if (!m_ctx || !m_ctx->players)
            return nullptr;
        for (const Bot& b : m_bots)
        {
            if (b.faction != bot.faction)
                continue;
            PawnInfo p;
            if (m_ctx->players->GetPawnByPlayer(b.id, p) && p.alive)
                return &b;
        }
        return nullptr;
    }

    float TFBotSystem::HealthFrac(const Bot& bot, const PawnInfo& self) const
    {
        float maxPool = 1000.0f; // ClassDef defaults (500 hp + 500 shield)
        if (m_ctx && m_ctx->data)
            if (const ClassDef* cd = m_ctx->data->GetClass(bot.cls))
                maxPool = std::max(1.0f, cd->health + cd->shield);
        return std::clamp((self.health + self.shield) / maxPool, 0.0f, 1.0f);
    }

    bool TFBotSystem::AcquireTarget(const Bot& bot, const PawnInfo& self, EntityId& outTarget,
                                    float outTargetPos[3]) const
    {
        if (!m_ctx->players)
            return false;

        struct Scan
        {
            const Bot* bot;
            const PawnInfo* self;
            EntityId best = 0;
            float bestD2 = kEngageRangeM * kEngageRangeM;
            float bestPos[3]{};
        } scan;
        scan.bot = &bot;
        scan.self = &self;

        // Hysteresis: an already-engaged target stays valid out to 70 m.
        const EntityId current = bot.targetEntity;

        m_ctx->players->ForEachAlivePawn(
            [&scan, current](const PawnInfo& p)
            {
                if (p.entity == scan.self->entity || !p.alive)
                    return;
                if (p.faction == scan.bot->faction || p.faction == FactionId::None)
                    return;
                const float dx = p.pos[0] - scan.self->pos[0];
                const float dy = p.pos[1] - scan.self->pos[1];
                const float dz = p.pos[2] - scan.self->pos[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (p.entity == current && d2 <= kDisengageRangeM * kDisengageRangeM)
                    d2 *= 0.25f; // sticky current target
                if (d2 < scan.bestD2)
                {
                    scan.bestD2 = d2;
                    scan.best = p.entity;
                    scan.bestPos[0] = p.pos[0];
                    scan.bestPos[1] = p.pos[1];
                    scan.bestPos[2] = p.pos[2];
                }
            });

        if (scan.best == 0)
            return false;

        const float eye[3] = {self.pos[0], self.pos[1] + kTFEyeHeightM, self.pos[2]};
        const float chest[3] = {scan.bestPos[0], scan.bestPos[1] + kChestHeightM, scan.bestPos[2]};
        if (!HasLineOfSight(eye, chest))
            return false;

        outTarget = scan.best;
        outTargetPos[0] = scan.bestPos[0];
        outTargetPos[1] = scan.bestPos[1];
        outTargetPos[2] = scan.bestPos[2];
        return true;
    }

    bool TFBotSystem::HasLineOfSight(const float eye[3], const float target[3]) const
    {
        if (!m_ctx->world)
            return true;
        float dir[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        const float dist = WeaponMath::Len3(dir);
        if (dist < kLosStepM || !WeaponMath::Normalize3(dir))
            return true;
        // Terrain march, same idea as TFWeaponSystem::TerrainBlocked (coarser step:
        // this runs per brain tick, not per shot).
        for (float t = kLosStepM; t < dist; t += kLosStepM)
        {
            const float x = eye[0] + dir[0] * t;
            const float y = eye[1] + dir[1] * t;
            const float z = eye[2] + dir[2] * t;
            if (y < m_ctx->world->TerrainHeightAt(x, z))
                return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // Objectives
    // ---------------------------------------------------------------------------

    void TFBotSystem::PickObjective(Bot& bot, const float selfPos[3]) const
    {
        // Default: map center keeps the war converging even with no data.
        float mapCenter = 2048.0f;
        bot.objectiveRegion = kInvalidRegion;

        if (!m_ctx->data || !m_ctx->data->IsLoaded())
        {
            bot.objectiveX = bot.objectiveZ = mapCenter;
            return;
        }
        const ContinentDef& cont = m_ctx->data->GetContinent();
        mapCenter = cont.sizeM * 0.5f;

        // Distance-weighted scoring: hot regions (contested, mid-capture, own
        // ground under attack) shrink their effective distance so nearby quiet
        // frontier only wins when nothing is burning. With no live region system
        // every weight is 1.0 -> identical to plain nearest-region marching.
        const RegionDef* best = nullptr;
        float bestScore = 1.0e30f;
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier == "skyanchor")
                continue;
            const FactionId owner = QueryRegionOwner(m_ctx->regions, r.id, FallbackOwner(cont, r));

            FactionId capturing = FactionId::None;
            bool contested = false;
            const float progress = QueryCaptureProgress(m_ctx->regions, r.id, capturing, contested);
            const bool underAttack = progress > 0.0f && capturing != FactionId::None && capturing != owner;

            float weight = 1.0f;
            if (owner == bot.faction)
            {
                if (!underAttack && !contested)
                    continue;   // quiet home ground — nothing to do there
                weight = 0.15f; // defend own regions under attack first
            }
            else
            {
                if (!QueryRegionCapturable(m_ctx->regions, r.id, bot.faction, /*fallback*/ true))
                    continue; // lattice rule once the region system answers
                if (contested)
                    weight = 0.25f; // join the fight on the point
                else if (capturing == bot.faction && progress > 0.0f)
                    weight = 0.20f; // finish captures we already started
                else if (underAttack)
                    weight = 0.50f; // third-party brawl worth crashing
            }

            const float dx = r.centerX - selfPos[0];
            const float dz = r.centerZ - selfPos[2];
            const float score = (dx * dx + dz * dz) * weight;
            if (score < bestScore)
            {
                bestScore = score;
                best = &r;
            }
        }

        if (!best)
        {
            bot.objectiveX = bot.objectiveZ = mapCenter;
            return;
        }

        bot.objectiveRegion = best->id;
        // Nearest capture point in the chosen region; region center as fallback.
        bot.objectiveX = best->centerX;
        bot.objectiveZ = best->centerZ;
        float bestCp2 = 1.0e30f;
        for (const auto& cp : best->capturePoints)
        {
            const float dx = cp[0] - selfPos[0];
            const float dz = cp[1] - selfPos[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestCp2)
            {
                bestCp2 = d2;
                bot.objectiveX = cp[0];
                bot.objectiveZ = cp[1];
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Combat (real TF_FireEvent -> ServerHandleFire, throttled to the weapon RoF)
    // ---------------------------------------------------------------------------

    void TFBotSystem::TryFire(Bot& bot, double now)
    {
        if (!m_ctx->players || !m_ctx->weapons || bot.weapon == kInvalidWeapon)
            return;

        // Mag/reload model mirroring the server's approximate mag in ValidateFire
        // (refills after reloadSec of silence) so bot shots are never rejected.
        if (bot.magLeft <= 0)
        {
            if (now < bot.reloadDoneAt)
                return;
            bot.magLeft = bot.magSize;
        }
        if (now < bot.nextFireAt)
            return;

        PawnInfo self, target;
        if (!m_ctx->players->GetPawnByPlayer(bot.id, self) || !self.alive)
            return;
        if (!m_ctx->players->GetPawnByEntity(bot.targetEntity, target) || !target.alive)
        {
            bot.targetEntity = 0;
            return;
        }

        float origin[3] = {self.pos[0], self.pos[1] + kTFEyeHeightM, self.pos[2]};
        float dir[3] = {target.pos[0] - origin[0], (target.pos[1] + kChestHeightM) - origin[1],
                        target.pos[2] - origin[2]};
        const float dist = WeaponMath::Len3(dir);
        if (dist > kDisengageRangeM || !WeaponMath::Normalize3(dir))
            return;
        WeaponMath::PerturbCone(dir, kAimErrorDeg, m_rng); // +-1.5 deg human error

        TF_FireEvent ev{};
        ev.seq = bot.seq;
        ev.weaponId = bot.weapon;
        ev.originX = origin[0];
        ev.originY = origin[1];
        ev.originZ = origin[2];
        ev.dirX = dir[0];
        ev.dirY = dir[1];
        ev.dirZ = dir[2];
        m_ctx->weapons->ServerHandleFire(bot.id, ev);

        ++m_shotsFired;
        bot.nextFireAt = now + bot.rofIntervalSec;
        if (--bot.magLeft <= 0)
            bot.reloadDoneAt = now + bot.reloadSec;
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    const char* TFBotSystem::StateName(BotState s)
    {
        switch (s)
        {
        case BotState::Deploying:
            return "deploying";
        case BotState::Moving:
            return "moving";
        case BotState::ToVehicle:
            return "toVehicle";
        case BotState::Driving:
            return "DRIVING";
        case BotState::Fighting:
            return "FIGHTING";
        case BotState::Dead:
            return "dead";
        }
        return "?";
    }

    std::string TFBotSystem::DebugSummary() const
    {
        std::ostringstream os;
        os << "[TF] bots " << m_bots.size() << "  spawnReqs " << m_spawnRequests << "  shots " << m_shotsFired
           << "  chaos " << (m_chaosActive ? "ON" : "off") << " (deployTries " << m_chaosDeployTries << ", vehTries "
           << m_chaosVehicleTries << ", vehBuys " << m_chaosVehiclePurchases << ")  abilityUses " << m_abilityUses
           << "  nav(blockedFeelers " << m_feelerBlocked << ", unsticks " << m_unsticks << ", stuckTeleports "
           << m_stuckTeleports << ")  now " << Now();
        for (const Bot& b : m_bots)
        {
            PawnInfo p{};
            const bool have = m_ctx && m_ctx->players && m_ctx->players->GetPawnByPlayer(b.id, p);
            os << "\n  p" << b.id << " " << FactionTag(b.faction) << " " << StateName(b.state)
               << " pawn=" << (have ? (p.alive ? "alive" : "dead") : "NONE");
            if (have)
                os << " pos(" << static_cast<int>(p.pos[0]) << "," << static_cast<int>(p.pos[1]) << ","
                   << static_cast<int>(p.pos[2]) << ") hp=" << static_cast<int>(p.health);
            os << " obj=r" << (b.objectiveRegion == kInvalidRegion ? -1 : static_cast<int>(b.objectiveRegion))
               << " tgt=" << b.targetEntity << " veh=" << b.vehicleEntity
               << " us=" << static_cast<unsigned>(b.unstickCount) << " nextSpawnTry=" << b.nextSpawnTryAt;
        }
        return os.str();
    }

    void TFBotSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Bots", &m_showDebug))
        {
            ImGui::Text("bots: %u / %u   shots fired: %u   spawn requests: %u", BotCount(), kTFMaxBots, m_shotsFired,
                        m_spawnRequests);
            ImGui::Text("nav: blocked feelers %u   unsticks %u   stuck teleports %u", m_feelerBlocked, m_unsticks,
                        m_stuckTeleports);
            ImGui::Separator();
            for (const Bot& b : m_bots)
            {
                const char* clsName = "?";
                if (m_ctx && m_ctx->data)
                    if (const ClassDef* cd = m_ctx->data->GetClass(b.cls))
                        clsName = cd->name.c_str();
                ImGui::Text("0x%08X %s %-10s %-9s obj=r%u (%.0f,%.0f) tgt=%u veh=%u mag=%d%s", b.id,
                            FactionTag(b.faction), clsName, StateName(b.state),
                            b.objectiveRegion == kInvalidRegion ? 0u : static_cast<unsigned>(b.objectiveRegion),
                            b.objectiveX, b.objectiveZ, b.targetEntity, b.vehicleEntity, b.magLeft,
                            b.lowHealth ? " LOW" : "");
            }
        }
        ImGui::End();
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
