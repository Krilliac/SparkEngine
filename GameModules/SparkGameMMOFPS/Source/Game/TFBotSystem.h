/**
 * @file TFBotSystem.h
 * @brief Server-side AI combatants — the W2 validation workhorse that makes
 *        the territory war visible without 64 humans.
 *
 * OWNERSHIP: this header + TFBotSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (wired from Main.cpp by
 * the orchestrator) — extend this class freely, but do not change the
 * lifecycle signatures.
 *
 * Bots exercise the REAL game paths, never teleport hacks:
 *  - spawn:   TFServerSim::SetPlayerFaction + TFPlayerSystem::
 *             ServerHandleFactionSelect -> ServerHandleSpawnRequest(skyanchor).
 *             (The TF_SpawnReply for a bot id is a safe no-op: NetworkManager::
 *             SendToClient drops sends to ids with no client address, and the
 *             Standalone role skips the send entirely.)
 *  - move:    TF_ClientInput built by a 5 Hz brain, enqueued into
 *             TFServerSim::EnqueueInput every fixed tick (same consume path
 *             as human clients).
 *  - fight:   TF_FireEvent -> TFWeaponSystem::ServerHandleFire at the
 *             weapon's real rate of fire, with a +-1.5 deg aim error.
 *  - respawn: 8 s after death, back through the spawn request pipeline.
 *  - drive:   TFVehicleSystem::ServerHandleSeatOp into the driver seat of a
 *             nearby friendly vehicle when the objective is far; the normal
 *             TF_ClientInput enqueue then steers it (TFServerSim forwards a
 *             seated player's inputs to ServerHandleSeatedInput). W9 bots-v2:
 *             terrain look-ahead steering around plateau walls, a reverse-out
 *             recovery phase before giving up on a wedged ride, and Vulture
 *             VTOL flight (altitude hold on the Jump/Crouch lift axis, fly at
 *             the objective, descend + landed-gated exit on arrival).
 *  - ability: (W9 bots-v2) situational class-ability triggers — Medtech near
 *             hurt friendlies, Bulwark under fire, Striker closing a gap.
 *             Bots hold TFB_Ability on their input AND, when the
 *             class-abilities lane's TFAbilitySystem is compiled in and
 *             published on TFGameContext, call the public CanUseAbility/
 *             UseAbility seam (detected at compile time, same shim pattern as
 *             the TFRegionSystem queries — absent system == silent no-op).
 *  - avoid:   (W12 bot-navigation) local obstacle avoidance while walking —
 *             short chest-height feeler rays (RaycastFiltered, WorldStatic)
 *             steer marching bots around structure OBBs (the W11 watchtower
 *             leg cages) instead of nosing in; a coarse no-progress detector
 *             breaks residual stalls with a random 90-150 deg unstick leg,
 *             and persistent stalls fall back to the chaos teleport-scatter
 *             (chaos mode only). tf_botinfo carries the counters.
 *
 * Tactics (all server-side, deterministic given the same tick sequence):
 *  - objective bias toward contested / actively-capturing regions and toward
 *    defending own regions under attack (TFRegionSystem CaptureProgress);
 *  - fireteam cohesion: the first alive bot of a faction is the implicit
 *    leader, the rest adopt its objective and regroup when strung out;
 *  - engage-vs-advance by health/ammo: healthy + loaded bots push, hurt or
 *    reloading bots give ground while keeping the target in their sights.
 *
 * Authority-only: inert on pure clients (and it despawns everything if the
 * process stops being the authority). Cap 32 bots, flat storage, no
 * allocations in the fixed-tick path.
 */
#pragma once

#include "Core/TFEvents.h"
#include "Core/TFTypes.h"
#include "Net/TFNetProtocol.h"

#include <memory>
#include <random>
#include <string>
#include <vector>

namespace Terrafront
{

    struct PawnInfo;      // defined in Game/TFPlayerSystem.h (frozen W1 contract)
    class TFChaosHarness; // Game/TFChaosHarness.h (bots-chaos lane validation)

