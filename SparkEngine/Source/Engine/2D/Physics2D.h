/**
 * @file Physics2D.h
 * @brief 2D physics simulation with AABB/circle collision detection and resolution
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a self-contained 2D physics world with broadphase spatial hashing,
 * narrowphase SAT/circle collision, and iterative impulse resolution. Designed
 * to work without external physics libraries for simple 2D games while
 * integrating with Bullet Physics via the EngineContext for hybrid 2D/3D scenes.
 */

#pragma once

#include "Physics2DTypes.h"
#include <vector>
#include <unordered_map>

namespace Spark::Physics2D
{

    // =========================================================================
    // Spatial hash grid for broadphase
    // =========================================================================

    class SpatialHash2D
    {
      public:
        explicit SpatialHash2D(float cellSize = 2.0f) : m_cellSize(cellSize), m_invCellSize(1.0f / cellSize) {}

        void Clear() { m_cells.clear(); }

        void Insert(uint32_t id, const AABB2D& aabb)
        {
            int minX = static_cast<int>(std::floor(aabb.min.x * m_invCellSize));
            int minY = static_cast<int>(std::floor(aabb.min.y * m_invCellSize));
            int maxX = static_cast<int>(std::floor(aabb.max.x * m_invCellSize));
            int maxY = static_cast<int>(std::floor(aabb.max.y * m_invCellSize));

            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    uint64_t key = CellKey(x, y);
                    m_cells[key].push_back(id);
                }
            }
        }

        /// Query all IDs that potentially overlap the given AABB
        std::vector<uint32_t> Query(const AABB2D& aabb) const
        {
            std::vector<uint32_t> results;
            int minX = static_cast<int>(std::floor(aabb.min.x * m_invCellSize));
            int minY = static_cast<int>(std::floor(aabb.min.y * m_invCellSize));
            int maxX = static_cast<int>(std::floor(aabb.max.x * m_invCellSize));
            int maxY = static_cast<int>(std::floor(aabb.max.y * m_invCellSize));

            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    auto it = m_cells.find(CellKey(x, y));
                    if (it != m_cells.end())
                    {
                        results.insert(results.end(), it->second.begin(), it->second.end());
                    }
                }
            }
            // Remove duplicates
            std::sort(results.begin(), results.end());
            results.erase(std::unique(results.begin(), results.end()), results.end());
            return results;
        }

        float GetCellSize() const { return m_cellSize; }

      private:
        static uint64_t CellKey(int x, int y)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
        }

        float m_cellSize;
        float m_invCellSize;
        std::unordered_map<uint64_t, std::vector<uint32_t>> m_cells;
    };

    // =========================================================================
    // Physics2DWorld — simulation driver
    // =========================================================================

    class Physics2DWorld
    {
      public:
        Physics2DWorld() = default;

        /// Set gravity (default: 0, -9.81)
        void SetGravity(float x, float y) { m_gravity = {x, y}; }
        Vec2 GetGravity() const { return m_gravity; }

        /// Set velocity iterations for constraint solving
        void SetVelocityIterations(int iterations) { m_velocityIterations = iterations; }

        /// Step the physics simulation
        void Step(float deltaTime)
        {
            if (deltaTime <= 0.0f)
                return;

            // Integrate velocities (gravity + damping)
            for (auto& body : m_bodies)
            {
                if (body.isStatic || body.isKinematic)
                    continue;

                body.velocity += m_gravity * body.gravityScale * deltaTime;
                body.velocity *= (1.0f / (1.0f + body.linearDamping * deltaTime));
                body.angularVelocity *= (1.0f / (1.0f + body.angularDamping * deltaTime));
            }

            // Broadphase
            m_spatialHash.Clear();
            for (size_t i = 0; i < m_bodies.size(); ++i)
            {
                UpdateAABB(m_bodies[i]);
                m_spatialHash.Insert(static_cast<uint32_t>(i), m_bodies[i].aabb);
            }

            // Narrowphase: detect contacts, apply positional correction once,
            // and record a velocity constraint per non-trigger contact.
            m_contacts.clear();
            struct VelocityConstraint2D
            {
                size_t a;
                size_t b;
                Vec2 normal;
            };
            std::vector<VelocityConstraint2D> constraints;
            for (size_t i = 0; i < m_bodies.size(); ++i)
            {
                auto candidates = m_spatialHash.Query(m_bodies[i].aabb);
                for (uint32_t j : candidates)
                {
                    if (j <= i)
                        continue;

                    // Layer mask check
                    if ((m_bodies[i].layerMask & m_bodies[j].layerMask) == 0)
                        continue;

                    // Skip static-static pairs
                    if (m_bodies[i].isStatic && m_bodies[j].isStatic)
                        continue;

                    Vec2 normal;
                    float depth;
                    bool colliding = false;

                    if (m_bodies[i].shapeType == PhysicsBody2D::ShapeType::Box &&
                        m_bodies[j].shapeType == PhysicsBody2D::ShapeType::Box)
                    {
                        colliding = TestAABBvsAABB(m_bodies[i].aabb, m_bodies[j].aabb, normal, depth);
                    }
                    else if (m_bodies[i].shapeType == PhysicsBody2D::ShapeType::Circle &&
                             m_bodies[j].shapeType == PhysicsBody2D::ShapeType::Circle)
                    {
                        colliding = TestCirclevsCircle(m_bodies[i].position, m_bodies[i].radius, m_bodies[j].position,
                                                       m_bodies[j].radius, normal, depth);
                    }
                    else
                    {
                        // Box vs Circle
                        size_t boxIdx = (m_bodies[i].shapeType == PhysicsBody2D::ShapeType::Box) ? i : j;
                        size_t circIdx = (m_bodies[i].shapeType == PhysicsBody2D::ShapeType::Circle) ? i : j;
                        colliding = TestAABBvsCircle(m_bodies[boxIdx].aabb, m_bodies[circIdx].position,
                                                     m_bodies[circIdx].radius, normal, depth);
                        if (boxIdx == j)
                            normal = normal * -1.0f; // Ensure normal points from i to j
                    }

                    if (colliding)
                    {
                        ContactPoint2D contact;
                        contact.normal = normal;
                        contact.depth = depth;
                        contact.bodyA = m_bodies[i].entityID;
                        contact.bodyB = m_bodies[j].entityID;
                        contact.point = m_bodies[i].position + normal * (depth * 0.5f);
                        contact.isTrigger = m_bodies[i].isTrigger || m_bodies[j].isTrigger;

                        m_contacts.push_back(contact);

                        if (!contact.isTrigger)
                        {
                            // Positional correction runs once per contact; the
                            // velocity impulse is iterated below for stability.
                            ResolvePosition(m_bodies[i], m_bodies[j], normal, depth);
                            constraints.push_back({i, j, normal});
                        }

                        // Fire callbacks
                        if (m_onCollision)
                            m_onCollision(contact);
                    }
                }
            }

            // Velocity solver: iterate the impulse resolution so stacked and
            // resting bodies settle instead of sinking under a single pass.
            const int iterations = m_velocityIterations > 0 ? m_velocityIterations : 1;
            for (int iter = 0; iter < iterations; ++iter)
            {
                for (const auto& c : constraints)
                {
                    ResolveVelocity(m_bodies[c.a], m_bodies[c.b], c.normal);
                }
            }

            // Integrate positions
            for (auto& body : m_bodies)
            {
                if (body.isStatic || body.isKinematic)
                    continue;

                body.position += body.velocity * deltaTime;
                if (!body.fixedRotation)
                    body.rotation += body.angularVelocity * deltaTime;
            }
        }

        /// Add a body to the world
        size_t AddBody(const PhysicsBody2D& body)
        {
            m_bodies.push_back(body);
            auto& b = m_bodies.back();
            if (b.mass <= 0.0f || b.isStatic || b.isKinematic)
            {
                b.invMass = 0.0f;
                b.invInertia = 0.0f;
            }
            else
            {
                b.invMass = 1.0f / b.mass;
                b.inertia = b.mass * 0.5f; // Simplified moment of inertia
                b.invInertia = 1.0f / b.inertia;
            }
            return m_bodies.size() - 1;
        }

        /// Remove all bodies
        void Clear()
        {
            m_bodies.clear();
            m_contacts.clear();
        }

        /// Get contacts from the last step
        const std::vector<ContactPoint2D>& GetContacts() const { return m_contacts; }

        /// Get all bodies
        std::vector<PhysicsBody2D>& GetBodies() { return m_bodies; }
        const std::vector<PhysicsBody2D>& GetBodies() const { return m_bodies; }

        /// Find body by entity ID
        PhysicsBody2D* FindBody(uint32_t entityID)
        {
            for (auto& body : m_bodies)
            {
                if (body.entityID == entityID)
                    return &body;
            }
            return nullptr;
        }

        /// Set collision callback
        void SetCollisionCallback(CollisionCallback callback) { m_onCollision = std::move(callback); }

        /// Raycast against all bodies
        struct RaycastHit2D
        {
            uint32_t entityID = 0;
            Vec2 point;
            Vec2 normal;
            float distance = 0.0f;
        };

        bool Raycast(const Vec2& origin, const Vec2& direction, float maxDistance, RaycastHit2D& hit,
                     uint32_t layerMask = 0xFFFFFFFF) const
        {
            float closestDist = maxDistance;
            bool found = false;
            Vec2 dir = direction.Normalized();

            for (const auto& body : m_bodies)
            {
                if ((body.layerMask & layerMask) == 0)
                    continue;

                // Simple ray-AABB intersection
                float tMin = 0.0f;
                float tMax = closestDist;

                for (int axis = 0; axis < 2; ++axis)
                {
                    float o = (axis == 0) ? origin.x : origin.y;
                    float d = (axis == 0) ? dir.x : dir.y;
                    float bMin = (axis == 0) ? body.aabb.min.x : body.aabb.min.y;
                    float bMax = (axis == 0) ? body.aabb.max.x : body.aabb.max.y;

                    if (std::abs(d) < 1e-6f)
                    {
                        if (o < bMin || o > bMax)
                        {
                            tMin = maxDistance + 1.0f;
                            break;
                        }
                    }
                    else
                    {
                        float t1 = (bMin - o) / d;
                        float t2 = (bMax - o) / d;
                        if (t1 > t2)
                            std::swap(t1, t2);
                        tMin = (std::max)(tMin, t1);
                        tMax = (std::min)(tMax, t2);
                        if (tMin > tMax)
                            break;
                    }
                }

                if (tMin <= tMax && tMin < closestDist && tMin >= 0.0f)
                {
                    closestDist = tMin;
                    hit.entityID = body.entityID;
                    hit.point = origin + dir * tMin;
                    hit.distance = tMin;
                    // Approximate normal from AABB face
                    Vec2 center = body.aabb.Center();
                    Vec2 diff = hit.point - center;
                    Vec2 half = body.aabb.HalfSize();
                    float biasX = (half.x > 1e-6f) ? diff.x / half.x : 0.0f;
                    float biasY = (half.y > 1e-6f) ? diff.y / half.y : 0.0f;
                    if (std::abs(biasX) > std::abs(biasY))
                        hit.normal = {biasX > 0 ? 1.0f : -1.0f, 0.0f};
                    else
                        hit.normal = {0.0f, biasY > 0 ? 1.0f : -1.0f};
                    found = true;
                }
            }
            return found;
        }

      private:
        void UpdateAABB(PhysicsBody2D& body)
        {
            if (body.shapeType == PhysicsBody2D::ShapeType::Circle)
            {
                body.aabb.min = {body.position.x - body.radius, body.position.y - body.radius};
                body.aabb.max = {body.position.x + body.radius, body.position.y + body.radius};
            }
            else
            {
                body.aabb.min = {body.position.x - body.halfExtents.x, body.position.y - body.halfExtents.y};
                body.aabb.max = {body.position.x + body.halfExtents.x, body.position.y + body.halfExtents.y};
            }
        }

        // Positional correction (prevent sinking). Applied once per contact.
        void ResolvePosition(PhysicsBody2D& a, PhysicsBody2D& b, const Vec2& normal, float depth)
        {
            float totalInvMass = a.invMass + b.invMass;
            if (totalInvMass <= 0.0f)
                return;

            constexpr float kSlop = 0.01f;
            constexpr float kCorrectionPercent = 0.8f;
            float correctionMag = (std::max)(depth - kSlop, 0.0f) * kCorrectionPercent / totalInvMass;
            Vec2 correction = normal * correctionMag;

            a.position -= correction * a.invMass;
            b.position += correction * b.invMass;
        }

        // Velocity impulse (with friction). Re-applied each solver iteration
        // using the current velocities, so it must not touch positions.
        void ResolveVelocity(PhysicsBody2D& a, PhysicsBody2D& b, const Vec2& normal)
        {
            float totalInvMass = a.invMass + b.invMass;
            if (totalInvMass <= 0.0f)
                return;

            // Impulse resolution
            Vec2 relVel = b.velocity - a.velocity;
            float velAlongNormal = relVel.Dot(normal);

            // Don't resolve if separating
            if (velAlongNormal > 0.0f)
                return;

            float e = (std::min)(a.restitution, b.restitution);
            float j = -(1.0f + e) * velAlongNormal / totalInvMass;

            Vec2 impulse = normal * j;
            a.velocity -= impulse * a.invMass;
            b.velocity += impulse * b.invMass;

            // Friction impulse
            Vec2 tangent = relVel - normal * velAlongNormal;
            float tangentLen = tangent.Length();
            if (tangentLen > 1e-6f)
            {
                tangent = tangent * (1.0f / tangentLen);
                float jt = -relVel.Dot(tangent) / totalInvMass;
                float mu = std::sqrt(a.friction * b.friction);

                Vec2 frictionImpulse;
                if (std::abs(jt) < j * mu)
                    frictionImpulse = tangent * jt;
                else
                    frictionImpulse = tangent * (-j * mu);

                a.velocity -= frictionImpulse * a.invMass;
                b.velocity += frictionImpulse * b.invMass;
            }
        }

        Vec2 m_gravity{0.0f, -9.81f};
        int m_velocityIterations = 8;
        std::vector<PhysicsBody2D> m_bodies;
        std::vector<ContactPoint2D> m_contacts;
        SpatialHash2D m_spatialHash{2.0f};
        CollisionCallback m_onCollision;
    };

} // namespace Spark::Physics2D
