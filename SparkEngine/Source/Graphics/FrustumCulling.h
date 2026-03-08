/**
 * @file FrustumCulling.h
 * @brief Camera frustum extraction and AABB/sphere visibility testing
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a Frustum class that extracts the six clip planes from a
 * view-projection matrix, and tests axis-aligned bounding boxes (AABBs)
 * and bounding spheres against them. Used by the RenderSystem to skip
 * draw calls for objects that are entirely outside the camera's view.
 *
 * ## Usage
 * @code
 *   Frustum frustum;
 *   frustum.ExtractPlanes(viewProj);
 *
 *   AABB box = { {-1,-1,-1}, {1,1,1} };
 *   if (frustum.TestAABB(box)) {
 *       // Object is potentially visible — submit draw call
 *   }
 * @endcode
 *
 * ## Integration with ECS
 * The RenderSystem calls `Frustum::ExtractPlanes()` once per frame, then
 * tests each MeshRenderer's bounding volume before submitting geometry.
 *
 * @see RenderSystem, MeshRenderer, Camera
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <array>
#include <cmath>

namespace Spark::Graphics
{

    /**
 * @brief Axis-aligned bounding box represented by min/max corners.
 */
    struct AABB
    {
        DirectX::XMFLOAT3 min{0, 0, 0};
        DirectX::XMFLOAT3 max{0, 0, 0};

        /** @brief Compute the center point of the AABB. */
        DirectX::XMFLOAT3 Center() const
        {
            return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
        }

        /** @brief Compute the half-extents (half-size) of the AABB. */
        DirectX::XMFLOAT3 Extents() const
        {
            return {(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f};
        }
    };

    /**
 * @brief Bounding sphere represented by center and radius.
 */
    struct BoundingSphere
    {
        DirectX::XMFLOAT3 center{0, 0, 0};
        float radius = 0.0f;
    };

    /**
 * @brief A single plane in Hessian normal form (ax + by + cz + d = 0).
 */
    struct Plane
    {
        float a = 0, b = 0, c = 0, d = 0;

        /** @brief Normalize the plane equation so (a,b,c) is a unit vector. */
        void Normalize()
        {
            float len = std::sqrt(a * a + b * b + c * c);
            if (len > 0.0f)
            {
                float inv = 1.0f / len;
                a *= inv;
                b *= inv;
                c *= inv;
                d *= inv;
            }
        }

        /** @brief Signed distance from a point to the plane. Positive = front side. */
        float DistanceToPoint(const DirectX::XMFLOAT3& p) const { return a * p.x + b * p.y + c * p.z + d; }
    };

    /**
 * @brief Visibility test result from frustum queries.
 */
    enum class CullResult
    {
        Outside,     ///< Entirely outside the frustum — safe to skip
        Inside,      ///< Entirely inside the frustum
        Intersecting ///< Partially inside — must be rendered
    };

    /**
 * @brief Camera frustum defined by six clip planes, extracted from a VP matrix.
 *
 * The Griggs-Hartmann method extracts planes directly from the combined
 * view-projection matrix, avoiding the need for inverse transforms. Each
 * plane normal points **inward** (toward the visible region).
 */
    class Frustum
    {
      public:
        /** @brief Plane indices for readability. */
        enum PlaneIndex
        {
            Left = 0,
            Right,
            Bottom,
            Top,
            Near,
            Far,
            Count
        };

        /**
     * @brief Extract the six frustum planes from a view-projection matrix.
     *
     * Call once per frame (or whenever the camera changes). The planes are
     * normalized so that distance queries return correct world-space values.
     *
     * @param viewProj  Combined View × Projection matrix (row-major).
     */
        void ExtractPlanes(const DirectX::XMMATRIX& viewProj)
        {
            DirectX::XMFLOAT4X4 m;
            DirectX::XMStoreFloat4x4(&m, viewProj);

            // Left: row3 + row0
            m_planes[Left] = {m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41};
            // Right: row3 - row0
            m_planes[Right] = {m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41};
            // Bottom: row3 + row1
            m_planes[Bottom] = {m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42};
            // Top: row3 - row1
            m_planes[Top] = {m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42};
            // Near: row2 (DirectX convention: 0 <= z <= w)
            m_planes[Near] = {m._13, m._23, m._33, m._43};
            // Far: row3 - row2
            m_planes[Far] = {m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43};

            for (auto& plane : m_planes)
            {
                plane.Normalize();
            }
        }

        /**
     * @brief Test an AABB against the frustum.
     *
     * Uses the "positive vertex" optimisation: for each plane, test only the
     * AABB corner that is most in the direction of the plane normal.
     *
     * @param box  The axis-aligned bounding box to test.
     * @return CullResult indicating visibility.
     */
        CullResult TestAABB(const AABB& box) const
        {
            bool allInside = true;

            for (int i = 0; i < Count; ++i)
            {
                const Plane& p = m_planes[i];

                // Positive vertex: the corner furthest along the plane normal
                DirectX::XMFLOAT3 pVertex{(p.a >= 0) ? box.max.x : box.min.x, (p.b >= 0) ? box.max.y : box.min.y,
                                          (p.c >= 0) ? box.max.z : box.min.z};

                // Negative vertex: the corner closest along the plane normal
                DirectX::XMFLOAT3 nVertex{(p.a >= 0) ? box.min.x : box.max.x, (p.b >= 0) ? box.min.y : box.max.y,
                                          (p.c >= 0) ? box.min.z : box.max.z};

                // If positive vertex is outside, the whole box is outside
                if (p.DistanceToPoint(pVertex) < 0.0f)
                {
                    return CullResult::Outside;
                }

                // If negative vertex is outside, the box intersects this plane
                if (p.DistanceToPoint(nVertex) < 0.0f)
                {
                    allInside = false;
                }
            }

            return allInside ? CullResult::Inside : CullResult::Intersecting;
        }

        /**
     * @brief Quick boolean test — returns true if the AABB is at least partially visible.
     */
        bool IsVisible(const AABB& box) const { return TestAABB(box) != CullResult::Outside; }

        /**
     * @brief Test a bounding sphere against the frustum.
     *
     * @param sphere  The bounding sphere to test.
     * @return CullResult indicating visibility.
     */
        CullResult TestSphere(const BoundingSphere& sphere) const
        {
            bool allInside = true;

            for (int i = 0; i < Count; ++i)
            {
                float dist = m_planes[i].DistanceToPoint(sphere.center);

                if (dist < -sphere.radius)
                {
                    return CullResult::Outside;
                }
                if (dist < sphere.radius)
                {
                    allInside = false;
                }
            }

            return allInside ? CullResult::Inside : CullResult::Intersecting;
        }

        /**
     * @brief Quick boolean test — returns true if the sphere is at least partially visible.
     */
        bool IsVisible(const BoundingSphere& sphere) const { return TestSphere(sphere) != CullResult::Outside; }

        /**
     * @brief Test a single point against the frustum.
     */
        bool IsPointInside(const DirectX::XMFLOAT3& point) const
        {
            for (int i = 0; i < Count; ++i)
            {
                if (m_planes[i].DistanceToPoint(point) < 0.0f)
                {
                    return false;
                }
            }
            return true;
        }

        /** @brief Access a frustum plane by index. */
        const Plane& GetPlane(int index) const { return m_planes[index]; }

      private:
        std::array<Plane, Count> m_planes;
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
