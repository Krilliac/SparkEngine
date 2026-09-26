/**
 * @file RacingTrackColliders.cpp
 * @brief RacingTrackSystem's static Jolt bodies: per-surface road meshes along the authored centerline, a run-off
 *        slab under the layout, barrier walls on the outside of the bends, and a sensor gate per checkpoint whose
 *        trigger contacts drive lap validation. Split from RacingTrackSystem.cpp (same class, feature-owned
 *        translation unit).
 */

#include "RacingTrackSystem.h"
#include "Physics/PhysicsBody.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/LogMacros.h"
#include "Vehicle/RacingVehicleSystem.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
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
        BuildBarriers(*physics);
        BuildCheckpointGates(*physics);
        physics->OptimizeBroadPhase();

        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "Racing track '%s': built %zu static colliders (%zu barrier pieces) and %zu checkpoint gates",
                       m_currentTrack.name.c_str(), m_colliders.size(), m_barrierCount, m_checkpointGates.size());
    }

    void RacingTrackSystem::BuildBarriers(PhysicsSystem& physics)
    {
        constexpr float kBarrierHeight = 1.2f; // above the road surface
        constexpr float kBarrierSink = 1.0f;   // below it, so a sloped piece leaves no gap under its low end
        constexpr float kBarrierThickness = 1.0f;
        constexpr float kMinBendRadians = 0.035f; // ~2 degrees: a straighter waypoint has no outside to guard
        constexpr float kRoadMargin = 0.5f;       // a piece within this of any road edge would stand on the road
        constexpr float kSampleSpacing = 2.0f;
        constexpr float kHalfPi = 1.57079633f;

        const uint32_t segmentCount = GetSegmentCount();
        const auto waypointCount = static_cast<uint32_t>(m_currentTrack.waypoints.size());
        const bool closed = m_currentTrack.layout != TrackLayout::PointToPoint;

        // Signed bend at a waypoint in radians: > 0 turns left, so the outside is on the right. A point-to-point
        // start or finish has no bend.
        auto bendAt = [&](uint32_t waypoint)
        {
            if (!closed && (waypoint == 0 || waypoint + 1 >= waypointCount))
                return 0.0f;
            const TrackWaypoint& previous = GetWaypoint(waypoint + waypointCount - 1);
            const TrackWaypoint& here = GetWaypoint(waypoint);
            const TrackWaypoint& next = GetWaypoint(waypoint + 1);
            const float inX = here.x - previous.x;
            const float inZ = here.z - previous.z;
            const float outX = next.x - here.x;
            const float outZ = next.z - here.z;
            return std::atan2(inX * outZ - inZ * outX, inX * outX + inZ * outZ);
        };

        // True when (x, z) lies on the road of any segment (or within kRoadMargin of its edge).
        auto onRoad = [&](float x, float z)
        {
            for (uint32_t segment = 0; segment < segmentCount; ++segment)
            {
                const TrackWaypoint& from = GetWaypoint(segment);
                const TrackWaypoint& to = GetWaypoint(segment + 1);
                const float segX = to.x - from.x;
                const float segZ = to.z - from.z;
                const float lengthSq = segX * segX + segZ * segZ;
                if (lengthSq <= 0.0f)
                    continue;
                const float t = std::clamp(((x - from.x) * segX + (z - from.z) * segZ) / lengthSq, 0.0f, 1.0f);
                const float halfWidth = from.width + (to.width - from.width) * t;
                if (std::hypot(x - (from.x + segX * t), z - (from.z + segZ * t)) < halfWidth + kRoadMargin)
                    return true;
            }
            return false;
        };

        for (uint32_t segment = 0; segment < segmentCount; ++segment)
        {
            const TrackWaypoint& from = GetWaypoint(segment);
            const TrackWaypoint& to = GetWaypoint(segment + 1);
            const float length = std::hypot(to.x - from.x, to.z - from.z);
            if (length <= 0.0f)
                continue;
            const float dirX = (to.x - from.x) / length;
            const float dirZ = (to.z - from.z) / length;
            const float roadHalfWidth = std::max(from.width, to.width);
            const float innerFace = roadHalfWidth + kBarrierClearance;

            // One piece per half segment; each half guards the outside of the bend at its own waypoint (the same
            // split GetSurfaceAt and the road strips use).
            for (int half = 0; half < 2; ++half)
            {
                const float bend = bendAt(half == 0 ? segment : (segment + 1) % waypointCount);
                if (std::fabs(bend) < kMinBendRadians)
                    continue;
                const float side = bend > 0.0f ? 1.0f : -1.0f; // +1 = right of the driving direction

                // Run the piece past its waypoint far enough to meet the next segment's outside wall at the apex.
                const float apexReach =
                    innerFace * std::tan(std::min(std::fabs(bend), kHalfPi) * 0.5f) + kBarrierThickness;
                const float start = half == 0 ? -apexReach : length * 0.5f;
                const float end = half == 0 ? length * 0.5f : length + apexReach;

                // Leave the piece out where it would stand on another stretch of road (a figure-8 crossing).
                bool blocksRoad = false;
                const auto intervals = static_cast<int>(std::ceil((end - start) / kSampleSpacing));
                for (int sample = 0; sample <= intervals && !blocksRoad; ++sample)
                {
                    const float along =
                        start + (end - start) * static_cast<float>(sample) / static_cast<float>(intervals);
                    for (const float face : {innerFace, innerFace + kBarrierThickness})
                    {
                        const float lateral = side * face;
                        if (onRoad(from.x + dirX * along + dirZ * lateral, from.z + dirZ * along - dirX * lateral))
                            blocksRoad = true;
                    }
                }
                if (blocksRoad)
                    continue;

                auto heightAt = [&](float station)
                { return from.y + (to.y - from.y) * std::clamp(station / length, 0.0f, 1.0f); };
                const float bottom = std::min(heightAt(start), heightAt(end)) - kBarrierSink;
                const float top = std::max(heightAt(start), heightAt(end)) + kBarrierHeight;
                const float middle = (start + end) * 0.5f;
                const float lateral = side * (innerFace + kBarrierThickness * 0.5f);

                PhysicsBodyDesc barrier;
                barrier.name = "Racing_Barrier_" + std::to_string(m_barrierCount);
                barrier.type = PhysicsBodyType::Static;
                barrier.mass = 0.0f;
                barrier.shape.type = CollisionShapeType::Box;
                // Full extents (the shape factory halves them): across, up, along the driving direction.
                barrier.shape.dimensions = {kBarrierThickness, top - bottom, end - start};
                barrier.position = {from.x + dirX * middle + dirZ * lateral, (top + bottom) * 0.5f,
                                    from.z + dirZ * middle - dirX * lateral};
                barrier.rotation = {0.0f, std::atan2(dirX, dirZ), 0.0f}; // radians; local +Z runs along the road
                barrier.material.friction = 0.3f;                        // a car that touches the wall scrapes along it
                barrier.material.restitution = 0.1f;
                if (auto body = physics.CreateBody(barrier))
                {
                    m_colliders.push_back(std::move(body));
                    ++m_barrierCount;
                }
            }
        }
    }

    void RacingTrackSystem::BuildCheckpointGates(PhysicsSystem& physics)
    {
        constexpr float kGateHeight = 6.0f;
        constexpr float kGateSink = 1.0f; // below the road surface

        const auto waypointCount = static_cast<uint32_t>(m_currentTrack.waypoints.size());
        const bool closed = m_currentTrack.layout != TrackLayout::PointToPoint;
        for (size_t index = 0; index < m_currentTrack.checkpoints.size(); ++index)
        {
            const Checkpoint& checkpoint = m_currentTrack.checkpoints[index];

            // Checkpoints are authored on waypoints: square the gate to the centerline through the nearest one.
            const uint32_t waypoint = GetNearestWaypoint(checkpoint.x, checkpoint.z);
            const TrackWaypoint& here = GetWaypoint(waypoint);
            const TrackWaypoint& previous = (closed || waypoint > 0) ? GetWaypoint(waypoint + waypointCount - 1) : here;
            const TrackWaypoint& next = (closed || waypoint + 1 < waypointCount) ? GetWaypoint(waypoint + 1) : here;

            PhysicsBodyDesc gate;
            gate.name = "Racing_CheckpointGate_" + std::to_string(index);
            gate.type = PhysicsBodyType::Static;
            gate.mass = 0.0f;
            gate.isTrigger = true;
            gate.collisionGroup = CollisionLayers::Trigger;
            gate.shape.type = CollisionShapeType::Box;
            // Full extents: across the road out to both barrier lines, up, and kGateDepth along the road.
            gate.shape.dimensions = {2.0f * (here.width + kBarrierClearance), kGateHeight, kGateDepth};
            gate.position = {checkpoint.x, checkpoint.y + kGateHeight * 0.5f - kGateSink, checkpoint.z};
            gate.rotation = {0.0f, std::atan2(next.x - previous.x, next.z - previous.z), 0.0f}; // radians
            std::shared_ptr<PhysicsBody> body = physics.CreateBody(gate);
            if (!body)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "Racing track '%s': checkpoint %zu has no sensor gate; no lap can be completed",
                                m_currentTrack.name.c_str(), index);
                continue;
            }
            m_checkpointGates.push_back({std::move(body), static_cast<uint32_t>(index)});
        }

        if (!m_checkpointGates.empty())
        {
            physics.SetTriggerCallback([this](PhysicsBody* first, PhysicsBody* second, bool entered)
                                       { OnTriggerContact(first, second, entered); });
        }
    }

    void RacingTrackSystem::OnTriggerContact(const PhysicsBody* first, const PhysicsBody* second, bool entered)
    {
        // Only entries count. An exit carries no lap information and can name a body that was already removed.
        if (!entered || !first || !second)
            return;

        for (const CheckpointGate& gate : m_checkpointGates)
        {
            const PhysicsBody* other = nullptr;
            if (gate.body.get() == first)
                other = second;
            else if (gate.body.get() == second)
                other = first;
            else
                continue;

            // RacingVehicleSystem tags each chassis body with its vehicle ID; untagged bodies are not racers.
            if (other->GetEntityID() != 0)
                m_pendingCrossings.push_back({other->GetEntityID(), gate.checkpointIndex});
            return;
        }
    }

    std::vector<CheckpointCrossing> RacingTrackSystem::TakeCheckpointCrossings()
    {
        std::vector<CheckpointCrossing> crossings;
        crossings.swap(m_pendingCrossings);
        return crossings;
    }

    void RacingTrackSystem::RemoveTrackColliders()
    {
        if (m_colliderPhysics)
        {
            if (!m_checkpointGates.empty())
                m_colliderPhysics->SetTriggerCallback(nullptr);
            for (const CheckpointGate& gate : m_checkpointGates)
                m_colliderPhysics->RemoveBody(gate.body);
            for (const std::shared_ptr<PhysicsBody>& body : m_colliders)
                m_colliderPhysics->RemoveBody(body);
        }
        m_checkpointGates.clear();
        m_pendingCrossings.clear();
        m_colliders.clear();
        m_barrierCount = 0;
        m_colliderPhysics = nullptr;
    }

} // namespace Racing
