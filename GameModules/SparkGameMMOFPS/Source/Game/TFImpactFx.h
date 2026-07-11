/**
 * @file TFImpactFx.h
 * @brief Client-side bullet-impact feedback: short-lived camera-facing
 *        flipbook quads at shot impact points, surface-flavored.
 *
 * Pure client presentation — no networking, no wire changes, no server state
 * (W10 impact-fx lane). Two producers feed one pooled emitter:
 *
 *  - LOCAL SHOOTER (OnLocalShot, hooked after TFWorldSetup::SpawnMuzzleFx in
 *    TFWeaponSystem::ClientTriggerFire): the impact POSITION is client-
 *    predicted from the same origin+dir the TF_FireEvent carries (dir already
 *    spread-perturbed, so tracer and impact agree visually). The server's
 *    TF_HitConfirm hitmarker carries no position (Net/TFNetProtocol.h,
 *    8 bytes), so the burst is predictive, mirroring how the muzzle
 *    flash/tracer are already client-side-only.
 *  - REMOTE FIRE (OnRemoteShot, hooked after SpawnMuzzleFx in
 *    TFAudioAmbience::ClientOnRemoteFire — the 0x54F4 TF_RemoteFireFx client
 *    seam, which carries pos + dir): a puff at the remote tracer's visual
 *    endpoint. Gated to shots whose muzzle is within kRemoteCamGateM of the
 *    camera, ray budget kTraceBudgetM, WorldStatic+Vehicle mask only.
 *
 * Surface flavor: terrain / static-world hits spawn a dirt puff (hover_dust
 * sheet, ALPHA blend — reads better than additive against bright sand);
 * pawn / vehicle hits spawn a smaller bright spark (muzzle_flash_common
 * sheet, ADDITIVE). Both are 4-frame flipbook quads, camera-facing,
 * depth READ-ONLY, ~0.15 s.
 *
 * Impact point resolution (TraceImpact) is layered exactly like the server's
 * hitscan fallbacks, but purely visual: live-Jolt RaycastFiltered
 * (GetJoltSystem() probe — the stub hands out dummy bodies), analytic
 * TerrainHeightAt march (1 m steps + bisection), then client-mirror pawn
 * capsules (WeaponMath::SegmentHitsCapsule; local trace only) and vehicle
 * spheres (mirrors have no client-side physics bodies). Nothing here touches
 * movement/collision state — the determinism contract is unaffected.
 *
 * Budget: hard pool of kMaxImpacts (48) live quads process-wide; a full pool
 * steals the slot closest to death.
 *
 * Wiring (see the lane wiring notes): TFWorldSetup::RenderWorld calls
 * Get().UpdateAndRender(*m_ctx, gfx, view, proj) once per frame right after
 * the TFGroundFx pass (before TFTransparentPass::Flush).
 *
 * OWNERSHIP: this header + TFImpactFx.cpp belong to ONE implementation agent
 * (impact-fx lane, W10). Structure follows TFGroundFx — the house pattern for
 * pooled billboard FX.
 */
#pragma once

#include "Core/TFTypes.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    class TFImpactFx
    {
      public:
        /// Process-wide instance. Justified singleton: pure client presentation
        /// with producers in other lanes' files (weapon fire, remote-fire seam,
        /// world render pass) — a static accessor keeps every hook to one line
        /// with no TFGameContext (frozen header) or lifecycle (Main.cpp) changes,
        /// exactly like TFViewModel / TFGroundFx / TFTransparentPass.
        static TFImpactFx& Get();

        /// Local client shot (TFWeaponSystem::ClientTriggerFire, after
        /// SpawnMuzzleFx): trace origin+dir (spread-perturbed fire ray) against
        /// world + pawn mirrors + vehicles and spawn a surface-flavored burst at
        /// the predicted impact point. No-op headless / on misses into the sky.
        void OnLocalShot(TFGameContext& ctx, const float origin[3], const float dir[3]);

        /// Remote validated shot (TFAudioAmbience::ClientOnRemoteFire — the
        /// 0x54F4 seam, muzzle pos + dir): spawn the puff at the tracer's visual
        /// endpoint. Cheap by construction: only for muzzles within
        /// kRemoteCamGateM of the camera, one WorldStatic|Vehicle ray capped at
        /// kTraceBudgetM (+ the analytic terrain fallback shared with the local
        /// path). Pawn capsules are skipped — this is flavor, not hit reg.
        void OnRemoteShot(TFGameContext& ctx, const float origin[3], const float dir[3]);

        /// Advance and draw all live impact quads. Call once per frame from
        /// TFWorldSetup::RenderWorld (BeginFrame active; basic shaders are
        /// (re)bound internally) right after TFGroundFx. Advances from an
        /// internal clock — no separate Update call needed. Safe no-op when
        /// headless or the pool is empty.
        void UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj);

        // Tuning surfaced for tests / debug UI readability.
        static constexpr float kTraceBudgetM = 250.0f;   ///< max visual trace length (any path)
        static constexpr float kRemoteCamGateM = 100.0f; ///< remote shots farther than this from the camera: skip
        static constexpr std::size_t kMaxImpacts = 48;   ///< hard process-wide live-quad cap (pooled)

      private:
        TFImpactFx() = default;

        /// Surface flavor of one impact.
        enum class Kind : uint8_t
        {
            Dust, ///< terrain / static world — dirt puff (alpha blend)
            Spark ///< pawn / vehicle — bright spark (additive, smaller)
        };

        /// One pooled live impact quad (world space).
        struct ImpactQuad
        {
            float pos[3] = {0.0f, 0.0f, 0.0f};
            float age = 0.0f;
            float life = 0.15f; ///< seconds; flipbook + fade run over this span
            float size = 0.5f;  ///< base billboard edge (meters)
            Kind kind = Kind::Dust;
            bool alive = false;
        };

        /// Resolve the visual impact point of a shot ray. Layered: live-Jolt
        /// physics ray (mask depends on includePawns), analytic terrain march,
        /// then (local only) pawn-capsule + vehicle-sphere mirror tests clipped
        /// to the world hit. Returns false when nothing lies within maxDist.
        bool TraceImpact(TFGameContext& ctx, const float origin[3], const float dir[3], float maxDist,
                         bool includePawns, float outPoint[3], Kind& outKind) const;

        /// Camera position (module-owned camera via TFWorldSetup::GetCamera,
        /// pawn eye fallback). False when neither exists (headless).
        static bool CameraPos(TFGameContext& ctx, float out[3]);

        void Spawn(const float point[3], Kind kind);
        float Rand01();

        std::array<ImpactQuad, kMaxImpacts> m_pool{};
        std::unique_ptr<Mesh> m_quad; ///< unit plane billboarded per quad (lazy)
        double m_lastRealTime = -1.0; ///< std::chrono seconds; -1 == first frame
        std::minstd_rand m_rng{0x1B4C7u};
    };

} // namespace Terrafront
