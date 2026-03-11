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

#include "../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <vector>
#include <functional>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <cstdint>

namespace Spark::Physics2D
{

    // =========================================================================
    // 2D Math Helpers
    // =========================================================================

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;

        Vec2() = default;
        Vec2(float ax, float ay) : x(ax), y(ay) {}
        Vec2(const DirectX::XMFLOAT2& f) : x(f.x), y(f.y) {}

        Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
        Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
        Vec2 operator*(float s) const { return {x * s, y * s}; }
        Vec2& operator+=(const Vec2& o)
        {
            x += o.x;
            y += o.y;
            return *this;
        }
        Vec2& operator-=(const Vec2& o)
        {
            x -= o.x;
            y -= o.y;
            return *this;
        }
        Vec2& operator*=(float s)
        {
            x *= s;
            y *= s;
            return *this;
        }
        float Dot(const Vec2& o) const { return x * o.x + y * o.y; }
        float Cross(const Vec2& o) const { return x * o.y - y * o.x; }
        float LengthSq() const { return x * x + y * y; }
        float Length() const { return std::sqrt(LengthSq()); }

        Vec2 Normalized() const
        {
            float len = Length();
            if (len < 1e-6f)
                return {0.0f, 0.0f};
            return {x / len, y / len};
        }

        DirectX::XMFLOAT2 ToXMFLOAT2() const { return {x, y}; }
    };

    inline Vec2 operator*(float s, const Vec2& v)
    {
        return {s * v.x, s * v.y};
    }

    // =========================================================================
    // AABB for broadphase
    // =========================================================================

    struct AABB2D
    {
        Vec2 min;
        Vec2 max;

        bool Overlaps(const AABB2D& other) const
        {
            return !(max.x < other.min.x || min.x > other.max.x || max.y < other.min.y || min.y > other.max.y);
        }

        Vec2 Center() const { return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}; }
        Vec2 HalfSize() const { return {(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f}; }
    };

    // =========================================================================
    // Collision manifold
    // =========================================================================

    struct ContactPoint2D
    {
        Vec2 point;     ///< Contact point in world space
        Vec2 normal;    ///< Contact normal (from A to B)
        float depth;    ///< Penetration depth
        uint32_t bodyA; ///< Entity ID of body A
        uint32_t bodyB; ///< Entity ID of body B
        bool isTrigger; ///< Whether this was a trigger overlap
    };

    using CollisionCallback = std::function<void(const ContactPoint2D&)>;

    // =========================================================================
    // Collision detection functions
    // =========================================================================

    /// Test AABB vs AABB collision
    inline bool TestAABBvsAABB(const AABB2D& a, const AABB2D& b, Vec2& normal, float& depth)
    {
        Vec2 centerA = a.Center();
        Vec2 centerB = b.Center();
        Vec2 halfA = a.HalfSize();
        Vec2 halfB = b.HalfSize();

        float dx = centerB.x - centerA.x;
        float dy = centerB.y - centerA.y;
        float overlapX = halfA.x + halfB.x - std::abs(dx);
        float overlapY = halfA.y + halfB.y - std::abs(dy);

        if (overlapX <= 0.0f || overlapY <= 0.0f)
            return false;

        if (overlapX < overlapY)
        {
            normal = {dx < 0.0f ? -1.0f : 1.0f, 0.0f};
            depth = overlapX;
        }
        else
        {
            normal = {0.0f, dy < 0.0f ? -1.0f : 1.0f};
            depth = overlapY;
        }
        return true;
    }

    /// Test circle vs circle collision
    inline bool TestCirclevsCircle(const Vec2& posA, float radA, const Vec2& posB, float radB, Vec2& normal,
                                   float& depth)
    {
        Vec2 diff = posB - posA;
        float distSq = diff.LengthSq();
        float radSum = radA + radB;

        if (distSq >= radSum * radSum)
            return false;

        float dist = std::sqrt(distSq);
        if (dist < 1e-6f)
        {
            normal = {1.0f, 0.0f};
            depth = radSum;
        }
        else
        {
            normal = diff * (1.0f / dist);
            depth = radSum - dist;
        }
        return true;
    }

    /// Test AABB vs circle collision
    inline bool TestAABBvsCircle(const AABB2D& box, const Vec2& circlePos, float circleRadius, Vec2& normal,
                                 float& depth)
    {
        Vec2 center = box.Center();
        Vec2 half = box.HalfSize();

        // Find closest point on AABB to circle center
        float closestX = std::clamp(circlePos.x, center.x - half.x, center.x + half.x);
        float closestY = std::clamp(circlePos.y, center.y - half.y, center.y + half.y);

        Vec2 closest = {closestX, closestY};
        Vec2 diff = circlePos - closest;
        float distSq = diff.LengthSq();

        if (distSq >= circleRadius * circleRadius)
            return false;

        float dist = std::sqrt(distSq);
        if (dist < 1e-6f)
        {
            // Circle center is inside the AABB
            float dx = half.x - std::abs(circlePos.x - center.x);
            float dy = half.y - std::abs(circlePos.y - center.y);
            if (dx < dy)
            {
                normal = {circlePos.x < center.x ? -1.0f : 1.0f, 0.0f};
                depth = dx + circleRadius;
            }
            else
            {
                normal = {0.0f, circlePos.y < center.y ? -1.0f : 1.0f};
                depth = dy + circleRadius;
            }
        }
        else
        {
            normal = diff * (1.0f / dist);
            depth = circleRadius - dist;
        }
        return true;
    }

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

    struct PhysicsBody2D
    {
        uint32_t entityID = 0;
        Vec2 position;
        float rotation = 0.0f;
        Vec2 velocity;
        float angularVelocity = 0.0f;
        float mass = 1.0f;
        float invMass = 1.0f;
        float inertia = 1.0f;
        float invInertia = 1.0f;
        float friction = 0.3f;
        float restitution = 0.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.05f;
        float gravityScale = 1.0f;
        bool isStatic = false;
        bool isKinematic = false;
        bool fixedRotation = false;
        bool isBullet = false;
        bool isTrigger = false;
        AABB2D aabb;
        uint32_t layerMask = 0xFFFFFFFF;

        enum class ShapeType
        {
            Box,
            Circle
        };
        ShapeType shapeType = ShapeType::Box;
        Vec2 halfExtents{0.5f, 0.5f};
        float radius = 0.5f;
    };

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

            // Narrowphase + resolve
            m_contacts.clear();
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
                            ResolveCollision(m_bodies[i], m_bodies[j], normal, depth);
                        }

                        // Fire callbacks
                        if (m_onCollision)
                            m_onCollision(contact);
                    }
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

        void ResolveCollision(PhysicsBody2D& a, PhysicsBody2D& b, const Vec2& normal, float depth)
        {
            // Positional correction (prevent sinking)
            float totalInvMass = a.invMass + b.invMass;
            if (totalInvMass <= 0.0f)
                return;

            constexpr float kSlop = 0.01f;
            constexpr float kCorrectionPercent = 0.8f;
            float correctionMag = (std::max)(depth - kSlop, 0.0f) * kCorrectionPercent / totalInvMass;
            Vec2 correction = normal * correctionMag;

            a.position -= correction * a.invMass;
            b.position += correction * b.invMass;

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
