/**
 * @file TFVehicleFx.h
 * @brief Client-side vehicle damage-state presentation: hull-hp-fraction
 *        smoke/spark/fire billboards plus the occasional critical-tier
 *        engine-pitch-down audio cue.
 *
 * Pure client presentation -- no server state, no TFVehicleSystem API
 * changes (reads TFVehicleInfo.hp/maxHp off the existing ForEachVehicle
 * enumerator, exactly like TFGroundFx reads pos for its dust emitter). Tiers
 * mirror TFVehicleSystem's damage-state thresholds (healthy >66% hp,
 * damaged 33-66%, critical <33% -- see kDamagedHpFrac/kCriticalHpFrac in the
 * .cpp). The critical threshold (0.33f) is INTENTIONALLY the same numeric
 * literal as TFVehicleSystem.cpp's kTFVehCriticalHpFrac so the visuals and
 * the server-authoritative speed/turn penalty change state at the same hp
 * fraction; same cross-file duplication convention TFVehicleSystem.cpp
 * already uses for its VTOL math-path/Jolt-path tuning parity. The damaged
 * threshold (0.66f) is visuals-only -- no server-side counterpart exists.
 *
 *  - Damaged (33-66% hp): light gray smoke trail off the hull + an
 *    occasional small additive spark.
 *  - Critical (<33% hp): heavy near-black smoke (higher rate, bigger,
 *    darker) + a continuous additive fire flicker at the hull + a periodic
 *    pitched-down one-shot of the vehicle's own vehicles.json `audioEngine`
 *    loop clip (previously-authored, never wired -- TFDataTables.cpp loads
 *    it, nothing played it before this).
 *
 * Destroyed vehicles are handled by TFVehicleSystem itself (persistent
 * charred wreck, see DestroyVehicle/OnNetVehDestroy) -- this class only
 * decorates HP > 0 hulls and drops its per-vehicle tracking the moment a
 * vehicle stops being enumerated (destroyed, or the mirror was dropped).
 *
 * Smoke sheet: Textures/MMOFPS/fx/veh_smoke.png (NEW, 4-frame horizontal
 * flipbook, white RGB + alpha-only shape -- the render-time color param
 * fully controls the tint, same trick as TFGroundFx's hover_dust sheet).
 * Fire/spark reuse Textures/MMOFPS/fx/muzzle_flash_common.png (additive;
 * same reuse precedent as TFGrenadeSystem's boom flipbook and TFImpactFx's
 * spark flavor -- no new fire texture needed).
 *
 * Wiring (one line, see the lane wiring notes): TFWorldSetup::RenderWorld
 * calls Get().UpdateAndRender(ctx, gfx, view, proj) once per frame, right
 * after the TFGroundFx pass.
 *
 * OWNERSHIP: this header + TFVehicleFx.cpp belong to the vehicle-damage-
 * states lane. Structure follows TFGroundFx/TFImpactFx -- the house pattern
 * for pooled billboard FX.
 */
#pragma once

#include "Core/TFTypes.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    class TFVehicleFx
    {
      public:
        /// Process-wide instance. Justified singleton: pure client presentation
        /// with a single producer (the TFWorldSetup render path, owned by
        /// another lane) -- a static accessor keeps the wiring to one line
        /// with no TFGameContext (frozen header) or lifecycle (Main.cpp)
        /// changes, exactly like TFGroundFx / TFImpactFx.
        static TFVehicleFx& Get();

        /// Advance the emitters, trigger the critical-tier audio cue, and draw
        /// all live billboards. Call once per frame from
        /// TFWorldSetup::RenderWorld (BeginFrame active; basic shaders are
        /// (re)bound internally). Safe no-op when headless or before the
        /// vehicle/world systems are published. Advances from an internal
        /// clock -- no separate Update call needed.
        void UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj);

      private:
        TFVehicleFx() = default;

        enum class Kind : uint8_t
        {
            Smoke, ///< alpha-blended, tint carries damaged (gray) vs critical (near-black)
            Fire,  ///< additive, critical-tier engine-bay flicker
            Spark  ///< additive, damaged-tier occasional pop
        };

        /// One live billboard (world space).
        struct FxQuad
        {
            float pos[3] = {0.0f, 0.0f, 0.0f};
            float vel[3] = {0.0f, 0.0f, 0.0f};
            float age = 0.0f;
            float life = 0.6f;
            float size0 = 0.5f, size1 = 1.0f;
            float alpha = 0.5f;
            float color[3] = {1.0f, 1.0f, 1.0f};
            Kind kind = Kind::Smoke;
        };

        /// Per-vehicle emission memory.
        struct VehicleTrack
        {
            float smokeAccum = 0.0f;     ///< fractional smoke puffs owed
            float fireAccum = 0.0f;      ///< fractional fire licks owed (critical only)
            float sparkCooldown = -1.0f; ///< seconds to next damaged-tier spark; <0 = roll on next tick
            float engineCueCooldown = 0.0f; ///< seconds to next critical-tier pitched-down engine cue
            uint32_t lastSeenFrame = 0;  ///< for pruning vehicles that stopped being enumerated
        };

        float Rand01();
        void Spawn(Kind kind, const float pos[3], const float vel[3], float life, float size0, float size1,
                  float alpha, const float color[3]);
        void PlayEngineCue(TFGameContext& ctx, EntityId vehicle, VehicleId vehId, const float pos[3]);

        std::vector<FxQuad> m_quads;
        std::unordered_map<EntityId, VehicleTrack> m_tracks;
        std::unique_ptr<Mesh> m_quad; ///< unit plane billboarded per FxQuad (lazy)
        double m_lastRealTime = -1.0; ///< std::chrono seconds; -1 == first frame
        uint32_t m_frame = 0;         ///< frame counter for track pruning
        std::minstd_rand m_rng{0x7EC4A5u};
        std::unordered_set<std::string> m_loadedEngineSounds; ///< LoadSound cache (PlayOneShot convention)
    };

} // namespace Terrafront
