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
 *  - SCENE OBJECTS get one static Jolt BOX body each, computed from the node's
 *    world-space AABB. Boxes (not tri-meshes) keep broadphase cheap and make
 *    the client/server body sets trivially identical.
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

    class TFWorldCollision
    {
      public:
        TFWorldCollision();
        ~TFWorldCollision();

        /// Parse `scenePath` and create one static Jolt box body per collidable
        /// [Object] node (type=cube|model; spawnpoints/planes are skipped —
        /// the plane IS the analytic terrain). Model nodes stream their OBJ once
        /// for a local AABB; the node transform (scale, Euler-degree rotation,
        /// translation) is baked into a world-space AABB, so no desc.rotation is
        /// ever passed (its radians-vs-degrees trap stays out of this file).
        /// @return true when Jolt is live and at least one body was created.
        bool Build(TFGameContext& ctx, const std::string& scenePath);

        /// Remove every body this class created. Safe to call twice.
        void Shutdown();

        bool IsActive() const { return m_physics != nullptr && !m_bodies.empty(); }
        size_t BodyCount() const { return m_bodies.size(); }

        /// Number of ResolveMove calls that hit a static body (blocked or slid)
        /// since Build(). Monotonic; game-thread only. The chaos-validation
        /// harness (tf_validate) diffs this across a run to prove bot movement
        /// actually interacts with the collision world.
        uint64_t BlockedMoveCount() const { return m_blockedMoves; }

        /// Post-TFMoveStep collision resolve — THE shared client/server hook.
        /// Sweeps the pawn capsule (Movement mask) along the horizontal part of
        /// prevPos -> pos and slides along blocking geometry; pos/vel are
        /// adjusted in place. Y is left untouched (terrain clamp owns it; the
        /// caller re-clamps via TFWorldSetup::ResolveMoveCollision). No-op when
        /// inactive. Deterministic: static bodies only + same code both sides.
        void ResolveMove(const float prevPos[3], float pos[3], float vel[3]) const;

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
