/**
 * @file TestBVHAccelerator.cpp
 * @brief Phase L — tests for the BVHAccelerator orphan
 *
 * Phase L activates `Spark::Graphics::BVHAccelerator` by wiring it into
 * `SceneRenderer::CullAndSort()` as a first-level frustum pre-cull.
 * These tests pin the Build / FrustumQuery contract so future refactors
 * can't silently regress the SAH construction or the plane-AABB
 * traversal math.
 *
 * The BVH, AABB, and Frustum classes are all gated behind
 * `SPARK_PLATFORM_WINDOWS` — they depend on DirectXMath and the stubs
 * on non-Windows are incomplete. The entire test file compiles to an
 * empty translation unit on Linux / macOS, so CI only picks these tests
 * up on the Windows jobs.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "../SparkEngine/Source/Graphics/BVHAccelerator.h"
#include "../SparkEngine/Source/Graphics/FrustumCulling.h"

#include <DirectXMath.h>

#include <algorithm>
#include <vector>

using Spark::Graphics::AABB;
using Spark::Graphics::BVHAccelerator;
using Spark::Graphics::BVHPrimitive;
using Spark::Graphics::Frustum;

namespace
{
    BVHPrimitive MakeUnitPrimitive(uint32_t id, float x, float y, float z, float halfExtent = 0.5f)
    {
        BVHPrimitive p;
        p.objectId = id;
        p.bounds.min = {x - halfExtent, y - halfExtent, z - halfExtent};
        p.bounds.max = {x + halfExtent, y + halfExtent, z + halfExtent};
        return p;
    }

    Frustum MakeFrustumLookingAtOrigin()
    {
        using namespace DirectX;
        XMVECTOR eye = XMVectorSet(0, 0, -10, 1);
        XMVECTOR at = XMVectorSet(0, 0, 0, 1);
        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, 100.0f);
        Frustum f;
        f.ExtractPlanes(XMMatrixMultiply(view, proj));
        return f;
    }
} // namespace

// =========================================================================
// Build
// =========================================================================

TEST(BVH_Build_EmptyInputIsSafe)
{
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> empty;
    bvh.Build(empty);
    // Frustum query on an empty BVH must return an empty list.
    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 0);
}

TEST(BVH_Build_SinglePrimitive)
{
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(42, 0, 0, 0)};
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 1);
    EXPECT_EQ(visible[0], 42u);
}

TEST(BVH_Build_ManyPrimitivesKeepsAllVisible)
{
    // Build a grid of unit cubes in front of the camera. All of them
    // should pass the frustum test.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            prims.push_back(MakeUnitPrimitive(static_cast<uint32_t>(prims.size()), static_cast<float>(x),
                                              static_cast<float>(y), 0.0f));
        }
    }
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), static_cast<int>(prims.size()));
}

// =========================================================================
// FrustumQuery culling
// =========================================================================

TEST(BVH_FrustumQuery_CullsBehindCamera)
{
    // The camera sits at z=-10 looking at the origin. Primitives behind
    // the camera (z < -10) should be culled.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(1, 0, 0, 0),    // in front, should stay
                                       MakeUnitPrimitive(2, 0, 0, -50)}; // behind camera, should cull
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);

    // Only the in-front primitive survives.
    EXPECT_EQ(static_cast<int>(visible.size()), 1);
    EXPECT_EQ(visible[0], 1u);
}

TEST(BVH_FrustumQuery_CullsBeyondFarPlane)
{
    // Far plane is at z=100 from the camera (at z=-10), so anything
    // past world z=90 is outside the frustum.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(1, 0, 0, 0),    // well inside
                                       MakeUnitPrimitive(2, 0, 0, 500)}; // way past far plane
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    // Primitive 2 must be culled.
    EXPECT_TRUE(std::find(visible.begin(), visible.end(), 2u) == visible.end());
    EXPECT_TRUE(std::find(visible.begin(), visible.end(), 1u) != visible.end());
}

TEST(BVH_FrustumQuery_PreservesObjectIds)
{
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(100, 0, 0, 0), MakeUnitPrimitive(200, 1, 0, 0),
                                       MakeUnitPrimitive(300, -1, 0, 0)};
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 3);
    // Each original object ID must appear in the result exactly once.
    EXPECT_TRUE(std::find(visible.begin(), visible.end(), 100u) != visible.end());
    EXPECT_TRUE(std::find(visible.begin(), visible.end(), 200u) != visible.end());
    EXPECT_TRUE(std::find(visible.begin(), visible.end(), 300u) != visible.end());
}

// =========================================================================
// Rebuild semantics
// =========================================================================

TEST(BVH_Rebuild_ReplacesOldPrimitives)
{
    BVHAccelerator bvh;
    {
        std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(1, 0, 0, 0), MakeUnitPrimitive(2, 1, 0, 0)};
        bvh.Build(prims);
    }
    Frustum f = MakeFrustumLookingAtOrigin();
    auto firstResult = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(firstResult.size()), 2);

    // Rebuild with a different set — the old primitives must be gone.
    {
        std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(999, 0, 0, 0)};
        bvh.Build(prims);
    }
    auto secondResult = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(secondResult.size()), 1);
    EXPECT_EQ(secondResult[0], 999u);
}

TEST(BVH_Build_EmptyAfterNonEmptyReturnsEmptyQuery)
{
    BVHAccelerator bvh;
    {
        std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(1, 0, 0, 0)};
        bvh.Build(prims);
    }
    {
        std::vector<BVHPrimitive> empty;
        bvh.Build(empty);
    }
    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 0);
}

// =========================================================================
// Large input smoke test
// =========================================================================

TEST(BVH_Build_HandlesFiftyPrimitives)
{
    // 50 primitives spread across a plane in front of the camera. All
    // should remain visible under a wide frustum, which also exercises
    // the SAH split path.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims;
    for (int i = 0; i < 50; ++i)
    {
        float x = static_cast<float>((i % 10) - 4);
        float y = static_cast<float>((i / 10) - 2);
        prims.push_back(MakeUnitPrimitive(static_cast<uint32_t>(i), x, y, 0.0f));
    }
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 50);
}

#endif // SPARK_PLATFORM_WINDOWS
