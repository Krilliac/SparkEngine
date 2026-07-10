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
#include "Game/TFMovementModel.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponMath.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFServerSim.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <concepts>
#include <numbers>
#include <sstream>

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
        constexpr float kVehStuckSec = 3.0f;      // wedged ride pose this long -> bail out
        constexpr double kVehRetrySec = 10.0;     // cooldown after a failed/abandoned plan
        constexpr uint8_t kVehMaxEnterTries = 3;

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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFBotSystem initialized");
        return true;
    }

    void TFBotSystem::Shutdown()
    {
        if (!m_initialized)
            return;
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
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
            bot.jumping = false;
        }

        ThinkAlive(bot, self, now);
    }

    void TFBotSystem::ThinkAlive(Bot& bot, const PawnInfo& self, double now)
    {
        // A seated bot rides the vehicle: infantry combat, stuck-jump and walking
        // objectives do not apply until it dismounts.
        if (bot.state == BotState::Driving)
        {
            ThinkDriving(bot, self, now);
            return;
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

            // --- objective (region march, contested-biased) ---
            PickObjective(bot, self.pos);

            // --- fireteam cohesion: followers adopt the leader's objective and
            //     regroup on the leader when strung out ---
            if (const Bot* leader = FireteamLeader(bot); leader && leader != &bot)
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
                if (dist > kSprintBeyondM)
                    in.buttons |= TFB_Sprint;
            }
        }

        if (bot.jumping)
            in.buttons |= TFB_Jump;

        bot.input = in; // seq is stamped per fixed tick in FixedUpdate
        bot.wantMove = true;
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

        // Wedged ride: pose unchanged too long -> dismount and walk instead.
        const float dsx = self.pos[0] - bot.stuckRefPos[0];
        const float dsz = self.pos[2] - bot.stuckRefPos[2];
        if (dsx * dsx + dsz * dsz > kStuckEpsM * kStuckEpsM)
        {
            bot.stuckRefPos[0] = self.pos[0];
            bot.stuckRefPos[1] = self.pos[1];
            bot.stuckRefPos[2] = self.pos[2];
            bot.stuckSince = now;
        }
        else if (now - bot.stuckSince > kVehStuckSec)
        {
            ExitVehicle(bot, now);
            return;
        }

        // Keep the objective fresh while riding (ownership can flip mid-drive).
        PickObjective(bot, self.pos);
        const float dx = bot.objectiveX - self.pos[0];
        const float dz = bot.objectiveZ - self.pos[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < kVehExitWithinM)
        {
            ExitVehicle(bot, now);
            return;
        }

        // Drive: moveY = throttle, moveX = steer (ServerHandleSeatedInput mapping).
        const float desiredYaw = std::atan2(dx, dz);
        const float yawErr = WrapPi(desiredYaw - veh.yaw);
        const float steer = std::clamp(yawErr * 1.5f, -1.0f, 1.0f);

        TF_ClientInput in{};
        in.weaponSlot = 0;
        in.viewYaw = desiredYaw;
        in.viewPitch = 0.0f;
        in.moveX = static_cast<int8_t>(steer * 127.0f);
        in.moveY = static_cast<int8_t>(std::fabs(yawErr) > 1.5f ? 60 : 127); // ease off in hard turns

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
           << "  now " << Now();
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
               << " tgt=" << b.targetEntity << " veh=" << b.vehicleEntity << " nextSpawnTry=" << b.nextSpawnTryAt;
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
