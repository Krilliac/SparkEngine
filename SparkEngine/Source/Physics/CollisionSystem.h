/**
 * @file CollisionSystem.h
 * @brief Comprehensive 3D collision detection and physics system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides a complete collision detection system with primitive shapes,
 * raycast functionality, contact resolution, and utility functions for 3D physics
 * calculations. The system supports sphere, box, and ray-based collision queries
 * with detailed collision information and contact manifolds.
 */

#pragma once

#include "Utils/Assert.h"
#include "../Core/framework.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <cfloat>
#include <cmath>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief Axis-aligned bounding box for collision detection
 * 
 * Represents a 3D bounding box defined by minimum and maximum corner points.
 * Used for efficient collision detection and spatial partitioning.
 */
struct BoundingBox
{
    XMFLOAT3 Min; ///< Minimum corner of the bounding box
    XMFLOAT3 Max; ///< Maximum corner of the bounding box

    /**
     * @brief Default constructor creating a unit box centered at origin
     */
    BoundingBox() : Min(-1, -1, -1), Max(1, 1, 1) {}

    /**
     * @brief Constructor with explicit min/max corners
     * @param min Minimum corner point
     * @param max Maximum corner point
     */
    BoundingBox(const XMFLOAT3& min, const XMFLOAT3& max) : Min(min), Max(max) {}

    /**
     * @brief Get the center point of the bounding box
     * @return Center point coordinates
     */
    XMFLOAT3 GetCenter() const;

    /**
     * @brief Get the extents (half-sizes) of the bounding box
     * @return Extents from center to edges along each axis
     */
    XMFLOAT3 GetExtents() const;

    /**
     * @brief Transform the bounding box by a matrix
     * @param transform 4x4 transformation matrix to apply
     */
    void Transform(const XMMATRIX& transform);
};

/**
 * @brief Bounding sphere for collision detection
 * 
 * Represents a 3D sphere defined by center point and radius.
 * Often used for fast collision detection due to simple math.
 */
struct BoundingSphere
{
    XMFLOAT3 Center; ///< Center point of the sphere
    float Radius;    ///< Radius of the sphere

    /**
     * @brief Default constructor creating a unit sphere at origin
     */
    BoundingSphere() : Center(0, 0, 0), Radius(1) {}

    /**
     * @brief Constructor with explicit center and radius
     * @param c Center point of the sphere
     * @param r Radius of the sphere
     */
    BoundingSphere(const XMFLOAT3& c, float r) : Center(c), Radius(r) {}

    /**
     * @brief Transform the bounding sphere by a matrix
     * @param transform 4x4 transformation matrix to apply
     * @note Only uniform scaling is properly supported for spheres
     */
    void Transform(const XMMATRIX& transform);
};

/**
 * @brief 3D ray for raycasting operations
 * 
 * Represents a ray with an origin point and direction vector.
 * Used for collision detection, picking, and line-of-sight queries.
 */
struct Ray
{
    XMFLOAT3 Origin;    ///< Starting point of the ray
    XMFLOAT3 Direction; ///< Direction vector of the ray (should be normalized)

    /**
     * @brief Default constructor creating a ray along positive Z-axis
     */
    Ray() : Origin(0, 0, 0), Direction(0, 0, 1) {}

    /**
     * @brief Constructor with explicit origin and direction
     * @param o Origin point of the ray
     * @param d Direction vector of the ray
     */
    Ray(const XMFLOAT3& o, const XMFLOAT3& d) : Origin(o), Direction(d) {}

    /**
     * @brief Get a point along the ray at parameter t
     * @param t Distance parameter along the ray
     * @return Point at position Origin + t * Direction
     */
    XMFLOAT3 GetPoint(float t) const;
};

/**
 * @brief Contact manifold for collision resolution
 * 
 * Contains detailed information about a collision including contact points,
 * surface normal, and penetration depth. Used for physics response and
 * collision resolution calculations.
 */
