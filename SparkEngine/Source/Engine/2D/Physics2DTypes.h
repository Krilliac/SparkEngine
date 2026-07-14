/**
 * @file Physics2DTypes.h
 * @brief Type definitions, structs, and collision functions for 2D physics
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains Vec2, AABB2D, ContactPoint2D, PhysicsBody2D, and narrowphase
 * collision test functions used by Physics2DWorld and other 2D subsystems.
 */

#pragma once

#include "../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif
#include <functional>
#include <cmath>
#include <algorithm>
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
        Vec2 min; ///< Lower-left corner of the bounding box.
        Vec2 max; ///< Upper-right corner of the bounding box.

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
    // Physics body definition
    // =========================================================================

    /// @brief A 2D rigid body with shape, mass, and simulation properties.
    struct PhysicsBody2D
    {
        uint32_t entityID = 0;           ///< ECS entity this body belongs to.
        Vec2 position;                   ///< World-space center position.
        float rotation = 0.0f;           ///< Rotation angle (radians).
        Vec2 velocity;                   ///< Linear velocity (units/s).
        float angularVelocity = 0.0f;    ///< Angular velocity (radians/s).
        float mass = 1.0f;               ///< Mass (kg). 0 for static bodies.
        float invMass = 1.0f;            ///< Precomputed 1/mass (0 for static).
        float inertia = 1.0f;            ///< Rotational inertia.
        float invInertia = 1.0f;         ///< Precomputed 1/inertia.
        float friction = 0.3f;           ///< Surface friction coefficient [0, 1].
        float restitution = 0.0f;        ///< Bounciness [0, 1] (0 = no bounce).
        float linearDamping = 0.0f;      ///< Linear velocity decay per second.
        float angularDamping = 0.05f;    ///< Angular velocity decay per second.
        float gravityScale = 1.0f;       ///< Gravity multiplier (0 = no gravity).
        bool isStatic = false;           ///< True for immovable bodies (walls, floors).
        bool isKinematic = false;        ///< True for script-driven bodies (platforms).
        bool fixedRotation = false;      ///< Prevent rotation from physics forces.
        bool isBullet = false;           ///< Use CCD to prevent tunneling.
        bool isTrigger = false;          ///< Detect overlaps without physical response.
        AABB2D aabb;                     ///< Broadphase bounding box (updated each step).
        uint32_t layerMask = 0xFFFFFFFF; ///< Collision layer bitmask.

        enum class ShapeType
        {
            Box,   ///< Axis-aligned box defined by halfExtents.
            Circle ///< Circle defined by radius.
        };
        ShapeType shapeType = ShapeType::Box; ///< Collision shape type.
        Vec2 halfExtents{0.5f, 0.5f};         ///< Half-size for Box shapes.
        float radius = 0.5f;                  ///< Radius for Circle shapes.
    };

} // namespace Spark::Physics2D
