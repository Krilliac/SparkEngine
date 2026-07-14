/**
 * @file TFBlobShadows.h
 * @brief Client-side ground-projected "blob" shadows under pawns, vehicles,
 *        and deployables (W13 shadow-polish lane).
 *
 * The basic render path has no shadow map (see the GetOrCreateSoftCircleShadowSRV
 * comment block in Graphics/GraphicsDeviceResourcesWindows.cpp for why real
 * shadow-map integration was out of scope for this pass). This is the scoped
 * substitute: a soft dark alpha disc, flat on the terrain, under every live
 * pawn/vehicle/deployable -- cheap, reads well at MMOFPS scale, and needs
 * nothing beyond the existing basic-shader draw surface.
 *
 * Per entity, per frame:
 *  - Radius from a small per-kind table (pawns fixed, vehicles by VehicleId,
 *    deployables via TFDeploySpecOf(kind)->overlapRadiusM).
 *  - Position: terrain height at the entity's (x,z) (TFWorldSetup::TerrainHeightAt),
 *    NOT the entity's own Y -- keeps the blob glued to the ground under slopes.
 *  - Height-above-ground fade: entity pos.y (feet-height for pawns/vehicles,
 *    ground-level for deployables) minus the terrain sample. A jumping pawn
 *    or an airborne Vulture VTOL shrinks + fades its blob out between
 *    kFadeStartM and kFadeEndM; this is an approximation (vehicle pos.y is not confirmed
 *    feet-exact the way PawnInfo.pos is documented to be) but the direction
 *    is always right: higher above ground -> smaller/fainter, never wrong-way.
 *  - Sun-direction offset: nudges the blob toward the ground projection
 *    opposite the sun (Spark::TimeOfDaySystem::GetInstance(), the SAME
 *    per-module-image instance TFDayNight drives -- both live in this DLL, so
 *    no cross-DLL / GraphicsEngine plumbing is needed to read it). Magnitude
 *    is clamped small; this is a cheap directional feel, not a true shadow
 *    projection.
 *  - Distance-culled from the camera and capped at kMaxBlobs process-wide.
 *
 * Wiring (see the lane wiring notes): TFWorldSetup::RenderWorld calls
 * Get().UpdateAndRender(*m_ctx, gfx, view, proj) once per frame, right next to
 * the TFGroundFx / TFImpactFx calls it structurally mirrors.
 *
 * OWNERSHIP: this header + TFBlobShadows.cpp belong to ONE implementation
 * agent (shadow-polish lane, W13). Structure follows TFGroundFx / TFImpactFx
 * -- the house pattern for pooled client-only basic-path draws.
 */
#pragma once

#include "Core/TFTypes.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <cstdint>
#include <memory>
#include <vector>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    class TFBlobShadows
    {
      public:
        /// Process-wide instance. Justified singleton: pure client presentation
        /// with a single producer (the TFWorldSetup render path, owned by
        /// another lane) -- a static accessor keeps the wiring to one line with
        /// no TFGameContext (frozen header) or lifecycle (Main.cpp) changes,
        /// exactly like TFGroundFx / TFImpactFx / TFViewModel.
        static TFBlobShadows& Get();

        /// Gather live pawns/vehicles/deployables and draw a blob shadow under
        /// each (camera-distance culled, capped at kMaxBlobs). Call once per
        /// frame from TFWorldSetup::RenderWorld (BeginFrame active; basic
        /// shaders are (re)bound internally). Safe no-op when headless or
        /// before the relevant systems are published. No persistent particle
        /// state -- this rebuilds a snapshot every call, so there is no
        /// separate Update needed.
        void UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj);

        // Tuning surfaced for readability / debug UI.
        static constexpr std::size_t kMaxBlobs = 128;   ///< hard process-wide cap per frame
        static constexpr float kMaxDrawDistM = 70.0f;   ///< camera-distance cull radius
        static constexpr float kFadeStartM = 0.3f;      ///< height above ground where fade begins
        static constexpr float kFadeEndM = 2.5f;        ///< height above ground where the blob is gone
        static constexpr float kBaseAlpha = 0.45f;      ///< peak opacity at ground level
        static constexpr float kMaxSunOffsetM = 0.5f;   ///< cap on the sun-direction ground offset

      private:
        TFBlobShadows() = default;

        /// One blob to draw this frame (world space, already fade/culled).
        struct BlobDraw
        {
            float x = 0.0f, y = 0.0f, z = 0.0f; ///< world position (terrain height + epsilon)
            float scale = 1.0f;                 ///< world-space edge length (CreatePlane(1,1) is a 1x1 unit square)
            float alpha = 0.0f;                 ///< final draw alpha
        };

        /// Footprint radius for a vehicle kind (local approximation -- every FX
        /// lane in this module keeps its own small table; see TFGroundFx.cpp /
        /// TFImpactFx.cpp HullRadiusM for the sibling precedents).
        static float VehicleBlobRadiusM(VehicleId id);

        /// Push one candidate into m_scratch if it survives distance culling
        /// and the height fade doesn't zero it out. `pos` is feet/ground-level
        /// position; `radiusM` is the undimmed footprint radius.
        void Consider(TFGameContext& ctx, const float pos[3], float radiusM, const float camPos[3],
                     float sunOffX, float sunOffZ);

        std::vector<BlobDraw> m_scratch; ///< rebuilt each frame; capacity reused (not a particle pool)
        std::unique_ptr<Mesh> m_quad;    ///< unit ground plane (XZ, +Y normal), scaled per blob (lazy)
    };

} // namespace Terrafront
