/**
 * @file TFWorldCollisionDecor.cpp
 * @brief W10 decor-collision + W11 gate-passages: AddModelObb / AddObbPart
 *        static-OBB registration, per-body removal and the post-bulk broadphase
 *        compaction. Scene parse/Build lives in TFWorldCollision.cpp and the
 *        shared move resolver in TFWorldCollisionMove.cpp (same class, split
 *        per the repo file-size rules — mirrors the TFWorldSetup/-Net split).
 */
#include "World/TFWorldCollision.h"

#include "World/TFWorldCollisionInternal.h" // WorldCollisionDetail: kMaxBodies, kMinHalfExtentM, kDegToRad

#include "Physics/PhysicsSystem.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

namespace Terrafront
{

    using namespace WorldCollisionDetail;

    // -----------------------------------------------------------------------
    // W10 decor-collision: model-bounds OBB registration (TFRegionDecor)
    // -----------------------------------------------------------------------

    std::shared_ptr<::PhysicsBody> TFWorldCollision::AddModelObb(const std::string& objPath, const float pos[3],
                                                                 float yawDeg, const std::string& name)
    {
        if (!m_physics)
            return nullptr; // Jolt absent: decor stays walk-through, like the scene set
        if (m_bodies.size() >= kMaxBodies)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] world collision: body cap %zu reached - no body for %s",
                           kMaxBodies, name.c_str());
            return nullptr;
        }

        auto it = m_modelAabbCache.find(objPath);
        if (it == m_modelAabbCache.end())
        {
            float mn[3], mx[3];
            if (!ObjLocalAabb(objPath, mn, mx))
            {
                // NO unit-cube fallback here (unlike scene models, which must
                // mirror the engine's placeholder mesh): a wrong-size invisible
                // wall around decor is worse than leaving the piece walk-through.
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] world collision: OBJ %s unreadable - decor piece %s stays walk-through",
                               objPath.c_str(), name.c_str());
                return nullptr;
            }
            it = m_modelAabbCache
                     .emplace(objPath, std::make_pair(std::array<float, 3>{mn[0], mn[1], mn[2]},
                                                      std::array<float, 3>{mx[0], mx[1], mx[2]}))
                     .first;
        }
        const std::array<float, 3>& lmin = it->second.first;
        const std::array<float, 3>& lmax = it->second.second;

        PhysicsBodyDesc desc;
        desc.type = PhysicsBodyType::Static;
        desc.mass = 0.0f;
        desc.shape.type = CollisionShapeType::Box;

        // Tight OBB at scale 1 — the Build() diagonal-yaw math verbatim (also
        // exact for 0/90/180/270: the rotated box degenerates to the same AABB).
        // desc.rotation is RADIANS (PhysicsSystemQueries.cpp -> JPH sEulerAngles;
        // the PhysicsTypes.h "degrees" doc lies). Decor yaw arrives in the scene/
        // Transform DEGREES convention — convert here.
        const float ryRad = yawDeg * kDegToRad;
        const float cyaw = std::cos(ryRad), syaw = std::sin(ryRad);
        const float ctr[3] = {(lmin[0] + lmax[0]) * 0.5f, (lmin[1] + lmax[1]) * 0.5f, (lmin[2] + lmax[2]) * 0.5f};
        // Body center = piece pos + Ry(yaw) * local center; the yaw mapping
        // (x' = x*c + z*s, z' = -x*s + z*c) is the play-test-validated Build()
        // convention.
        desc.position = {ctr[0] * cyaw + ctr[2] * syaw + pos[0], ctr[1] + pos[1],
                         -ctr[0] * syaw + ctr[2] * cyaw + pos[2]};
        desc.shape.dimensions = {std::max(std::fabs((lmax[0] - lmin[0]) * 0.5f), kMinHalfExtentM),
                                 std::max(std::fabs((lmax[1] - lmin[1]) * 0.5f), kMinHalfExtentM),
                                 std::max(std::fabs((lmax[2] - lmin[2]) * 0.5f), kMinHalfExtentM)};
        desc.rotation = {0.0f, ryRad, 0.0f}; // radians (see comment above)
        desc.material.friction = 0.7f;
        desc.material.restitution = 0.0f;
        desc.collisionGroup = CollisionLayers::WorldStatic;
        desc.collisionMask = CollisionLayers::All;
        desc.name = name;

        std::shared_ptr<PhysicsBody> body = m_physics->CreateBody(desc);
        if (!body)
            return nullptr;
        // Defensive layer push, mirroring Build() (never rely on the default).
        body->SetCollisionGroup(CollisionLayers::WorldStatic);
        body->SetCollisionMask(CollisionLayers::All);
        m_bodies.push_back(body);
        return body;
    }

    std::shared_ptr<::PhysicsBody> TFWorldCollision::AddObbPart(const float piecePos[3], float pieceYawDeg,
                                                                const float partOffset[3], const float partSize[3],
                                                                float partYawDeg, const std::string& name)
    {
        if (!m_physics)
            return nullptr; // Jolt absent: decor stays walk-through, like the scene set
        if (m_bodies.size() >= kMaxBodies)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] world collision: body cap %zu reached - no body for %s",
                           kMaxBodies, name.c_str());
            return nullptr;
        }

        PhysicsBodyDesc desc;
        desc.type = PhysicsBodyType::Static;
        desc.mass = 0.0f;
        desc.shape.type = CollisionShapeType::Box;

        // W11 gate-passages: the part box is authored MODEL-LOCAL (center =
        // partOffset, FULL size = partSize, local yaw = partYawDeg) and gets
        // composed with the owning piece's world transform. Body center =
        // piecePos + Ry(pieceYaw) * partOffset — the exact AddModelObb yaw
        // mapping (x' = x*c + z*s, z' = -x*s + z*c), play-test-validated.
        // Yaw about a common axis composes additively, so the body's rotation
        // is pieceYaw + partYaw. desc.rotation is RADIANS (see AddModelObb);
        // both yaw arguments arrive in the scene/Transform DEGREES convention.
        const float ryPieceRad = pieceYawDeg * kDegToRad;
        const float cyaw = std::cos(ryPieceRad), syaw = std::sin(ryPieceRad);
        desc.position = {partOffset[0] * cyaw + partOffset[2] * syaw + piecePos[0], partOffset[1] + piecePos[1],
                         -partOffset[0] * syaw + partOffset[2] * cyaw + piecePos[2]};
        desc.shape.dimensions = {std::max(std::fabs(partSize[0]) * 0.5f, kMinHalfExtentM),
                                 std::max(std::fabs(partSize[1]) * 0.5f, kMinHalfExtentM),
                                 std::max(std::fabs(partSize[2]) * 0.5f, kMinHalfExtentM)};
        desc.rotation = {0.0f, (pieceYawDeg + partYawDeg) * kDegToRad, 0.0f}; // radians (see comment above)
        desc.material.friction = 0.7f;
        desc.material.restitution = 0.0f;
        desc.collisionGroup = CollisionLayers::WorldStatic;
        desc.collisionMask = CollisionLayers::All;
        desc.name = name;

        std::shared_ptr<PhysicsBody> body = m_physics->CreateBody(desc);
        if (!body)
            return nullptr;
        // Defensive layer push, mirroring Build() (never rely on the default).
        body->SetCollisionGroup(CollisionLayers::WorldStatic);
        body->SetCollisionMask(CollisionLayers::All);
        m_bodies.push_back(body);
        return body;
    }

    void TFWorldCollision::RemoveBody(const std::shared_ptr<::PhysicsBody>& body)
    {
        if (!body)
            return;
        const auto it = std::find(m_bodies.begin(), m_bodies.end(), body);
        if (it == m_bodies.end())
            return; // foreign / already removed
        if (m_physics)
            m_physics->RemoveBody(*it);
        m_bodies.erase(it);
    }

    void TFWorldCollision::OptimizeBroadPhase()
    {
        if (m_physics)
            m_physics->OptimizeBroadPhase();
    }

} // namespace Terrafront