struct ContactManifold
{
    XMFLOAT3 ContactPoints[4]; ///< Up to 4 contact points for stable collision resolution
    XMFLOAT3 Normal;           ///< Surface normal at the collision point
    float PenetrationDepth;    ///< How deep objects are interpenetrating
    int ContactCount;          ///< Number of valid contact points (0-4)

    /**
     * @brief Default constructor with safe initial values
     */
    ContactManifold() : Normal(0, 1, 0), PenetrationDepth(0), ContactCount(0)
    {
        for (int i = 0; i < 4; ++i)
            ContactPoints[i] = XMFLOAT3(0, 0, 0);
    }
};

/**
 * @brief Result of a raycast collision query
 * 
 * Contains all information about a ray-object intersection including
 * hit status, contact point, surface normal, distance, and optional user data.
 */
struct CollisionResult
{
    bool Hit;        ///< Whether the ray hit the object
    XMFLOAT3 Point;  ///< World position of the intersection point
    XMFLOAT3 Normal; ///< Surface normal at the intersection point
    float Distance;  ///< Distance from ray origin to intersection point
    void* UserData;  ///< Optional pointer to additional collision data

    /**
     * @brief Default constructor indicating no collision
     */
    CollisionResult() : Hit(false), Point(0, 0, 0), Normal(0, 1, 0), Distance(0), UserData(nullptr) {}
};

/**
 * @brief Static collision detection and physics utility class
 * 
 * The CollisionSystem provides a comprehensive set of static methods for
 * performing various types of collision detection queries. It supports
 * primitive-vs-primitive tests, ray casting, and utility functions for
 * 3D vector mathematics and geometric calculations.
 * 
 * Features include:
 * - Sphere-sphere, sphere-box, and box-box collision detection
 * - Ray-casting against spheres, boxes, planes, and triangles
 * - Contact manifold generation for physics response
 * - Point-in-shape queries for spatial testing
 * - Vector mathematics utilities for 3D calculations
 * 
 * @note All methods are static and thread-safe
 * @warning Ensure all input vectors are valid (no NaN or infinite values)
 */
class CollisionSystem
{
  public:
    /**
     * @brief Test collision between two spheres
     * @param a First bounding sphere
     * @param b Second bounding sphere
     * @return true if spheres are overlapping, false otherwise
     */
    static bool SphereVsSphere(const BoundingSphere& a, const BoundingSphere& b);

    /**
     * @brief Test collision between two spheres with contact manifold
     * @param a First bounding sphere
     * @param b Second bounding sphere
     * @param m Output contact manifold with collision details
     * @return true if spheres are overlapping, false otherwise
     */
    static bool SphereVsSphere(const BoundingSphere& a, const BoundingSphere& b, ContactManifold& m);

    /**
     * @brief Test collision between sphere and axis-aligned box
     * @param sphere Bounding sphere to test
     * @param box Bounding box to test
     * @return true if sphere and box are overlapping, false otherwise
     */
    static bool SphereVsBox(const BoundingSphere& sphere, const BoundingBox& box);

    /**
     * @brief Test collision between two axis-aligned boxes
     * @param a First bounding box
     * @param b Second bounding box
     * @return true if boxes are overlapping, false otherwise
     */
    static bool BoxVsBox(const BoundingBox& a, const BoundingBox& b);

    /**
     * @brief Cast a ray against a sphere
     * @param ray Ray to cast
     * @param sphere Sphere to test against
     * @return CollisionResult with hit information
     */
    static CollisionResult RayVsSphere(const Ray& ray, const BoundingSphere& sphere);

    /**
     * @brief Cast a ray against an axis-aligned box
     * @param ray Ray to cast
     * @param box Box to test against
     * @return CollisionResult with hit information
     */
    static CollisionResult RayVsBox(const Ray& ray, const BoundingBox& box);