    /// Bot player ids live in their own range, distinct from real network client
    /// ids (small integers from NetworkManager) and kTFLocalHostPlayer (0xFFFFFF01).
    constexpr PlayerId kTFBotIdBase = 0xB0700000u;
    constexpr uint32_t kTFMaxBots = 32;

    class TFBotSystem
    {
      public:
        TFBotSystem();
        ~TFBotSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- W2 cross-agent contract -------------------------------------------
        /// Spawn/despawn bots until exactly `n` exist (clamped to kTFMaxBots).
        /// Authority only; a warning is logged and nothing happens otherwise.
        void ServerSetBotCount(uint32_t n);
        uint32_t BotCount() const { return static_cast<uint32_t>(m_bots.size()); }

        /// Debug panel toggle (hidden by default; for the tf_* console commands).
        void ToggleDebugUI() { m_showDebug = !m_showDebug; }

        // --- chaos exercise mode (bots-chaos lane, 2026-07-10) -------------------
        /// tf_chaos entry point: spawn `botCount` bots (round-robin factions) and
        /// switch them into deliberately chaotic exercise mode for `seconds`
        /// (0 = until tf_chaos 0 / bot despawn): teleport-scatter onto capture
        /// points for coverage (even slots into one shared multi-faction "arena"
        /// region so kills + contests are guaranteed; odd slots onto a random
        /// lattice-capturable region so capture progress is guaranteed),
        /// randomized objective re-rolls, wide strafe weaving through the
        /// collision world, periodic deployable placements (class-gated) and
        /// vehicle-purchase attempts (server-validated; refusals are free
        /// exercise). Authority only. `botCount` 0 stops chaos and despawns.
        /// W9 bots-v2: two designated pilot slots scatter to their own faction
        /// skyanchor terminal instead — slot 1 is bankrolled/ranked through the
        /// REAL progression paths (ServerGrantFlux/ServerAwardXP/ServerTryUnlock)
        /// and pulls a Vulture to fly at a far objective; slot 2 buys a Drifter
        /// and drives there — so the tf_validate vehicles check has a
        /// deterministic purchase + >20 m movement to observe.
        void ServerStartChaos(uint32_t botCount, float seconds);
        bool ChaosActive() const { return m_chaosActive; }

        /// Validation report for the tf_validate console command (delegates to
        /// the harness; also mirrored into the engine log line by line).
        std::string ValidationReport();

        /// One-line-per-bot diagnostic dump for the tf_botinfo console command
        /// (state, pawn liveness, position, objective, spawn/respawn scheduling).
        std::string DebugSummary() const;

      private:
        enum class BotState : uint8_t
        {
            Deploying,
            Moving,
            ToVehicle,
            Driving,
            Fighting,
            Dead
        };
        static const char* StateName(BotState s);

        struct Bot
        {
            PlayerId id = kInvalidPlayer;
            FactionId faction = FactionId::None;
            ClassId cls = ClassId::Ghost;
            BotState state = BotState::Deploying;
            uint32_t seq = 0; ///< monotonic TF_ClientInput sequence

            TF_ClientInput input{}; ///< rebuilt by Think(); enqueued per fixed tick
            bool wantMove = false;  ///< pawn alive and input valid

            // objective (region march)
            RegionId objectiveRegion = kInvalidRegion;
            float objectiveX = 0.0f, objectiveZ = 0.0f;

            // vehicle use (driver seat 0 only)
            EntityId vehicleEntity = 0;  ///< approach / ride target (0 = none)
            double vehicleRetryAt = 0.0; ///< no vehicle scan before this time
            uint8_t enterTries = 0;      ///< failed seat-op attempts this approach

            // driving v2 (W9 bots-v2): wedge recovery instead of instant dismount
            double reverseUntil = 0.0; ///< reverse-out phase deadline (0 = off)
            int8_t reverseSteer = 0;   ///< steer held while reversing out
            uint8_t wedgeCount = 0;    ///< wedges this ride; 2nd inside window -> walk
            double lastWedgeAt = 0.0;

