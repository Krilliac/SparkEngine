#include "../Core/Platform.h"
// CollisionSystem.cpp
#include "CollisionSystem.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iostream>

using namespace DirectX;

// BoundingBox methods
XMFLOAT3 BoundingBox::GetCenter() const
{
    return XMFLOAT3((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f, (Min.z + Max.z) * 0.5f);
}

XMFLOAT3 BoundingBox::GetExtents() const
{
    return XMFLOAT3((Max.x - Min.x) * 0.5f, (Max.y - Min.y) * 0.5f, (Max.z - Min.z) * 0.5f);
}

void BoundingBox::Transform(const XMMATRIX& transform)
{
    ASSERT_MSG(std::isfinite(transform.r[0].m128_f32[0]), "Invalid transform matrix");

    XMFLOAT3 corners[8] = {{Min.x, Min.y, Min.z}, {Max.x, Min.y, Min.z}, {Min.x, Max.y, Min.z}, {Max.x, Max.y, Min.z},
                           {Min.x, Min.y, Max.z}, {Max.x, Min.y, Max.z}, {Min.x, Max.y, Max.z}, {Max.x, Max.y, Max.z}};

    Min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    Max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR v = XMLoadFloat3(&corners[i]);
        v = XMVector3Transform(v, transform);
        XMFLOAT3 t;
        XMStoreFloat3(&t, v);

        Min.x = std::min(Min.x, t.x);
        Min.y = std::min(Min.y, t.y);
        Min.z = std::min(Min.z, t.z);
        Max.x = std::max(Max.x, t.x);
        Max.y = std::max(Max.y, t.y);
        Max.z = std::max(Max.z, t.z);
    }
}

// BoundingSphere methods
void BoundingSphere::Transform(const XMMATRIX& transform)
{
    ASSERT_MSG(std::isfinite(transform.r[0].m128_f32[0]), "Invalid transform matrix");

    XMVECTOR c = XMLoadFloat3(&Center);
    c = XMVector3Transform(c, transform);
    XMStoreFloat3(&Center, c);

    float sx = XMVectorGetX(XMVector3Length(transform.r[0]));
    float sy = XMVectorGetX(XMVector3Length(transform.r[1]));
    float sz = XMVectorGetX(XMVector3Length(transform.r[2]));
    float scale = std::max({sx, sy, sz});
    ASSERT_MSG(scale > 0.0f, "Non-positive scale");
    Radius *= scale;
}

// Ray methods
XMFLOAT3 Ray::GetPoint(float t) const
{
    ASSERT_MSG(std::isfinite(t), "Invalid ray parameter");
    return XMFLOAT3(Origin.x + Direction.x * t, Origin.y + Direction.y * t, Origin.z + Direction.z * t);
}

// Sphere-vs-Sphere
bool CollisionSystem::SphereVsSphere(const BoundingSphere& a, const BoundingSphere& b)
{
    XMVECTOR c1 = XMLoadFloat3(&a.Center);
    XMVECTOR c2 = XMLoadFloat3(&b.Center);
    float rSum = a.Radius + b.Radius;
    float dist = XMVectorGetX(XMVector3Length(c1 - c2));
    return dist <= rSum;
}

bool CollisionSystem::SphereVsSphere(const BoundingSphere& a, const BoundingSphere& b, ContactManifold& m)
{
    XMVECTOR c1 = XMLoadFloat3(&a.Center);
    XMVECTOR c2 = XMLoadFloat3(&b.Center);
    XMVECTOR delta = c2 - c1;
    float dist = XMVectorGetX(XMVector3Length(delta));
    float rSum = a.Radius + b.Radius;

    if (dist <= rSum)
    {
        m.ContactCount = 1;
        m.PenetrationDepth = rSum - dist;
        if (dist > 1e-5f)
        {
            XMVECTOR n = XMVector3Normalize(delta);
            XMStoreFloat3(&m.Normal, n);
            XMVECTOR pt = XMVectorAdd(c1, XMVectorScale(n, a.Radius));
            XMStoreFloat3(&m.ContactPoints[0], pt);
        }
        else
        {
            m.Normal = XMFLOAT3(1, 0, 0);
            m.ContactPoints[0] = a.Center;
        }
        return true;
    }
    return false;
}

