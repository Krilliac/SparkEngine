/**
 * @file TFWorldCollision.h
 * @brief Static Jolt collision for the authored scene (mesas, structures, props)
 *        plus the shared capsule-sweep move resolver.
 *
 * OWNERSHIP: world-collision lane (2026-07-10 play-test hotfix wave). Owned and
 * built by TFWorldSetup right after scene load, on BOTH server and client.
 *
 * ## Terrain / object split (deliberate)
 *  - TERRAIN STAYS ANALYTIC. TFWorldSetup::TerrainHeightAt is the one true
 *    ground on every role (already clamped per tick inside TFMoveStep step 7).
 *    No heightfield mesh body is ever built here — a Jolt heightfield would be
 *    a second, subtly different ground truth and a determinism hazard.
 *  - SCENE OBJECTS get one static Jolt BOX body each. Axis-aligned nodes
 *    (rotation a multiple of 90 degrees) bake the node transform into a
 *    world-space AABB. Yaw-only diagonal nodes (35/45/135-degree barriers)
 *    get a properly ROTATED box (collision-v2) — the old conservative fat
 *    AABB was up to ~40% oversize on those walls. Boxes (not tri-meshes)
 *    keep broadphase cheap and make the client/server body sets trivially
 *    identical.
 *
 * ## Determinism contract
 * Bodies are parsed straight from the .scene file ([Object] INI sections) —
 * NOT from SceneManager/GameObjects — because headless dedicated servers have
 * no GraphicsEngine and therefore no SceneManager. Both roles read the same
 * file in the same order with the same float math, so the static body set is
 * identical and ResolveMove() (used by the server integrator AND the client
 * prediction simulator) produces the same slide on both sides.
 *
 * ## Jolt-absent behavior
 * Build() probes PhysicsSystem::GetJoltSystem() (the no-Jolt stub hands out
 * valid dummy bodies, so null-body checks are NOT sufficient). When Jolt is
 * absent the class stays inactive and ResolveMove() is a no-op — movement
 * falls back to today's terrain-clamp-only behavior.
 */
#pragma once

#include "Core/TFTypes.h"

#include <memory>
#include <string>
#include <vector>

class PhysicsSystem; // Physics/PhysicsSystem.h (engine, global namespace)
class PhysicsBody;   // Physics/PhysicsBody.h

namespace Terrafront
{

    /// Capsule-sweep tuning. Part of the client/server determinism contract —
    /// change these on both sides at once (they live only here, so that is free).
    constexpr float kTFStepUpM = 0.35f;   ///< swept capsule starts this far above the feet (step-over height)
    constexpr float kTFMoveSkinM = 0.02f; ///< stop this short of a blocking surface
    constexpr int kTFSlideIters = 3;      ///< max slide re-projections per tick
    /// Vertical-resolve tuning (collision-v2). Also determinism-contract values.
    constexpr float kTFLandSnapM = 0.05f;      ///< extra downward sweep so standing-on-body contact stays grounded
    constexpr float kTFWalkableNormalY = 0.5f; ///< min upward normal.y for a down-sweep hit to count as a landing

    class TFWorldCollision
    {
      public:
        TFWorldCollision();
        ~TFWorldCollision();

        /// Parse `scenePath` and create one static Jolt box body per collidable
        /// [Object] node (type=cube|model; spawnpoints/planes are skipped —
        /// the plane IS the analytic terrain). Model nodes stream their OBJ once
        /// for a local AABB. Nodes whose rotation is a 90-degree multiple bake
        /// the transform into a world-space AABB (exact, no desc.rotation).
        /// Yaw-only diagonal nodes get a tight ROTATED box: desc.rotation is
        /// passed in RADIANS — PhysicsSystemQueries.cpp feeds it straight to
        /// JPH::Quat::sEulerAngles; the PhysicsTypes.h "degrees" doc is wrong
        /// (TFVehiclePhysics depends on the radians behavior too). Scene files
        /// store rotation in DEGREES, so this file converts. Multi-axis
        /// non-90 rotations (none shipped) keep the conservative baked AABB.
        /// @return true when Jolt is live and at least one body was created.
        bool Build(TFGameContext& ctx, const std::string& scenePath);

        /// Remove every body this class created. Safe to call twice.
        void Shutdown();

        bool IsActive() const { return m_physics != nullptr && !m_bodies.empty(); }
        size_t BodyCount() const { return m_bodies.size(); }

        /// Number of ResolveMove calls that hit a static body since Build():
        /// horizontal block/slide OR vertical landing/ceiling stop (collision-v2
        /// counts vertical hits too — at most one increment per call, so the
        /// counting seam is unchanged). Monotonic; game-thread only. The
        /// chaos-validation harness (tf_validate) diffs this across a run to
        /// prove bot movement actually interacts with the collision world.
        uint64_t BlockedMoveCount() const { return m_blockedMoves; }

        /// Post-TFMoveStep collision resolve — THE shared client/server hook.
        /// 1) Sweeps the pawn capsule (Movement mask) along the horizontal part
        ///    of prevPos -> pos and slides along blocking geometry.
        /// 2) collision-v2 vertical resolve at the resolved column: descending
        ///    moves land on static-body tops (feet = contact + skin; vel.y
        ///    zeroed; *grounded set true when provided) with a kTFLandSnapM
        ///    ground-snap so standing stays stable; ascending moves stop under
        ///    static-body undersides (vel.y zeroed).
        /// pos/vel are adjusted in place. The terrain clamp stays the
        /// floor-of-last-resort (caller re-clamps via
        /// TFWorldSetup::ResolveMoveCollision AFTER this). `grounded` is
        /// optional (may be null); it is only ever SET true (landing) — never
        /// cleared, TFMoveStep step 7 owns the airborne transition. No-op when
        /// inactive. Deterministic: static bodies only + same code both sides.
        void ResolveMove(const float prevPos[3], float pos[3], float vel[3], bool* grounded = nullptr) const;

      private:
        struct SceneObj
        {
            std::string type;  ///< "cube" | "model"
            std::string name;  ///< node name (body naming/debug)
            std::string model; ///< OBJ path for type=model
            float pos[3] = {0.0f, 0.0f, 0.0f};
            float scale[3] = {1.0f, 1.0f, 1.0f};
            float rotDeg[3] = {0.0f, 0.0f, 0.0f};
        };

        static bool ParseScene(const std::string& path, std::vector<SceneObj>& out);
        static bool ObjLocalAabb(const std::string& objPath, float outMin[3], float outMax[3]);

        ::PhysicsSystem* m_physics{nullptr}; // engine-owned; reached via IEngineContext
        std::vector<std::shared_ptr<::PhysicsBody>> m_bodies;

        /// ResolveMove is const (shared client/server hook) — the diagnostic
        /// counter is mutable bookkeeping, not simulation state, so mutating it
        /// there cannot affect determinism.
        mutable uint64_t m_blockedMoves{0};
    };

} // namespace Terrafront