            // class ability (W9 bots-v2; silent no-op until the abilities lane lands)
            double nextAbilityAt = 0.0; ///< situational trigger rate limit
            float lastPool = -1.0f;     ///< health+shield last think (-1 = unknown)
            bool underFire = false;     ///< pool dropped since the previous think

            // chaos pilot (W9 bots-v2): purchase retry schedule for pilot slots
            double pilotBuyAt = 0.0;

            // fitness cache (drives engage-vs-advance; refreshed each Think)
            bool lowHealth = false;

            // combat
            EntityId targetEntity = 0; ///< 0 == no target
            WeaponId weapon = kInvalidWeapon;
            float rofIntervalSec = 0.1f;
            int magSize = 30;
            float reloadSec = 2.5f;
            int magLeft = 30;
            double nextFireAt = 0.0;
            double reloadDoneAt = 0.0;

            // scheduling
            double nextThinkAt = 0.0; ///< staggered 5 Hz brain
            double nextSpawnTryAt = 0.0;

            // stuck detection (position unchanged > 2 s -> jump)
            float stuckRefPos[3]{0.0f, 0.0f, 0.0f};
            double stuckSince = 0.0;
            bool jumping = false;
            float strafePhase = 0.0f;

            // local obstacle avoidance (W12 bot-navigation)
            uint8_t feelerPhase = 0;      ///< stagger: full feeler pattern 1-in-4 thinks
            int8_t lastAvoidSide = 0;     ///< last steer side (+1 right / -1 left)
            float lastBlockedYaw = 0.0f;  ///< path memory: heading that hit a wall
            double blockedYawUntil = 0.0; ///< memory expiry (bias away while fresh)
            float backTurnYaw = 0.0f;     ///< heading held while backing off a pocket
            double backTurnUntil = 0.0;   ///< both-feelers-blocked back-turn deadline

            // unstick (W12): coarse no-progress detector while trying to walk
            float moveRefPos[2]{0.0f, 0.0f}; ///< XZ progress reference
            double moveRefAt = 0.0;          ///< when the reference was taken
            float unstickYaw = 0.0f;         ///< random 90-150 deg escape heading
            double unstickUntil = 0.0;       ///< escape leg deadline (0 = off)
            uint8_t unstickCount = 0;        ///< consecutive unsticks; 3rd -> chaos scatter

            // chaos exercise mode (bots-chaos lane)
            bool chaosScatterPending = false; ///< teleport-scatter on next alive think
            double chaosRerollAt = 0.0;       ///< next randomized objective re-roll
            double chaosUtilityAt = 0.0;      ///< next deployable/vehicle-purchase attempt
        };

