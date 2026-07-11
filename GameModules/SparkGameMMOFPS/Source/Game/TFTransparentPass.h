/**
 * @file TFTransparentPass.h
 * @brief Sorted alpha-blended draw pass for the module's basic-shader path
 *        (W8 render-transparency lane).
 *
 * OWNERSHIP: this header + TFTransparentPass.cpp belong to ONE implementation
 * agent (render-transparency lane).
 *
 * Producers Submit() transparent surfaces during the frame's Update pass
 * (first user: TFDeployableSystem's shield-wall energy plane); the world
 * renderer calls Flush() ONCE after all opaque draws (one-line call site in
 * TFWorldSetup::RenderWorld, see the lane wiring notes). Flush sorts the
 * batch back-to-front by camera distance, draws every item through the
 * engine's basic shader with SetBasicBlendMode(Alpha), then restores Opaque
 * blending and clears the batch.
 *
 * Depth policy (W9): Flush binds the engine's read-only depth state via
 * SetBasicDepthMode(ReadOnly) — depth test LESS_EQUAL against the opaque
 * scene, write mask ZERO — and restores Default afterwards. Transparent
 * surfaces therefore never occlude later draws; back-to-front sorting still
 * orders compositing between the transparent surfaces themselves. Do NOT
 * hack raw OMSetDepthStencilState here — go through SetBasicDepthMode so the
 * restore path stays in sync with the frame's baseline state.
 *
 * Why a singleton: same justification as TFViewModel — pure client-side
 * presentation, the producers and the flush site are owned by other lanes,
 * and a static accessor keeps their wiring to one line each with no
 * TFGameContext (frozen header) or Main.cpp lifecycle changes. Module DLL =
 * one image, so the per-image static is THE instance.
 */
#pragma once

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class GraphicsEngine;
class Mesh;
struct ID3D11ShaderResourceView; // real type on Windows, PlatformD3DStubs elsewhere

namespace Terrafront
{

    class TFTransparentPass
    {
      public:
        /// Process-wide instance (module image). See file header for rationale.
        static TFTransparentPass& Get();

        /// Queue one transparent draw for this frame.
        /// @param mesh     Device-ready mesh (caller keeps ownership; must stay
        ///                 alive through Flush — cache it like the ECS meshes).
        /// @param world    Full world matrix (scale * rotation * translation).
        /// @param srv      Albedo texture; nullptr = engine default 1x1 white.
        /// @param tint     RGB multiplies the texture (ObjectColor.rgb, alpha
        ///                 lane of ObjectColor stays 1); tint.w is the surface
        ///                 ALPHA, fed to MaterialProperties.w — the basic PS
        ///                 computes a = texAlpha * 1 * MaterialProperties.w.
        /// @param emissive MaterialProperties.z unlit glow (0 = lit only).
        /// Ignored (counted, logged once) past kMaxItems — a producer leak must
        /// not grow unbounded if Flush is never reached.
        void Submit(Mesh* mesh, const DirectX::XMMATRIX& world, ID3D11ShaderResourceView* srv,
                    const DirectX::XMFLOAT4& tint, float emissive);

        /// Sort back-to-front against the camera (from the inverse view matrix)
        /// and draw everything queued since the last Flush, then restore Opaque
        /// blending + default texture and clear the batch. Call once per frame
        /// from the world render AFTER all opaque passes (BeginFrame active).
        /// Clears the batch even when gfx/context are unavailable.
        void Flush(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

        /// Shared two-sided unit quad: XY plane, z = 0, spans [-0.5, 0.5] in
        /// x/y (scale sets final size), 0..1 UVs, both windings so it is
        /// visible from either side under back-face culling with no rasterizer
        /// state change. Lazily created on the given device; nullptr while the
        /// device is unavailable or creation failed.
        Mesh* GetUnitPlane(GraphicsEngine& gfx);

        /// Items queued and not yet flushed (debug/tests).
        size_t PendingCount() const { return m_items.size(); }

      private:
        TFTransparentPass() = default;

        struct Item
        {
            Mesh* mesh;
            // Stored UNALIGNED (XMFLOAT4X4, not XMMATRIX): stable_sort's temp
            // buffer instantiates std::aligned_storage, and a 16-byte-aligned
            // Item trips MSVC's C2338 extended-alignment static_assert.
            // Loaded back into an XMMATRIX at draw time (integrator, W8).
            DirectX::XMFLOAT4X4 world;
            ID3D11ShaderResourceView* srv;
            DirectX::XMFLOAT4 tint;
            float emissive;
            float dist2; ///< squared camera distance, filled by Flush for the sort
        };

        /// Hard cap on queued items per frame (drops + one warn past it).
        static constexpr size_t kMaxItems = 256;

        std::vector<Item> m_items;
        std::unique_ptr<Mesh> m_unitPlane;
        bool m_planeBuildFailed{false}; ///< don't retry a failed build every frame
        bool m_warnedDrop{false};       ///< log the overflow once, not per item
        uint32_t m_dropped{0};
        uint32_t m_drawnLastFlush{0};
    };

} // namespace Terrafront
