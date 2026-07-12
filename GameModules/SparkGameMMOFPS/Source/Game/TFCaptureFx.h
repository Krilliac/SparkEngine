/**
 * @file TFCaptureFx.h
 * @brief Client-side "capturing feels alive" presentation for region capture
 *        points (W13 capture-fx lane): a per-tower progress beam, a one-shot
 *        ownership-flip burst, and a standing-in-point ground ring under the
 *        local player.
 *
 * Pure client presentation, driven ENTIRELY off state the client already
 * mirrors — TFRegionSystem's public CaptureProgress/IsCapturable accessors
 * (server truth on authority roles, the TF_RegionState/TF_CaptureTick mirror
 * on pure clients — the SAME accessors the HUD capture feed and
 * TFAudioAmbience's capture-alarm loop already read) plus the RegionDef
 * capturePoints list. No new TFMsg wire traffic, no TFGameContext/Main.cpp
 * changes. Rendering routes through the existing W8 TFTransparentPass
 * (Flush already runs every frame from TFWorldSetup::RenderWorld, so this
 * file needs no render-pass wiring of its own) and audio through
 * AudioEngine::PlaySound3D (Game/TFUiSounds.h lazy-load precedent).
 *
 * Justified singleton: same rationale as TFGroundFx/TFImpactFx/TFPingSystem
 * — pure client presentation with a single producer
 * (TFRegionSystem::UpdateCaptureVisuals, same lane) — a static accessor
 * keeps that (contended) file's diff to a handful of call-site lines with NO
 * changes to TFRegionSystem.h's class layout. Module DLL = one image, so the
 * per-image static is THE instance (per-image-DLL-statics gotcha respected).
 *
 * Budget: kMaxParticles (48) + kMaxFlares (8) pooled, process-wide, shared
 * by every region's owner-flip burst; a full pool silently drops new spawns
 * rather than growing (TFImpactFx-style hard cap). The per-frame tower beams
 * and the single standing ring add at most a couple of TFTransparentPass
 * items per region/frame — well inside its own 256-item/frame cap.
 *
 * OWNERSHIP: capture-fx lane (this header + TFCaptureFx.cpp), driven from
 * World/TFRegionSystem.cpp (same lane, capture-FX section of
 * UpdateCaptureVisuals).
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstddef>
#include <random>
#include <vector>

namespace Terrafront
{

    class TFCaptureFx
    {
      public:
        /// Process-wide instance. See file header for the singleton rationale.
        static TFCaptureFx& Get();

        /// Advance every pooled one-shot burst particle/flare and (re)submit
        /// the still-live ones to TFTransparentPass for this frame. Call once
        /// per frame from TFRegionSystem::UpdateCaptureVisuals (any position —
        /// pure pool bookkeeping, independent of the per-region loop below).
        /// Safe no-op when headless (no GraphicsEngine yet).
        void Update(TFGameContext& ctx, float dt);

        /// Progress beam at one capture tower: a faction-tinted vertical light
        /// column whose height tracks `progress`; contested pulses red at a
        /// minimum visible height regardless of `progress`. Submits nothing at
        /// an idle, uncontested, 0-progress point — quiet towers stay quiet.
        /// Call once per non-skyanchor region per frame (region-fx section of
        /// UpdateCaptureVisuals's per-frame retint loop).
        void SubmitTowerBeam(TFGameContext& ctx, const float towerBasePos[3], float progress, FactionId capturing,
                             bool contested);

        /// One-shot ownership-flip celebration at a tower: a pooled particle
        /// burst + a rising faction-colored flare + a capture_alarm sting
        /// (PlaySound3D at the tower). Call EXACTLY ONCE per real flip, never
        /// on the initial owner-paint at spawn — the caller (UpdateCaptureVisuals)
        /// guards the first-paint case before calling this.
        void SpawnOwnerFlipBurst(TFGameContext& ctx, const float towerBasePos[3], FactionId newOwner);

        /// Standing-in-point feedback: does its own local-pawn + capturable-
        /// point lookup (TFRegionSystem::FeedLocalCaptureHUD /
        /// TFAudioAmbience::LocalOnActiveCapturePoint precedent, rebuilt from
        /// PUBLIC TFRegionSystem accessors only) and submits a faction-tinted
        /// ground ring under the player's feet, filling with progress. No-op
        /// off any capturable point, headless, or without a local player. Call
        /// once per frame from UpdateCaptureVisuals.
        void UpdateStandingRing(TFGameContext& ctx);

      private:
        TFCaptureFx() = default;

        /// One pooled owner-flip burst spark (world space).
        struct BurstParticle
        {
            float pos[3]{};
            float vel[3]{};
            float age = 0.0f, life = 0.5f;
            float size0 = 0.30f, size1 = 0.08f; ///< billboard edge at birth/death (shrinks as it fades)
            float color[3]{1.0f, 1.0f, 1.0f};
        };

        /// One pooled rising ownership flare (world space).
        struct Flare
        {
            float pos[3]{};
            float age = 0.0f, life = 1.1f;
            float color[3]{1.0f, 1.0f, 1.0f};
        };

        float Rand01();

        std::vector<BurstParticle> m_particles;
        std::vector<Flare> m_flares;
        float m_pulseClock = 0.0f; ///< shared clock for the contested-pulse red alarm
        std::minstd_rand m_rng{0xCAF7Eu};

        static constexpr std::size_t kMaxParticles = 48; ///< hard process-wide pool cap
        static constexpr std::size_t kMaxFlares = 8;
    };

} // namespace Terrafront
