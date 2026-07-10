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
 *             seated player's inputs to ServerHandleSeatedInput).
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

#include <random>
#include <string>
#include <vector>

namespace Terrafront
{

    struct PawnInfo; // defined in Game/TFPlayerSystem.h (frozen W1 contract)

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
        void TryFire(Bot& bot, double now);
        bool AcquireTarget(const Bot& bot, const PawnInfo& self, EntityId& outTarget, float outTargetPos[3]) const;
        void PickObjective(Bot& bot, const float selfPos[3]) const;
        /// First alive same-faction bot in slot order (may be `bot` itself);
        /// nullptr when none is alive. The implicit fireteam leader.
        const Bot* FireteamLeader(const Bot& bot) const;
        float HealthFrac(const Bot& bot, const PawnInfo& self) const;
        bool HasLineOfSight(const float eye[3], const float target[3]) const;

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        std::vector<Bot> m_bots;       ///< flat, capped at kTFMaxBots
        double m_clock{0.0};           ///< fallback time base (no serverSim)
        std::mt19937 m_rng{0xB07B07u}; ///< deterministic bot randomness

        // debug counters
        uint32_t m_shotsFired{0};
        uint32_t m_spawnRequests{0};
        bool m_showDebug{false};
    };

} // namespace Terrafront
