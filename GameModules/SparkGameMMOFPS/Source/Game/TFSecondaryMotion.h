/**
 * @file TFSecondaryMotion.h
 * @brief Reusable client-side secondary-motion framework: damped point-mass
 *        pendulum chains (Verlet integration + hard link constraints) driven by
 *        parent-anchor motion, gravity, and queued impulses. Pure presentation
 *        math — no Jolt, no net state, no GPU resources.
 *
 * OWNERSHIP: this header + TFSecondaryMotion.cpp belong to the secondary-motion
 * lane (with TFViewModel.h/.cpp).
 *
 * Usage (see TFViewModel.cpp charm/strap for the reference wiring):
 *   TFPendulumHandle h = TFSecondaryMotion::Get().AttachPendulum(params);
 *   // per frame, with the attach point already transformed to WORLD space:
 *   chain->AddImpulse(velWorldMps);            // optional punch (fire, land, ...)
 *   chain->Update(anchorWorld, dtSec);         // integrate + constrain
 *   for (int i = 0; i < chain->LinkCount(); ++i)
 *       draw(chain->LinkWorld(i, thickM));     // unit-cube world matrices
 *   draw(chain->TipWorld(fobScaleM));          // oriented trinket at the tip
 *
 * The simulation is world-space on purpose: an anchor that composes every
 * parent motion source (camera turn, walk bob, recoil kick, vehicle bounce)
 * drives the chain with zero extra plumbing. Future world uses (vehicle
 * antennae, capture-point banners) attach the same way — transform the local
 * mount point to world each frame and call Update.
 */
#pragma once

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    /// Tuning for one pendulum chain. Defaults suit a small dangling trinket.
    struct TFPendulumParams
    {
        int linkCount = 3;                      ///< rigid segments (points = linkCount + 1)
        float linkLengthM = 0.03f;              ///< per-segment length, meters
        float gravityMps2 = 9.81f;              ///< world -Y pull (lower == lazier swing)
        float dampingPerSec = 2.5f;             ///< exponential velocity damping (1/s)
        float stiffness = 0.0f;                 ///< 1/s^2 pull toward restDir off each parent
                                                ///< point (0 == free chain; >0 for antennae
                                                ///< / banners that want a preferred pose)
        float restDir[3] = {0.0f, -1.0f, 0.0f}; ///< world rest/snap direction (unit)
        float teleportSnapM = 2.0f;             ///< anchor jump beyond this => snap, no whip
    };

    /// One damped pendulum chain, simulated in WORLD space. Point 0 is pinned to
    /// the anchor passed to Update(); the rest integrate under gravity + inertia
    /// with hard distance constraints (ordered projection pass, root to tip).
    class TFPendulumChain
    {
      public:
        TFPendulumChain() = default;

        /// (Re)build the chain from params. Points snap to the anchor on the
        /// next Update() (rest pose along params.restDir).
        void Configure(const TFPendulumParams& params);

        /// Queue a world-space velocity change (m/s) applied to every free point
        /// on the next Update(). Accumulates across calls within a frame.
        void AddImpulse(const DirectX::XMFLOAT3& velWorldMps);

        /// Integrate one frame. anchorWorld is the attach point in world space;
        /// dtSec is clamped internally (hitch-safe). First call (or a teleport /
        /// non-finite blowup) snaps the chain to rest instead of whipping.
        void Update(const DirectX::XMFLOAT3& anchorWorld, float dtSec);

        int LinkCount() const { return m_pos.empty() ? 0 : static_cast<int>(m_pos.size()) - 1; }
        bool Ready() const { return m_hasAnchor; }

        /// Endpoints of one link, world space. link in [0, LinkCount()).
        void GetLinkEndpoints(int link, DirectX::XMFLOAT3& outA, DirectX::XMFLOAT3& outB) const;

        /// World matrix stretching a unit cube joint-to-joint along one link.
        DirectX::XMMATRIX LinkWorld(int link, float thickM) const;

        /// World matrix for an oriented box hanging off the chain tip (charm fob,
        /// banner cloth, antenna tip light): scaled scaleM, +Z along the last
        /// link's direction, centered half its depth past the tip point.
        DirectX::XMMATRIX TipWorld(const DirectX::XMFLOAT3& scaleM) const;

      private:
        void SnapToRest(const DirectX::XMFLOAT3& anchorWorld);

        TFPendulumParams m_params;
        std::vector<DirectX::XMFLOAT3> m_pos;          ///< current world positions
        std::vector<DirectX::XMFLOAT3> m_prev;         ///< previous positions (Verlet)
        DirectX::XMFLOAT3 m_impulse{0.0f, 0.0f, 0.0f}; ///< queued, consumed by Update
        bool m_hasAnchor = false;
    };

    /// Opaque chain handle. 0 == invalid.
    using TFPendulumHandle = uint32_t;

    /// Process-wide chain registry. Justified singleton (same reasoning as
    /// TFViewModel::Get): one local client, pure presentation, and consumers
    /// live in render paths owned by other lanes — a static accessor keeps
    /// their wiring to a line or two with no TFGameContext (frozen) changes.
    class TFSecondaryMotion
    {
      public:
        static TFSecondaryMotion& Get();

        /// Create a chain; returns its handle (never 0).
        TFPendulumHandle AttachPendulum(const TFPendulumParams& params);

        /// Destroy a chain. Ignores 0 / unknown handles.
        void Detach(TFPendulumHandle handle);

        /// Look up a chain (nullptr for 0 / unknown). Pointer valid until the
        /// next Attach/Detach — re-Find each frame, do not cache.
        TFPendulumChain* Find(TFPendulumHandle handle);

      private:
        TFSecondaryMotion() = default;

        std::unordered_map<TFPendulumHandle, TFPendulumChain> m_chains;
        TFPendulumHandle m_nextHandle = 1;
    };

} // namespace Terrafront
