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
//
// IMPORTANT: BVHAccelerator's FrustumQuery only performs PER-LEAF frustum
// culling, not per-primitive. When a leaf node's bounding box intersects
// the frustum, ALL primitives in that leaf are returned regardless of
// their individual positions. The default `maxLeafSize` is 4, so up to 4
// primitives can share a single leaf.
//
// These tests use SINGLE-primitive BVHs for the cull-target side so the
// "outside" leaf only contains one primitive whose bounds are entirely
// outside the frustum — guaranteeing the frustum query rejects it via
// the leaf-level test. Mixing inside/outside primitives in the same BVH
// would put them in a single leaf whose bounds CROSS the frustum plane,
// and the leaf-level cull would conservatively keep both.

TEST(BVH_FrustumQuery_CullsBehindCamera)
{
    // Lone primitive way behind the camera. With maxLeafSize=4 and only
    // one primitive, the BVH builds a single leaf with bounds entirely
    // outside the near plane → frustum query returns empty.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(2, 0, 0, -50)};
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);

    EXPECT_EQ(static_cast<int>(visible.size()), 0);
}

TEST(BVH_FrustumQuery_KeepsInFrontPrimitive)
{
    // Companion to CullsBehindCamera — verify the front primitive
    // survives when it's the only one in the BVH.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(1, 0, 0, 0)};
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);

    EXPECT_EQ(static_cast<int>(visible.size()), 1);
    EXPECT_EQ(visible[0], 1u);
}

TEST(BVH_FrustumQuery_CullsBeyondFarPlane)
{
    // Far plane is at z≈90 in world space (camera at z=-10 + far distance
    // 100). A single primitive at z=500 is well past the far plane and
    // forms a leaf whose bounds are entirely outside.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims = {MakeUnitPrimitive(2, 0, 0, 500)};
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 0);
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
    // 50 primitives in a 10x5 grid centred on the origin, scaled so they
    // all sit well inside the camera frustum (extent ±4.143 at z=0 from
    // a camera at z=-10 with 45° vertical FOV). Halving the grid step to
    // 0.5 keeps the maximum primitive bounds at ±2.5+0.5 = ±3.0, which
    // is comfortably inside the right/left planes on every platform —
    // both real DirectXMath on Windows and the Linux scalar stub.
    BVHAccelerator bvh;
    std::vector<BVHPrimitive> prims;
    for (int i = 0; i < 50; ++i)
    {
        float x = (static_cast<float>((i % 10) - 4) - 0.5f) * 0.5f; // x ∈ [-2.25, 2.25]
        float y = static_cast<float>((i / 10) - 2) * 0.5f;          // y ∈ [-1.0, 1.0]
        prims.push_back(MakeUnitPrimitive(static_cast<uint32_t>(i), x, y, 0.0f, 0.2f));
    }
    bvh.Build(prims);

    Frustum f = MakeFrustumLookingAtOrigin();
    auto visible = bvh.FrustumQuery(f);
    EXPECT_EQ(static_cast<int>(visible.size()), 50);
}

#endif // SPARK_PLATFORM_WINDOWS