// Sphere-vs-Box
bool CollisionSystem::SphereVsBox(const BoundingSphere& s, const BoundingBox& b)
{
    XMFLOAT3 cp = ClosestPointOnBox(s.Center, b);
    XMVECTOR c = XMLoadFloat3(&s.Center);
    XMVECTOR p = XMLoadFloat3(&cp);
    float dist = XMVectorGetX(XMVector3Length(c - p));
    return dist <= s.Radius;
}

// Box-vs-Box
bool CollisionSystem::BoxVsBox(const BoundingBox& a, const BoundingBox& b)
{
    return (a.Min.x <= b.Max.x && a.Max.x >= b.Min.x) && (a.Min.y <= b.Max.y && a.Max.y >= b.Min.y) &&
           (a.Min.z <= b.Max.z && a.Max.z >= b.Min.z);
}

// Ray-vs-Sphere
CollisionResult CollisionSystem::RayVsSphere(const Ray& ray, const BoundingSphere& sphere)
{
    CollisionResult res;
    XMVECTOR o = XMLoadFloat3(&ray.Origin);
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&ray.Direction));
    XMVECTOR c = XMLoadFloat3(&sphere.Center);
    XMVECTOR oc = XMVectorSubtract(o, c);

    float a = XMVectorGetX(XMVector3Dot(d, d));
    float b = 2.0f * XMVectorGetX(XMVector3Dot(oc, d));
    float cc = XMVectorGetX(XMVector3Dot(oc, oc)) - sphere.Radius * sphere.Radius;
    float disc = b * b - 4 * a * cc;
    if (disc < 0.0f)
        return res;

    float sq = sqrtf(disc);
    float t0 = (-b - sq) / (2 * a);
    float t1 = (-b + sq) / (2 * a);
    float t = (t0 > 0.0f) ? t0 : t1;
    if (t < 0.0f)
        return res;

    res.Hit = true;
    res.Distance = t;
    XMVECTOR pt = XMVectorAdd(o, XMVectorScale(d, t));
    XMStoreFloat3(&res.Point, pt);
    XMVECTOR n = XMVector3Normalize(XMVectorSubtract(pt, c));
    XMStoreFloat3(&res.Normal, n);
    return res;
}

// Ray-vs-Box
CollisionResult CollisionSystem::RayVsBox(const Ray& ray, const BoundingBox& box)
{
    CollisionResult res;
    XMVECTOR o = XMLoadFloat3(&ray.Origin);
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&ray.Direction));
    // Manual component intersection
    float ox = ray.Origin.x, oy = ray.Origin.y, oz = ray.Origin.z;
    float dx = ray.Direction.x, dy = ray.Direction.y, dz = ray.Direction.z;
    constexpr float epsilon = 1e-8f;
    float invX = (fabsf(dx) > epsilon) ? (1.0f / dx) : ((dx >= 0.0f) ? (1.0f / epsilon) : (-1.0f / epsilon));
    float invY = (fabsf(dy) > epsilon) ? (1.0f / dy) : ((dy >= 0.0f) ? (1.0f / epsilon) : (-1.0f / epsilon));
    float invZ = (fabsf(dz) > epsilon) ? (1.0f / dz) : ((dz >= 0.0f) ? (1.0f / epsilon) : (-1.0f / epsilon));

    float t1x = (box.Min.x - ox) * invX;
    float t2x = (box.Max.x - ox) * invX;
    float tminx = std::min(t1x, t2x), tmaxx = std::max(t1x, t2x);

    float t1y = (box.Min.y - oy) * invY;
    float t2y = (box.Max.y - oy) * invY;
    float tminy = std::min(t1y, t2y), tmaxy = std::max(t1y, t2y);

    float t1z = (box.Min.z - oz) * invZ;
    float t2z = (box.Max.z - oz) * invZ;
    float tminz = std::min(t1z, t2z), tmaxz = std::max(t1z, t2z);

    float tmin = std::max({tminx, tminy, tminz, 0.0f});
    float tmax = std::min({tmaxx, tmaxy, tmaxz, FLT_MAX});
    if (tmin > tmax)
        return res;

    res.Hit = true;
    res.Distance = tmin;
    XMVECTOR pt = XMVectorAdd(o, XMVectorScale(d, tmin));
    XMStoreFloat3(&res.Point, pt);
    XMFLOAT3 center = box.GetCenter();
    XMVECTOR cen = XMLoadFloat3(&center);
    XMVECTOR n = XMVector3Normalize(XMVectorSubtract(pt, cen));
    XMStoreFloat3(&res.Normal, n);
    return res;
}

