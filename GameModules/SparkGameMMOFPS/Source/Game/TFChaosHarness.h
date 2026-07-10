/**
 * @file TFChaosHarness.h
 * @brief Chaos-run validation counters + the tf_validate PASS/FAIL report.
 *
 * OWNERSHIP: bots-chaos lane (2026-07-10 fleet). Owned and driven by
 * TFBotSystem (constructed in its Initialize, sampled from its Update while a
 * chaos run is active, reported by the tf_validate console command).
 *
 * The harness is the fleet's acceptance gate: after `tf_chaos <n> [sec]` has
 * run on a dedicated server, `tf_validate` prints machine-greppable
 * `[TF-VALIDATE]` PASS/FAIL lines proving the live systems actually worked:
 *
 *   physics    Jolt world is live (PhysicsSystem::GetJoltSystem() != null —
 *              the no-Jolt stub hands out valid dummy bodies, so this probe is
 *              the only trustworthy signal; same probe as TFWorldCollision).
 *   collision  static scene bodies were built (TFWorldCollision::BodyCount()>0)
 *              AND at least one pawn move was blocked/slid against them this
 *              run (TFWorldCollision::BlockedMoveCount() delta since chaos
 *              start) — i.e. bots path AROUND obstacles, not through them.
 *   capture    territory progress advanced somewhere: max CaptureProgress
 *              observed across all regions during 1 Hz sampling > 0, or a
 *              region ownership flip happened (EvRegionCaptured).
 *   kills      at least one EvPlayerKilled fired since chaos start.
 *   terrain    no alive pawn was ever sampled meaningfully below
 *              TFWorldSetup::TerrainHeightAt at its own XZ (worst observed
 *              depth across all 1 Hz samples + a fresh sample at report time).
 *
 * Counters reset on every OnChaosStart so each run is judged on its own.
 * Everything here is read-only w.r.t. gameplay: the harness never mutates
 * simulation state.
 */
#pragma once

#include "Core/TFEvents.h"
#include "Core/TFTypes.h"

#include <cstdint>
#include <string>

namespace Terrafront
{

    class TFChaosHarness
    {
      public:
        /// Subscribe the kill / region-flip observers. Call once, from
        /// TFBotSystem::Initialize (the bus outlives the harness's use; the
        /// module tears both down together, matching every other subscriber).
        bool Initialize(TFGameContext& ctx, TFEventBus& events);

        /// Reset all counters and capture the blocked-move baseline. Called by
        /// TFBotSystem::ServerStartChaos at the start of every chaos run.
        void OnChaosStart(uint32_t botCount, float seconds);

        /// ~1 Hz sampling while a chaos run is active (region capture progress,
        /// pawns-below-terrain). Cheap; call freely from TFBotSystem::Update.
        void Sample(double now);

        /// Build the [TF-VALIDATE] report (also mirrors every line into the
        /// engine log via SPARK_LOG_INFO so `grep [TF-VALIDATE] server.log`
        /// works even when the console echo is lost). Safe on any role/time —
        /// checks that never ran simply FAIL with zeros.
        std::string BuildReport();

        bool Armed() const { return m_armed; }

      private:
        uint64_t BlockedMovesNow() const;
        /// One pass over the alive pawns: how many are currently more than
        /// kBelowTolM under the terrain, and the deepest violation in meters.
        void CountBelowTerrain(uint32_t& outCount, float& outWorstDepthM) const;

        TFGameContext* m_ctx{nullptr};
        bool m_armed{false}; ///< a chaos run has started (counters are live)

        // run counters (reset by OnChaosStart)
        uint32_t m_kills{0};
        uint32_t m_regionFlips{0};
        float m_maxCaptureProgress{0.0f};
        uint64_t m_blockedBaseline{0};
        uint32_t m_belowTerrainEver{0}; ///< pawns seen below terrain across all samples
        float m_worstBelowM{0.0f};      ///< deepest below-terrain violation seen
        uint32_t m_samples{0};
        double m_nextSampleAt{0.0};
    };

} // namespace Terrafront
