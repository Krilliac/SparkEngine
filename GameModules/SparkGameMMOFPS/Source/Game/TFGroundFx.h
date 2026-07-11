/**
 * @file TFGroundFx.h
 * @brief Client-side hover-dust emitter: short-lived camera-facing flipbook
 *        quads spawned at the terrain under fast-moving vehicles.
 *
 * Pure client presentation — no networking or server state. For every vehicle
 * moving faster than ~3 m/s it spawns dust puffs at the ground under the hull
 * (terrain height sampled per puff), spawn rate proportional to speed, hard
 * cap of 64 live puffs process-wide. Puffs are textured with the 4-frame
 * horizontal sprite strip Textures/MMOFPS/fx/hover_dust.png (frame selected by
 * puff age via UV tiling/offset — same flipbook trick as the TFViewModel
 * muzzle flash), drawn additively, camera-facing, fading + growing over
 * ~0.6 s.
 *
 * Vehicle speed is derived client-side from per-entity position deltas, which
 * works identically on server roles (authoritative records) and pure clients
 * (interpolated replication mirrors) — no TFVehicleSystem API changes needed.
 *
 * Wiring (one line, see the lane wiring notes): TFWorldSetup::RenderWorld
 * calls Get().UpdateAndRender(ctx, gfx, view, proj) once per frame after the
 * world/ECS/shot-FX passes while BeginFrame is active.
 *
 * OWNERSHIP: this header + TFGroundFx.cpp belong to ONE implementation agent
 * (fx-flipbook lane).
 */
#pragma once

#include "Core/TFTypes.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    class TFGroundFx
    {
      public:
        /// Process-wide instance. Justified singleton: pure client presentation
        /// with a single producer (the TFWorldSetup render path, owned by another
        /// lane) — a static accessor keeps the wiring to one line with no
        /// TFGameContext (frozen header) or lifecycle (Main.cpp) changes, exactly
        /// like TFViewModel.
        static TFGroundFx& Get();

        /// Advance the emitter and draw all live puffs. Call once per frame from
        /// TFWorldSetup::RenderWorld (BeginFrame active; basic shaders are
        /// (re)bound internally). Safe no-op when headless or before the vehicle/
        /// world systems are published. Advances from an internal clock — no
        /// separate Update call needed.
        void UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj);

      private:
        TFGroundFx() = default;

        /// One live dust sprite (world space).
        struct DustPuff
        {
            float pos[3] = {0.0f, 0.0f, 0.0f};
            float vel[3] = {0.0f, 0.0f, 0.0f};
            float age = 0.0f;
            float life = 0.6f; ///< seconds; flipbook + fade run over this span
        };

        /// Per-vehicle motion memory (speed from position deltas + spawn debt).
        struct VehicleTrack
        {
            float prevPos[3] = {0.0f, 0.0f, 0.0f};
            bool hasPrev = false;
            float speedMps = 0.0f;           ///< smoothed horizontal speed
            float moveDir[2] = {0.0f, 1.0f}; ///< horizontal travel direction (x,z)
            float spawnAccum = 0.0f;         ///< fractional puffs owed
            uint32_t lastSeenFrame = 0;      ///< for pruning destroyed vehicles
        };

        float Rand01();

        std::vector<DustPuff> m_puffs;
        std::unordered_map<EntityId, VehicleTrack> m_tracks;
        std::unique_ptr<Mesh> m_quad; ///< unit plane billboarded per puff (lazy)
        double m_lastRealTime = -1.0; ///< std::chrono seconds; -1 == first frame
        uint32_t m_frame = 0;         ///< frame counter for track pruning
        std::minstd_rand m_rng{0xD057u};
    };

} // namespace Terrafront