// Ray-vs-Plane
CollisionResult CollisionSystem::RayVsPlane(const Ray& ray, const XMFLOAT3& pp, const XMFLOAT3& pn)
{
    CollisionResult res;
    XMVECTOR o = XMLoadFloat3(&ray.Origin);
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&ray.Direction));
    XMVECTOR p = XMLoadFloat3(&pp);
    XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&pn));

    float denom = XMVectorGetX(XMVector3Dot(d, n));
    if (fabs(denom) < 1e-6f)
        return res;
    float num = XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, o), n));
    float t = num / denom;
    if (t < 0.0f)
        return res;

    res.Hit = true;
    res.Distance = t;
    XMStoreFloat3(&res.Normal, n);
    XMVECTOR pt = XMVectorAdd(o, XMVectorScale(d, t));
    XMStoreFloat3(&res.Point, pt);
    return res;
}

// Ray-vs-Triangle
CollisionResult CollisionSystem::RayVsTriangle(const Ray& ray, const XMFLOAT3& v0, const XMFLOAT3& v1,
                                               const XMFLOAT3& v2)
{
    CollisionResult res;
    XMVECTOR o = XMLoadFloat3(&ray.Origin);
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&ray.Direction));
    XMVECTOR p0 = XMLoadFloat3(&v0), p1 = XMLoadFloat3(&v1), p2 = XMLoadFloat3(&v2);
    XMVECTOR e1 = XMVectorSubtract(p1, p0);
    XMVECTOR e2 = XMVectorSubtract(p2, p0);

    XMVECTOR h = XMVector3Cross(d, e2);
    float a = XMVectorGetX(XMVector3Dot(e1, h));
    if (fabs(a) < 1e-6f)
        return res;

    float f = 1.0f / a;
    XMVECTOR s = XMVectorSubtract(o, p0);
    float u = f * XMVectorGetX(XMVector3Dot(s, h));
    if (u < 0.0f || u > 1.0f)
        return res;

    XMVECTOR q = XMVector3Cross(s, e1);
    float v = f * XMVectorGetX(XMVector3Dot(d, q));
    if (v < 0.0f || u + v > 1.0f)
        return res;

    float t = f * XMVectorGetX(XMVector3Dot(e2, q));
    if (t <= 1e-6f)
        return res;

    res.Hit = true;
    res.Distance = t;
    XMVECTOR pt = XMVectorAdd(o, XMVectorScale(d, t));
    XMStoreFloat3(&res.Point, pt);
    XMVECTOR nrm = XMVector3Normalize(XMVector3Cross(e1, e2));
    XMStoreFloat3(&res.Normal, nrm);
    return res;
}

// Utility
XMFLOAT3 CollisionSystem::ClosestPointOnBox(const XMFLOAT3& pt, const BoundingBox& b)
{
    return XMFLOAT3(std::clamp(pt.x, b.Min.x, b.Max.x), std::clamp(pt.y, b.Min.y, b.Max.y),
                    std::clamp(pt.z, b.Min.z, b.Max.z));
}

XMFLOAT3 CollisionSystem::ClosestPointOnSphere(const XMFLOAT3& pt, const BoundingSphere& s)
{
    XMVECTOR c = XMLoadFloat3(&s.Center);
    XMVECTOR p = XMLoadFloat3(&pt);
    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(p, c));
    XMVECTOR cp = XMVectorAdd(c, XMVectorScale(dir, s.Radius));
    XMFLOAT3 out;
    XMStoreFloat3(&out, cp);
    return out;
}