    /**
     * @brief Cast a ray against an infinite plane
     * @param ray Ray to cast
     * @param planePoint Any point on the plane
     * @param planeNormal Normal vector of the plane (should be normalized)
     * @return CollisionResult with hit information
     */
    static CollisionResult RayVsPlane(const Ray& ray, const XMFLOAT3& planePoint, const XMFLOAT3& planeNormal);

    /**
     * @brief Cast a ray against a triangle
     * @param ray Ray to cast
     * @param v0 First vertex of the triangle
     * @param v1 Second vertex of the triangle
     * @param v2 Third vertex of the triangle
     * @return CollisionResult with hit information
     */
    static CollisionResult RayVsTriangle(const Ray& ray, const XMFLOAT3& v0, const XMFLOAT3& v1, const XMFLOAT3& v2);

    /**
     * @brief Find the closest point on a box to a given point
     * @param point Point to find closest position to
     * @param box Bounding box to find closest point on
     * @return Closest point on the box surface
     */
    static XMFLOAT3 ClosestPointOnBox(const XMFLOAT3& point, const BoundingBox& box);

    /**
     * @brief Find the closest point on a sphere to a given point
     * @param point Point to find closest position to
     * @param sphere Bounding sphere to find closest point on
     * @return Closest point on the sphere surface
     */
    static XMFLOAT3 ClosestPointOnSphere(const XMFLOAT3& point, const BoundingSphere& sphere);

    /**
     * @brief Calculate distance from a point to an infinite plane
     * @param point Point to measure distance from
     * @param planePoint Any point on the plane
     * @param planeNormal Normal vector of the plane (should be normalized)
     * @return Signed distance to the plane (positive = in front of normal)
     */
    static float DistancePointToPlane(const XMFLOAT3& point, const XMFLOAT3& planePoint, const XMFLOAT3& planeNormal);

    /**
     * @brief Test if a point is inside a sphere
     * @param point Point to test
     * @param sphere Sphere to test against
     * @return true if point is inside sphere, false otherwise
     */
    static bool PointInSphere(const XMFLOAT3& point, const BoundingSphere& sphere);

    /**
     * @brief Test if a point is inside an axis-aligned box
     * @param point Point to test
     * @param box Box to test against
     * @return true if point is inside box, false otherwise
     */
    static bool PointInBox(const XMFLOAT3& point, const BoundingBox& box);

    /**
     * @brief Calculate the length of a 3D vector
     * @param v Vector to calculate length of
     * @return Length (magnitude) of the vector
     */
    static float Vector3Length(const XMFLOAT3& v);

    /**
     * @brief Calculate the squared length of a 3D vector
     * @param v Vector to calculate squared length of
     * @return Squared length of the vector (avoids sqrt for performance)
     */
    static float Vector3LengthSquared(const XMFLOAT3& v);

    /**
     * @brief Normalize a 3D vector to unit length
     * @param v Vector to normalize
     * @return Normalized vector with length 1.0
     */
    static XMFLOAT3 Vector3Normalize(const XMFLOAT3& v);

    /**
     * @brief Calculate the dot product of two 3D vectors
     * @param a First vector
     * @param b Second vector
     * @return Dot product (scalar result)
     */
    static float Vector3Dot(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Calculate the cross product of two 3D vectors
     * @param a First vector
     * @param b Second vector
     * @return Cross product vector perpendicular to both inputs
     */
    static XMFLOAT3 Vector3Cross(const XMFLOAT3& a, const XMFLOAT3& b);

    /**
     * @brief Reflect a vector off a surface normal
     * @param i Incident vector
     * @param n Surface normal (should be normalized)
     * @return Reflected vector
     */
    static XMFLOAT3 Vector3Reflect(const XMFLOAT3& i, const XMFLOAT3& n);

    /**
     * @brief Linear interpolation between two 3D vectors
     * @param a Start vector
     * @param b End vector
     * @param t Interpolation parameter (0.0 = a, 1.0 = b)
     * @return Interpolated vector
     */
    static XMFLOAT3 Vector3Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t);
};
