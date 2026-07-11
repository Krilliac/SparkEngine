/**
 * @file TFFootsteps.h
 * @brief Client-side footstep audio for the local pawn and nearby remote
 *        pawns (footstep_sand_01..04 / footstep_metal_01..04).
 *
 * OWNERSHIP: audio-wave-2 lane (W10). New system, uniform TF lifecycle
 * (Initialize/Update/FixedUpdate/Shutdown/RenderDebugUI), wired from Main.cpp
 * by the integrator (see the wave wiringNotes). Presentation only: reads the
 * shared PawnInfo registry, never writes any sim state — the determinism
 * contract is untouched. Inert on dedicated servers (HasLocalPlayer false)
 * and headless runs (GetAudio null).
 *
 * ## How steps are triggered
 * Per alive pawn (the LOCAL pawn always; remote pawns within kHearRangeM of
 * the listener) the system accumulates HORIZONTAL distance actually moved
 * between frames (position deltas — robust for remote pawns, whose velocity
 * is only estimated from 20 Hz replication) and plays one step every ~2.2 m
 * (walk) / ~2.8 m (sprint, horizontal speed above kSprintSpeedMps) while the
 * pawn is grounded. Round-robin through the 4 variants per surface with a
 * small pitch jitter; remote-step volume falls off linearly with distance.
 *
 * ## Grounded + surface heuristics (honest limitations)
 * TFWorldSetup's ResolveMoveCollision grounded out-param is consumed inside
 * the movement integrators (TFServerSim / TFClientNet prediction — both
 * CONTENDED files) and is not exposed per-pawn, so this system derives
 * grounded from what PawnInfo replicates everywhere:
 *   grounded  = feet within 0.10 m of TerrainHeightAt  OR  |vel.y| < 0.15 m/s
 * (resting pawns have vel.y == 0 — TFMoveStep step 7 / the collision-v2
 * landing both zero it; a jump apex can alias as grounded for ~1 frame,
 * which adds centimeters of accumulation and never a spurious step).
 * Surface pick is equally cheap: METAL inside the sanctuary (its pad/floors)
 * or when a grounded pawn's feet sit well ABOVE the analytic terrain (i.e.
 * supported by a static scene body top — collision-v2 landing); SAND
 * everywhere else. No per-material scene data exists yet, so a pawn on a
 * low mesa ramp still reads as sand — noted, acceptable for W10.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

class AudioEngine;

namespace Terrafront
{

    struct PawnInfo; // Game/TFPlayerSystem.h (TFReplication.h precedent)

    class TFFootsteps
    {
      public:
        TFFootsteps();
        ~TFFootsteps();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

      private:
        /// Per-pawn stride accumulator (keyed by network entity id).
        struct Track
        {
            bool havePrev = false;
            float prevPos[3] = {0.0f, 0.0f, 0.0f};
            float accum = 0.0f;    ///< grounded horizontal meters since the last step
            double lastSeen = 0.0; ///< m_clock stamp for the stale sweep
        };

        ::AudioEngine* Audio() const;
        bool ListenerPos(float out[3]) const;
        void UpdatePawn(::AudioEngine& audio, const PawnInfo& pawn, bool isLocal, float distToListener);
        void PlayStep(::AudioEngine& audio, const PawnInfo& pawn, bool isLocal, bool sprinting, float distToListener);
        void EnsureLoaded(::AudioEngine& audio, const std::string& assetPath);
        void PruneStale();

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        double m_clock{0.0};
        float m_pruneAccum{0.0f};
        std::unordered_map<EntityId, Track> m_tracks;
        std::unordered_set<std::string> m_loaded; ///< LoadSound already issued
        uint32_t m_sandIdx{0};                    ///< round-robin cursor, sand variants
        uint32_t m_metalIdx{0};                   ///< round-robin cursor, metal variants
        std::mt19937 m_rng{0xF007u};

        // debug
        uint32_t m_stepsPlayed{0};
        bool m_lastMetal{false};
    };

} // namespace Terrafront