float CollisionSystem::DistancePointToPlane(const XMFLOAT3& pt, const XMFLOAT3& pp, const XMFLOAT3& pn)
{
    XMVECTOR p = XMLoadFloat3(&pt);
    XMVECTOR p0 = XMLoadFloat3(&pp);
    XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&pn));
    return XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, p0), n));
}

bool CollisionSystem::PointInSphere(const XMFLOAT3& pt, const BoundingSphere& s)
{
    float dx = pt.x - s.Center.x;
    float dy = pt.y - s.Center.y;
    float dz = pt.z - s.Center.z;
    return (dx * dx + dy * dy + dz * dz) <= (s.Radius * s.Radius);
}

bool CollisionSystem::PointInBox(const XMFLOAT3& pt, const BoundingBox& b)
{
    return (pt.x >= b.Min.x && pt.x <= b.Max.x) && (pt.y >= b.Min.y && pt.y <= b.Max.y) &&
           (pt.z >= b.Min.z && pt.z <= b.Max.z);
}

// Vector helpers
float CollisionSystem::Vector3Length(const XMFLOAT3& v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

float CollisionSystem::Vector3LengthSquared(const XMFLOAT3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

XMFLOAT3 CollisionSystem::Vector3Normalize(const XMFLOAT3& v)
{
    float len = Vector3Length(v);
    if (len > 0.0f)
        return XMFLOAT3(v.x / len, v.y / len, v.z / len);
    return XMFLOAT3(0, 0, 0);
}

float CollisionSystem::Vector3Dot(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

XMFLOAT3 CollisionSystem::Vector3Cross(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

XMFLOAT3 CollisionSystem::Vector3Reflect(const XMFLOAT3& i, const XMFLOAT3& n)
{
    float d = Vector3Dot(i, n);
    return XMFLOAT3(i.x - 2.0f * d * n.x, i.y - 2.0f * d * n.y, i.z - 2.0f * d * n.z);
}

XMFLOAT3 CollisionSystem::Vector3Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    return XMFLOAT3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

// ============================================================================
// Contact manifold generators
// ============================================================================

bool CollisionSystem::SphereVsBox(const BoundingSphere& sphere, const BoundingBox& box, ContactManifold& m)
{
    XMFLOAT3 closest = ClosestPointOnBox(sphere.Center, box);

    float dx = sphere.Center.x - closest.x;
    float dy = sphere.Center.y - closest.y;
    float dz = sphere.Center.z - closest.z;
    float distSq = dx * dx + dy * dy + dz * dz;
    float radiusSq = sphere.Radius * sphere.Radius;

    if (distSq > radiusSq)
        return false;

    float dist = sqrtf(distSq);

    m.ContactCount = 1;
    m.ContactPoints[0] = closest;

    if (dist > 1e-5f)
    {
        // Normal points from box surface towards sphere center
        float invDist = 1.0f / dist;
        m.Normal = XMFLOAT3(dx * invDist, dy * invDist, dz * invDist);
        m.PenetrationDepth = sphere.Radius - dist;
    }
    else
    {
        // Sphere center is inside the box; find the axis of minimum penetration
        XMFLOAT3 center = box.GetCenter();
        XMFLOAT3 extents = box.GetExtents();

        float penX = extents.x - std::abs(sphere.Center.x - center.x);
        float penY = extents.y - std::abs(sphere.Center.y - center.y);
        float penZ = extents.z - std::abs(sphere.Center.z - center.z);

        if (penX <= penY && penX <= penZ)
        {
            float sign = (sphere.Center.x >= center.x) ? 1.0f : -1.0f;
            m.Normal = XMFLOAT3(sign, 0.0f, 0.0f);
            m.PenetrationDepth = penX + sphere.Radius;
        }
        else if (penY <= penX && penY <= penZ)
        {
            float sign = (sphere.Center.y >= center.y) ? 1.0f : -1.0f;
            m.Normal = XMFLOAT3(0.0f, sign, 0.0f);
            m.PenetrationDepth = penY + sphere.Radius;
        }
        else
        {
            float sign = (sphere.Center.z >= center.z) ? 1.0f : -1.0f;
            m.Normal = XMFLOAT3(0.0f, 0.0f, sign);
            m.PenetrationDepth = penZ + sphere.Radius;
        }
    }

    return true;
}

bool CollisionSystem::BoxVsBox(const BoundingBox& a, const BoundingBox& b, ContactManifold& m)
{
    // First check if they overlap at all
    if (!BoxVsBox(a, b))
        return false;

    XMFLOAT3 centerA = a.GetCenter();
    XMFLOAT3 centerB = b.GetCenter();
    XMFLOAT3 extA = a.GetExtents();
    XMFLOAT3 extB = b.GetExtents();

    // Find axis of minimum penetration (separating axis theorem for AABBs)
    float overlapX = (extA.x + extB.x) - std::abs(centerB.x - centerA.x);
    float overlapY = (extA.y + extB.y) - std::abs(centerB.y - centerA.y);
    float overlapZ = (extA.z + extB.z) - std::abs(centerB.z - centerA.z);

    m.ContactCount = 1;

    if (overlapX <= overlapY && overlapX <= overlapZ)
    {
        float sign = (centerB.x > centerA.x) ? 1.0f : -1.0f;
        m.Normal = XMFLOAT3(sign, 0.0f, 0.0f);
        m.PenetrationDepth = overlapX;
    }
    else if (overlapY <= overlapX && overlapY <= overlapZ)
    {
        float sign = (centerB.y > centerA.y) ? 1.0f : -1.0f;
        m.Normal = XMFLOAT3(0.0f, sign, 0.0f);
        m.PenetrationDepth = overlapY;
    }
    else
    {
        float sign = (centerB.z > centerA.z) ? 1.0f : -1.0f;
        m.Normal = XMFLOAT3(0.0f, 0.0f, sign);
        m.PenetrationDepth = overlapZ;
    }

    // Contact point: midpoint of the overlap region along the normal axis
    // Use the face center of the intersection region
    float cpX = std::max(a.Min.x, b.Min.x) + (std::min(a.Max.x, b.Max.x) - std::max(a.Min.x, b.Min.x)) * 0.5f;
    float cpY = std::max(a.Min.y, b.Min.y) + (std::min(a.Max.y, b.Max.y) - std::max(a.Min.y, b.Min.y)) * 0.5f;
    float cpZ = std::max(a.Min.z, b.Min.z) + (std::min(a.Max.z, b.Max.z) - std::max(a.Min.z, b.Min.z)) * 0.5f;
    m.ContactPoints[0] = XMFLOAT3(cpX, cpY, cpZ);

    return true;
}

// ============================================================================
// Sweep (shape-cast) tests
// ============================================================================

CollisionResult CollisionSystem::SweepSphereVsSphere(const BoundingSphere& moving, const XMFLOAT3& velocity,
                                                      const BoundingSphere& target, float& hitTime)
{
    CollisionResult result;
    hitTime = 0.0f;

    // Reduce to ray vs expanded sphere: expand target by moving's radius
    float expandedRadius = moving.Radius + target.Radius;

    // Solve: |moving.Center + t * velocity - target.Center|^2 = expandedRadius^2
    float dx = moving.Center.x - target.Center.x;
    float dy = moving.Center.y - target.Center.y;
    float dz = moving.Center.z - target.Center.z;

    float a = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
    if (a < 1e-10f)
    {
        // Not moving; check static overlap
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq <= expandedRadius * expandedRadius)
        {
            result.Hit = true;
            result.Distance = 0.0f;
            result.Point = moving.Center;
            hitTime = 0.0f;

            float dist = sqrtf(distSq);
            if (dist > 1e-5f)
            {
                result.Normal = XMFLOAT3(dx / dist, dy / dist, dz / dist);
            }
            else
            {
                result.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            }
        }
        return result;
    }

    float b = 2.0f * (dx * velocity.x + dy * velocity.y + dz * velocity.z);
    float c = dx * dx + dy * dy + dz * dz - expandedRadius * expandedRadius;

    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f)
        return result;

    float sqrtDisc = sqrtf(discriminant);
    float t0 = (-b - sqrtDisc) / (2.0f * a);
    float t1 = (-b + sqrtDisc) / (2.0f * a);

    // We want the earliest intersection in [0, 1]
    float t = -1.0f;
    if (t0 >= 0.0f && t0 <= 1.0f)
        t = t0;
    else if (t1 >= 0.0f && t1 <= 1.0f)
        t = t1;
    else if (t0 < 0.0f && t1 > 1.0f)
    {
        // Overlapping throughout the sweep
        t = 0.0f;
    }

    if (t < 0.0f)
        return result;

    hitTime = t;
    result.Hit = true;

    // Compute hit point: position of moving sphere center at time t
    XMFLOAT3 hitCenter(
        moving.Center.x + velocity.x * t,
        moving.Center.y + velocity.y * t,
        moving.Center.z + velocity.z * t);

    // Normal from target to hit center
    float nx = hitCenter.x - target.Center.x;
    float ny = hitCenter.y - target.Center.y;
    float nz = hitCenter.z - target.Center.z;
    float nLen = sqrtf(nx * nx + ny * ny + nz * nz);
    if (nLen > 1e-5f)
    {
        result.Normal = XMFLOAT3(nx / nLen, ny / nLen, nz / nLen);
    }
    else
    {
        result.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
    }

    // Contact point on the target sphere surface
    result.Point = XMFLOAT3(
        target.Center.x + result.Normal.x * target.Radius,
        target.Center.y + result.Normal.y * target.Radius,
        target.Center.z + result.Normal.z * target.Radius);

    result.Distance = sqrtf(
        (hitCenter.x - moving.Center.x) * (hitCenter.x - moving.Center.x) +
        (hitCenter.y - moving.Center.y) * (hitCenter.y - moving.Center.y) +
        (hitCenter.z - moving.Center.z) * (hitCenter.z - moving.Center.z));

    return result;
}

CollisionResult CollisionSystem::SweepSphereVsBox(const BoundingSphere& moving, const XMFLOAT3& velocity,
                                                    const BoundingBox& target, float& hitTime)
{
    CollisionResult result;
    hitTime = 0.0f;

    // Minkowski expansion: expand box by sphere radius, then ray-test from sphere center
    BoundingBox expanded(
        XMFLOAT3(target.Min.x - moving.Radius, target.Min.y - moving.Radius, target.Min.z - moving.Radius),
        XMFLOAT3(target.Max.x + moving.Radius, target.Max.y + moving.Radius, target.Max.z + moving.Radius));

    Ray ray(moving.Center, velocity);
    CollisionResult rayHit = RayVsBox(ray, expanded);

    if (!rayHit.Hit)
        return result;

    // Compute parametric t along velocity vector
    float velLenSq = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
    if (velLenSq < 1e-10f)
    {
        // Static test
        if (SphereVsBox(moving, target))
        {
            result.Hit = true;
            result.Distance = 0.0f;
            result.Point = ClosestPointOnBox(moving.Center, target);
            result.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            hitTime = 0.0f;
        }
        return result;
    }

    float velLen = sqrtf(velLenSq);
    float t = rayHit.Distance / velLen;

    if (t < 0.0f || t > 1.0f)
        return result;

    hitTime = t;
    result.Hit = true;
    result.Distance = rayHit.Distance;

    // Sphere center at hit time
    XMFLOAT3 hitCenter(
        moving.Center.x + velocity.x * t,
        moving.Center.y + velocity.y * t,
        moving.Center.z + velocity.z * t);

    // Closest point on the original (unexpanded) box to the sphere center at hit time
    result.Point = ClosestPointOnBox(hitCenter, target);

    // Normal from contact point to sphere center
    float nx = hitCenter.x - result.Point.x;
    float ny = hitCenter.y - result.Point.y;
    float nz = hitCenter.z - result.Point.z;
    float nLen = sqrtf(nx * nx + ny * ny + nz * nz);
    if (nLen > 1e-5f)
    {
        result.Normal = XMFLOAT3(nx / nLen, ny / nLen, nz / nLen);
    }
    else
    {
        result.Normal = rayHit.Normal;
    }

    return result;
}

CollisionResult CollisionSystem::SweepBoxVsBox(const BoundingBox& moving, const XMFLOAT3& velocity,
                                                const BoundingBox& target, float& hitTime)
{
    CollisionResult result;
    hitTime = 0.0f;

    // Minkowski sum: expand the target box by the moving box's half-extents
    XMFLOAT3 movExt = moving.GetExtents();
    BoundingBox expanded(
        XMFLOAT3(target.Min.x - movExt.x, target.Min.y - movExt.y, target.Min.z - movExt.z),
        XMFLOAT3(target.Max.x + movExt.x, target.Max.y + movExt.y, target.Max.z + movExt.z));

    // Ray from moving box center along velocity against expanded box
    XMFLOAT3 movCenter = moving.GetCenter();

    constexpr float epsilon = 1e-8f;
    float invX = (std::abs(velocity.x) > epsilon) ? (1.0f / velocity.x) :
                 ((velocity.x >= 0.0f) ? (1.0f / epsilon) : (-1.0f / epsilon));
    float invY = (std::abs(velocity.y) > epsilon) ? (1.0f / velocity.y) :
                 ((velocity.y >= 0.0f) ? (1.0f / epsilon) : (-1.0f / epsilon));
    float invZ = (std::abs(velocity.z) > epsilon) ? (1.0f / velocity.z) :
                 ((velocity.z >= 0.0f) ? (1.0f / epsilon) : (-1.0f / epsilon));

    float t1x = (expanded.Min.x - movCenter.x) * invX;
    float t2x = (expanded.Max.x - movCenter.x) * invX;
    float tMinX = std::min(t1x, t2x);
    float tMaxX = std::max(t1x, t2x);

    float t1y = (expanded.Min.y - movCenter.y) * invY;
    float t2y = (expanded.Max.y - movCenter.y) * invY;
    float tMinY = std::min(t1y, t2y);
    float tMaxY = std::max(t1y, t2y);

    float t1z = (expanded.Min.z - movCenter.z) * invZ;
    float t2z = (expanded.Max.z - movCenter.z) * invZ;
    float tMinZ = std::min(t1z, t2z);
    float tMaxZ = std::max(t1z, t2z);

    float tEnter = std::max({tMinX, tMinY, tMinZ});
    float tExit = std::min({tMaxX, tMaxY, tMaxZ});

    // No intersection or entirely behind
    if (tEnter > tExit || tExit < 0.0f || tEnter > 1.0f)
        return result;

    // If tEnter < 0, boxes are already overlapping
    float t = std::max(tEnter, 0.0f);

    hitTime = t;
    result.Hit = true;

    // Compute contact point: center of moving box at hit time
    XMFLOAT3 hitCenter(
        movCenter.x + velocity.x * t,
        movCenter.y + velocity.y * t,
        movCenter.z + velocity.z * t);

    result.Point = hitCenter;

    // Determine the normal based on which axis had the latest entry
    float velLen = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    result.Distance = t * velLen;

    if (tMinX >= tMinY && tMinX >= tMinZ)
    {
        result.Normal = XMFLOAT3((velocity.x > 0.0f) ? -1.0f : 1.0f, 0.0f, 0.0f);
    }
    else if (tMinY >= tMinX && tMinY >= tMinZ)
    {
        result.Normal = XMFLOAT3(0.0f, (velocity.y > 0.0f) ? -1.0f : 1.0f, 0.0f);
    }
    else
    {
        result.Normal = XMFLOAT3(0.0f, 0.0f, (velocity.z > 0.0f) ? -1.0f : 1.0f);
    }

    return result;
}
