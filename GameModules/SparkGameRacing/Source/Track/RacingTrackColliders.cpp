/**
 * @file RacingTrackColliders.cpp
 * @brief RacingTrackSystem's static Jolt colliders: per-surface road meshes along the authored centerline and a
 *        run-off slab under the layout. Split from RacingTrackSystem.cpp (same class, feature-owned translation
 *        unit).
 */

#include "RacingTrackSystem.h"
#include "Physics/PhysicsBody.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/LogMacros.h"
#include "Vehicle/RacingVehicleSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace Racing
{
    void RacingTrackSystem::BuildTrackColliders()
    {
        RemoveTrackColliders();
        PhysicsSystem* physics = m_context ? m_context->GetPhysics() : nullptr;
        const uint32_t segmentCount = GetSegmentCount();
        if (!physics || !physics->GetJoltSystem() || segmentCount == 0)
            return;
        m_colliderPhysics = physics;

        // Road surface sits just above the run-off slab so wheel rays on the road never report the grass.
        constexpr float kRoadLift = 0.05f;
        constexpr float kRunOffMargin = 150.0f;

        // One triangle list per surface type; each body's friction is that surface's grip.
        struct SurfaceMesh
        {
            std::vector<XMFLOAT3> vertices;
            std::vector<uint32_t> indices;
        };
        std::vector<SurfaceMesh> meshes(static_cast<size_t>(SurfaceType::Count));

        // Each segment is two half-strips: the first half carries the start waypoint's surface, the second the
        // end waypoint's (matching GetSurfaceAt). Both ends are extended by the half-width with flat caps, so the
        // strips of adjacent segments overlap at every waypoint and a corner leaves no gap in the road.
        auto addStrip =
            [&](SurfaceType surface, const TrackWaypoint& from, const TrackWaypoint& to, const float(&stations)[3])
        {
            const float segX = to.x - from.x;
            const float segZ = to.z - from.z;
            const float length = std::hypot(segX, segZ);
            const float dirX = segX / length;
            const float dirZ = segZ / length;
            SurfaceMesh& mesh = meshes[static_cast<size_t>(surface)];
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            for (const float station : stations)
            {
                const float along = std::clamp(station / length, 0.0f, 1.0f);
                const float halfWidth = from.width + (to.width - from.width) * along;
                const float y = from.y + (to.y - from.y) * along + kRoadLift;
                const float cx = from.x + dirX * station;
                const float cz = from.z + dirZ * station;
                // Right of the driving direction is (dirZ, -dirX) in the engine's +X-right convention.
                mesh.vertices.push_back({cx - dirZ * halfWidth, y, cz + dirX * halfWidth}); // left
                mesh.vertices.push_back({cx + dirZ * halfWidth, y, cz - dirX * halfWidth}); // right
            }
            for (uint32_t quad = 0; quad < 2; ++quad)
            {
                const uint32_t left0 = base + quad * 2;
                const uint32_t right0 = left0 + 1;
                const uint32_t left1 = left0 + 2;
                const uint32_t right1 = left0 + 3;
                // Counter-clockwise seen from above: the face normals point up, toward the wheel rays.
                mesh.indices.insert(mesh.indices.end(), {left0, left1, right0, right0, left1, right1});
            }
        };

        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minZ = minX;
        float maxZ = maxX;
        float minY = minX;
        for (uint32_t segment = 0; segment < segmentCount; ++segment)
        {
            const TrackWaypoint& from = GetWaypoint(segment);
            const TrackWaypoint& to = GetWaypoint(segment + 1);
            const float length = std::hypot(to.x - from.x, to.z - from.z);
            if (length <= 0.0f)
                continue;
            const float capStart = -from.width;
            const float capEnd = length + to.width;
            addStrip(from.surface, from, to, {capStart, 0.0f, length * 0.5f});
            addStrip(to.surface, from, to, {length * 0.5f, length, capEnd});

            const float reach = std::max(from.width, to.width);
            minX = std::min({minX, from.x - reach, to.x - reach});
            maxX = std::max({maxX, from.x + reach, to.x + reach});
            minZ = std::min({minZ, from.z - reach, to.z - reach});
            maxZ = std::max({maxZ, from.z + reach, to.z + reach});
            minY = std::min({minY, from.y, to.y});
        }

        for (size_t surface = 0; surface < meshes.size(); ++surface)
        {
            SurfaceMesh& mesh = meshes[surface];
            if (mesh.indices.empty())
                continue;
            PhysicsBodyDesc road;
            road.name = "Racing_Road_" + std::to_string(surface);
            road.type = PhysicsBodyType::Static;
            road.mass = 0.0f;
            road.shape.type = CollisionShapeType::Mesh;
            road.shape.vertices = std::move(mesh.vertices);
            road.shape.indices = std::move(mesh.indices);
            road.material.friction = RacingVehicleSystem::GetSurfaceGrip(static_cast<SurfaceType>(surface));
            if (auto body = physics->CreateBody(road))
                m_colliders.push_back(std::move(body));
        }

        // Run-off slab under the whole layout: grass grip, top face level with the lowest waypoint.
        if (minX <= maxX)
        {
            PhysicsBodyDesc ground;
            ground.name = "Racing_RunOff";
            ground.type = PhysicsBodyType::Static;
            ground.mass = 0.0f;
            ground.shape.type = CollisionShapeType::Box;
            // Full extents: CreateBody's shape factory halves box dimensions for Jolt.
            ground.shape.dimensions = {(maxX - minX) + 2.0f * kRunOffMargin, 2.0f,
                                       (maxZ - minZ) + 2.0f * kRunOffMargin};
            ground.position = {(minX + maxX) * 0.5f, minY - 1.0f, (minZ + maxZ) * 0.5f};
            ground.material.friction = RacingVehicleSystem::GetSurfaceGrip(SurfaceType::Grass);
            if (auto body = physics->CreateBody(ground))
                m_colliders.push_back(std::move(body));
        }
        physics->OptimizeBroadPhase();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing track '%s': built %zu static colliders",
                       m_currentTrack.name.c_str(), m_colliders.size());
    }

    void RacingTrackSystem::RemoveTrackColliders()
    {
        if (m_colliderPhysics)
        {
            for (const std::shared_ptr<PhysicsBody>& body : m_colliders)
                m_colliderPhysics->RemoveBody(body);
        }
        m_colliders.clear();
        m_colliderPhysics = nullptr;
    }

} // namespace Racing
