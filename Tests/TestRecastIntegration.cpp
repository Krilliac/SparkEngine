/**
 * @file TestRecastIntegration.cpp
 * @brief Tests verifying Recast/Detour integration with the NavMesh system
 */

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "Engine/AI/RecastDetourBackend.h"
#include "Engine/AI/NavMesh.h"

using namespace Spark::AI;

// ============================================================================
// Recast Availability
// ============================================================================

TEST(Recast_IsAvailable)
{
#ifdef SPARK_RECAST_AVAILABLE
    EXPECT_TRUE(IsRecastAvailable());
#else
    EXPECT_FALSE(IsRecastAvailable());
#endif
}

// ============================================================================
// Recast Build — Flat Quad
// ============================================================================

TEST(Recast_BuildFlatQuad)
{
    // 5x5 grid — enough triangulated geometry for Recast's voxelization pipeline
    std::vector<XMFLOAT3> verts;
    std::vector<uint32_t> indices;
    const int sz = 5;
    for (int z = 0; z <= sz; ++z)
        for (int x = 0; x <= sz; ++x)
            verts.push_back({static_cast<float>(x * 10), 0.0f, static_cast<float>(z * 10)});
    for (int z = 0; z < sz; ++z)
    {
        for (int x = 0; x < sz; ++x)
        {
            uint32_t tl = static_cast<uint32_t>(z * (sz + 1) + x);
            indices.push_back(tl);
            indices.push_back(tl + static_cast<uint32_t>(sz + 1));
            indices.push_back(tl + 1);
            indices.push_back(tl + 1);
            indices.push_back(tl + static_cast<uint32_t>(sz + 1));
            indices.push_back(tl + static_cast<uint32_t>(sz + 1) + 1);
        }
    }

    NavMeshBuildSettings settings;
    settings.cellSize = 0.5f;
    settings.cellHeight = 0.2f;
    settings.agentHeight = 2.0f;
    settings.agentRadius = 0.6f;
    settings.agentMaxClimb = 0.9f;
    settings.agentMaxSlope = 45.0f;

    auto result = BuildNavMeshWithRecast(verts, indices, settings);

    if (IsRecastAvailable())
    {
        EXPECT_TRUE(result != nullptr);
        if (result)
        {
            // Recast should produce triangles from the flat quad
            EXPECT_GT(result->triangles.size(), static_cast<size_t>(0));
            EXPECT_GT(result->vertices.size(), static_cast<size_t>(0));

            // Agent params should be carried through
            EXPECT_NEAR(result->agentHeight, 2.0f, 0.01f);
            EXPECT_NEAR(result->agentRadius, 0.6f, 0.01f);

            // Bounds should encompass the input
            EXPECT_NEAR(result->boundsMin.x, 0.0f, 1.0f);
            EXPECT_NEAR(result->boundsMax.x, 50.0f, 5.0f);
        }
    }
    else
    {
        // Without Recast, BuildNavMeshWithRecast returns nullptr
        EXPECT_TRUE(result == nullptr);
    }
}

// ============================================================================
// Recast Build — Large Grid (stress test)
// ============================================================================

TEST(Recast_BuildGrid)
{
    // 20x20 grid terrain
    const int gridSize = 20;
    std::vector<XMFLOAT3> verts;
    std::vector<uint32_t> indices;

    for (int z = 0; z <= gridSize; ++z)
    {
        for (int x = 0; x <= gridSize; ++x)
        {
            verts.push_back({static_cast<float>(x), 0.0f, static_cast<float>(z)});
        }
    }

    for (int z = 0; z < gridSize; ++z)
    {
        for (int x = 0; x < gridSize; ++x)
        {
            uint32_t tl = static_cast<uint32_t>(z * (gridSize + 1) + x);
            uint32_t tr = tl + 1;
            uint32_t bl = tl + static_cast<uint32_t>(gridSize + 1);
            uint32_t br = bl + 1;

            indices.push_back(tl);
            indices.push_back(bl);
            indices.push_back(tr);
            indices.push_back(tr);
            indices.push_back(bl);
            indices.push_back(br);
        }
    }

    NavMeshBuildSettings settings;
    settings.cellSize = 0.3f;

    auto result = BuildNavMeshWithRecast(verts, indices, settings);

    if (IsRecastAvailable())
    {
        EXPECT_TRUE(result != nullptr);
        if (result)
        {
            // Recast optimizes geometry — should produce at least some triangles
            EXPECT_GT(result->triangles.size(), static_cast<size_t>(0));
        }
    }
}

// ============================================================================
// Recast Build — Empty Input
// ============================================================================

TEST(Recast_BuildEmptyInput)
{
    std::vector<XMFLOAT3> emptyVerts;
    std::vector<uint32_t> emptyIndices;
    NavMeshBuildSettings settings;

    auto result = BuildNavMeshWithRecast(emptyVerts, emptyIndices, settings);

    if (IsRecastAvailable())
    {
        // Empty input should return an empty navmesh (not null)
        EXPECT_TRUE(result != nullptr);
        if (result)
        {
            EXPECT_EQ(result->triangles.size(), static_cast<size_t>(0));
        }
    }
}

// ============================================================================
// Recast Integration — NavMeshBuilder uses Recast when available
// ============================================================================

TEST(Recast_NavMeshBuilderUsesRecast)
{
    // Build through the public NavMeshBuilder::Build API
    std::vector<XMFLOAT3> verts = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}};
    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

    NavMeshBuildSettings settings;
    settings.cellSize = 0.5f;

    auto result = NavMeshBuilder::Build(verts, indices, settings);
    EXPECT_TRUE(result != nullptr);

    if (result)
    {
        // Whether Recast or fallback, we should get valid triangles
        EXPECT_GT(result->triangles.size(), static_cast<size_t>(0));
        EXPECT_GT(result->vertices.size(), static_cast<size_t>(0));
    }
}

// ============================================================================
// Recast Integration — Pathfinding works on Recast-generated mesh
// ============================================================================

TEST(Recast_PathfindingOnRecastMesh)
{
    // Build a navmesh from a flat surface
    std::vector<XMFLOAT3> verts = {{0, 0, 0}, {20, 0, 0}, {20, 0, 20}, {0, 0, 20}};
    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

    NavMeshBuildSettings settings;
    settings.cellSize = 0.5f;
    settings.agentRadius = 0.3f;

    auto navMeshData = NavMeshBuilder::Build(verts, indices, settings);
    EXPECT_TRUE(navMeshData != nullptr);

    if (navMeshData && !navMeshData->triangles.empty())
    {
        NavMeshQuery query(navMeshData.get());

        PathRequest req;
        req.start = {2.0f, 0.0f, 2.0f};
        req.end = {18.0f, 0.0f, 18.0f};
        req.agentRadius = 0.3f;

        PathResult result = query.FindPath(req);

        // Pathfinding should succeed on a flat walkable surface
        EXPECT_TRUE(result.found);
        if (result.found)
        {
            EXPECT_GT(result.path.size(), static_cast<size_t>(0));
        }
    }
}