        double Now() const;
        void SpawnBotSlot(uint32_t slot);
        void DespawnBot(Bot& bot);
        void ResolveLoadout(Bot& bot) const;
        void TrySpawn(Bot& bot, double now);
        void Think(Bot& bot, double now);
        void ThinkAlive(Bot& bot, const PawnInfo& self, double now);
        void ThinkDriving(Bot& bot, const PawnInfo& self, double now);
        /// Continue / start a driver-seat approach. Fills `in` and returns true
        /// while the vehicle plan owns this think's movement.
        bool TryUseVehicle(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in);
        void ExitVehicle(Bot& bot, double now);
        // --- W9 bots-v2 -----------------------------------------------------
        /// Situational class-ability trigger (Medtech near hurt friendlies,
        /// Bulwark under fire, Striker closing a gap). Holds TFB_Ability on
        /// `in` and calls the CanUseAbility/UseAbility seam when compiled in.
        void TryClassAbility(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in);
        /// True for the chaos pilot slots (Vulture pilot / Drifter driver).
        bool ChaosIsPilot(const Bot& bot) const;
        /// Pilot purchase: at the faction terminal with no free ride nearby,
        /// buy the slot's vehicle (Vulture falls back to Drifter when refused)
        /// and aim the bot at a far region so TryUseVehicle boards + goes.
        void ChaosPilotTryPurchase(Bot& bot, const PawnInfo& self, double now);
        /// Objective = the farthest non-skyanchor region (long ride target).
        void SetFarObjective(Bot& bot, const float selfPos[3]);
        // ---------------------------------------------------------------------
        // --- W12 bot-navigation: local obstacle avoidance -------------------
        /// Walking-leg avoidance: no-progress unstick detector + feeler
        /// steering, applied to the movement input after the march/approach
        /// heading is chosen (Moving/ToVehicle states only).
        void ApplyAvoidance(Bot& bot, const PawnInfo& self, double now, TF_ClientInput& in);
        /// Feeler steering: forward + ±30 deg chest-height rays against
        /// WorldStatic. Returns the (possibly detoured) heading to walk.
        float SteerFeelers(Bot& bot, const PawnInfo& self, double now, float desiredYaw);
        /// Clear distance along one feeler (kFeelerLenM when nothing hit or
        /// the physics world is not live).
        float FeelerClearance(const float origin[3], float yaw) const;
        /// Reset the avoidance/unstick reference state (fresh life, teleport).
        static void ResetAvoidance(Bot& bot, float x, float z, double now);
        // ---------------------------------------------------------------------
        void TryFire(Bot& bot, double now);
        bool AcquireTarget(const Bot& bot, const PawnInfo& self, EntityId& outTarget, float outTargetPos[3]) const;
        void PickObjective(Bot& bot, const float selfPos[3]) const;
        /// First alive same-faction bot in slot order (may be `bot` itself);
        /// nullptr when none is alive. The implicit fireteam leader.
        const Bot* FireteamLeader(const Bot& bot) const;
        float HealthFrac(const Bot& bot, const PawnInfo& self) const;
        bool HasLineOfSight(const float eye[3], const float target[3]) const;

        // chaos exercise mode (bots-chaos lane)
        /// Teleport-scatter `bot` for coverage: even slots to the shared arena
        /// region (multi-faction brawl), odd slots to a random region capturable
        /// by the bot's faction; drop point = a capture point + random offset.
        void ChaosScatter(Bot& bot, double now);
        /// Randomized objective override: ~50% keep the scored objective, else a
        /// random non-skyanchor region (coverage over optimality) every 6-16 s.
        void ChaosMaybeReroll(Bot& bot, double now);
        /// Periodic deployable placement (class-gated) + vehicle-purchase
        /// attempts through the real validated server entry points.
        void ChaosTryUtility(Bot& bot, double now);
        /// Region index for the shared brawl arena (first "facility", else the
        /// first non-skyanchor region); kInvalidRegion when data is not loaded.
        RegionId ChaosArenaRegion() const;

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        std::vector<Bot> m_bots;       ///< flat, capped at kTFMaxBots
        double m_clock{0.0};           ///< fallback time base (no serverSim)
        std::mt19937 m_rng{0xB07B07u}; ///< deterministic bot randomness

        // chaos exercise mode (bots-chaos lane)
        std::unique_ptr<TFChaosHarness> m_chaos; ///< validation counters + report
        bool m_chaosActive{false};
        double m_chaosEndsAt{0.0}; ///< 0 = open-ended
        bool m_chaosCmds{false};   ///< tf_chaos/tf_validate registered by this instance
        uint32_t m_chaosDeployTries{0};
        uint32_t m_chaosVehicleTries{0};
        uint32_t m_chaosVehiclePurchases{0}; ///< W9: successful bot purchases this run
        uint32_t m_abilityUses{0};           ///< W9: successful seam activations (lifetime)

        // debug counters
        uint32_t m_shotsFired{0};
        uint32_t m_spawnRequests{0};
        // W12 bot-navigation instrumentation (tf_botinfo)
        uint32_t m_feelerBlocked{0};  ///< feeler rays that hit WorldStatic
        uint32_t m_unsticks{0};       ///< no-progress escape legs started
        uint32_t m_stuckTeleports{0}; ///< persistent-stuck chaos scatters
        bool m_showDebug{false};
    };

} // namespace Terrafront
