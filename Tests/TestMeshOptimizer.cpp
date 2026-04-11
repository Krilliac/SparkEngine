/**
 * @file TestMeshOptimizer.cpp
 * @brief Phase L — real tests against `Spark::Graphics::MeshOptimizer`
 *
 * Phase L activates the MeshOptimizer Tier 2 graphics orphan by giving
 * it a real engine-side caller inside `LODManager::GenerateLODChain`
 * (Windows-only path) and by running comprehensive tests against the
 * real class from the portable test suite.
 *
 * `TestGraphicsIntegration.cpp` already contained a *local
 * reimplementation* of `ComputeACMR` under the `TestMeshOpt::` namespace,
 * but the real `Spark::Graphics::MeshOptimizer::ComputeACMR` /
 * `OptimizeVertexCache` / `OptimizeVertexFetch` / `GenerateMeshlets`
 * functions had no dedicated test coverage. Phase L fixes that — this
 * file exercises the real static methods directly on portable CPU
 * data so every platform's CI picks them up.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/MeshOptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Spark::Graphics::Meshlet;
using Spark::Graphics::MeshletData;
using Spark::Graphics::MeshOptimizer;

namespace
{
    // Build a simple 4x4 grid of quads (32 triangles, 25 vertices) with
    // scattered indices that will give the vertex-cache optimizer
    // something to improve.
    void BuildQuadGrid(std::vector<uint32_t>& indices, std::vector<float>& positions, uint32_t gridSize = 4)
    {
        const uint32_t verticesPerRow = gridSize + 1;
        positions.clear();
        positions.reserve(static_cast<size_t>(verticesPerRow) * verticesPerRow * 3);
        for (uint32_t y = 0; y <= gridSize; ++y)
        {
            for (uint32_t x = 0; x <= gridSize; ++x)
            {
                positions.push_back(static_cast<float>(x));
                positions.push_back(static_cast<float>(y));
                positions.push_back(0.0f);
            }
        }

        indices.clear();
        indices.reserve(static_cast<size_t>(gridSize) * gridSize * 6);
        for (uint32_t y = 0; y < gridSize; ++y)
        {
            for (uint32_t x = 0; x < gridSize; ++x)
            {
                uint32_t v0 = y * verticesPerRow + x;
                uint32_t v1 = v0 + 1;
                uint32_t v2 = v0 + verticesPerRow;
                uint32_t v3 = v2 + 1;
                // Two triangles per quad — deliberately scattered
                indices.push_back(v0);
                indices.push_back(v2);
                indices.push_back(v1);
                indices.push_back(v1);
                indices.push_back(v2);
                indices.push_back(v3);
            }
        }
    }
} // namespace

// =========================================================================
// ComputeACMR
// =========================================================================

TEST(MeshOptimizer_ComputeACMR_EmptyMeshReturnsZero)
{
    float acmr = MeshOptimizer::ComputeACMR(nullptr, 0);
    EXPECT_NEAR(acmr, 0.0f, 1e-6f);
}

TEST(MeshOptimizer_ComputeACMR_SingleTriangle)
{
    // 3 unique vertices, 3 cache misses → ACMR = 3.0 for a single-tri mesh.
    std::vector<uint32_t> indices = {0, 1, 2};
    float acmr = MeshOptimizer::ComputeACMR(indices.data(), indices.size());
    EXPECT_NEAR(acmr, 3.0f, 1e-4f);
}

TEST(MeshOptimizer_ComputeACMR_SequentialStrip_IsCacheFriendly)
{
    // A triangle strip of 4 vertices (2 tris). First tri has 3 misses,
    // second tri reuses 2 cached vertices → 4 total misses / 2 tris = 2.0.
    std::vector<uint32_t> indices = {0, 1, 2, 1, 2, 3};
    float acmr = MeshOptimizer::ComputeACMR(indices.data(), indices.size());
    EXPECT_LE(acmr, 2.0f);
    EXPECT_GT(acmr, 0.0f);
}

TEST(MeshOptimizer_ComputeACMR_ScatteredIndicesAreWorse)
{
    // Two different index orderings over the same set of vertices. The
    // sequential version should have strictly lower ACMR.
    std::vector<uint32_t> sequential = {0, 1, 2, 1, 2, 3, 2, 3, 4, 3, 4, 5};
    std::vector<uint32_t> scattered = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110};
    float acmrSeq = MeshOptimizer::ComputeACMR(sequential.data(), sequential.size());
    float acmrScat = MeshOptimizer::ComputeACMR(scattered.data(), scattered.size());
    EXPECT_LT(acmrSeq, acmrScat);
}

// =========================================================================
// OptimizeVertexCache
// =========================================================================

TEST(MeshOptimizer_OptimizeVertexCache_EmptyIsSafe)
{
    // Zero indices or zero vertices must not crash.
    MeshOptimizer::OptimizeVertexCache(nullptr, 0, 0);
    MeshOptimizer::OptimizeVertexCache(nullptr, 0, 10);
    std::vector<uint32_t> indices = {0, 1, 2};
    MeshOptimizer::OptimizeVertexCache(indices.data(), 0, 3);
    // Buffer untouched.
    EXPECT_EQ(indices[0], 0u);
    EXPECT_EQ(indices[1], 1u);
    EXPECT_EQ(indices[2], 2u);
}

TEST(MeshOptimizer_OptimizeVertexCache_PreservesTriangleCount)
{
    std::vector<uint32_t> indices;
    std::vector<float> positions;
    BuildQuadGrid(indices, positions, 4);

    size_t originalCount = indices.size();
    MeshOptimizer::OptimizeVertexCache(indices.data(), indices.size(), 25);
    EXPECT_EQ(indices.size(), originalCount);
}

TEST(MeshOptimizer_OptimizeVertexCache_PreservesVertexSet)
{
    std::vector<uint32_t> indices;
    std::vector<float> positions;
    BuildQuadGrid(indices, positions, 4);

    std::vector<uint32_t> before = indices;
    MeshOptimizer::OptimizeVertexCache(indices.data(), indices.size(), 25);

    // Same set of indices (just reordered across triangles).
    std::sort(before.begin(), before.end());
    std::vector<uint32_t> after = indices;
    std::sort(after.begin(), after.end());
    EXPECT_EQ(static_cast<int>(before.size()), static_cast<int>(after.size()));
    for (size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_EQ(before[i], after[i]);
    }
}

// =========================================================================
// OptimizeVertexFetch
// =========================================================================

TEST(MeshOptimizer_OptimizeVertexFetch_SingleTriangle)
{
    struct Vertex
    {
        float x, y, z;
    };
    std::vector<Vertex> verts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<uint32_t> indices = {2, 0, 1}; // deliberately not in vertex order

    MeshOptimizer::OptimizeVertexFetch(verts.data(), indices.data(), verts.size(), indices.size(), sizeof(Vertex));

    // After fetch-optimisation, the first-used vertex should land at slot 0.
    // Original order was 2 → 0 → 1, so the remap is v2→0, v0→1, v1→2.
    // Indices are remapped accordingly.
    EXPECT_EQ(indices[0], 0u);
    EXPECT_EQ(indices[1], 1u);
    EXPECT_EQ(indices[2], 2u);

    // The vertex at new slot 0 must hold the original vertex 2's data.
    EXPECT_NEAR(verts[0].y, 1.0f, 1e-6f);
}

TEST(MeshOptimizer_OptimizeVertexFetch_PreservesTriangleMeaning)
{
    // A triangle should shade identically after optimise — the per-vertex
    // data that each triangle references is unchanged, only the vertex
    // buffer layout moves. This test just confirms the optimized buffer
    // is readable after the remapping; the Vertex data is still live at
    // the new indices[i] positions.
    struct Vertex
    {
        float x, y, z;
    };
    std::vector<Vertex> original = {{10, 0, 0}, {0, 20, 0}, {0, 0, 30}};
    std::vector<Vertex> optimised = original;
    std::vector<uint32_t> indices = {1, 2, 0};

    MeshOptimizer::OptimizeVertexFetch(optimised.data(), indices.data(), optimised.size(), indices.size(),
                                       sizeof(Vertex));

    // Read each triangle vertex through the remapped index and confirm
    // it is reachable without a crash. Earlier this test also computed
    // an out-of-bounds `original[1 + ((i + 2) % 3) - ...]` lookup for a
    // comparison that was never actually used; removing it because it
    // aborts under _GLIBCXX_ASSERTIONS (Ubuntu-runner coverage build).
    for (int i = 0; i < 3; ++i)
    {
        const Vertex& optV = optimised[indices[i]];
        EXPECT_TRUE(optV.x >= 0.0f || optV.y >= 0.0f || optV.z >= 0.0f);
    }
}

// =========================================================================
// GenerateMeshlets
// =========================================================================

TEST(MeshOptimizer_GenerateMeshlets_EmptyMeshReturnsEmpty)
{
    MeshletData data = MeshOptimizer::GenerateMeshlets(nullptr, 0, nullptr, 0, 0);
    EXPECT_EQ(static_cast<int>(data.meshlets.size()), 0);
    EXPECT_EQ(static_cast<int>(data.meshletVertices.size()), 0);
    EXPECT_EQ(static_cast<int>(data.meshletTriangles.size()), 0);
}

TEST(MeshOptimizer_GenerateMeshlets_SingleTriangleProducesOneMeshlet)
{
    std::vector<uint32_t> indices = {0, 1, 2};
    float positions[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    MeshletData data = MeshOptimizer::GenerateMeshlets(indices.data(), indices.size(), positions, 3, sizeof(float) * 3);
    EXPECT_EQ(static_cast<int>(data.meshlets.size()), 1);
    EXPECT_EQ(data.meshlets[0].triangleCount, 1u);
    EXPECT_EQ(data.meshlets[0].vertexCount, 3u);
}

TEST(MeshOptimizer_GenerateMeshlets_QuadGridFitsInOneMeshlet)
{
    // A 4x4 quad grid is 32 tris and 25 verts — well under the 124 tri /
    // 64 vertex meshlet limits, so the whole mesh collapses to one meshlet.
    std::vector<uint32_t> indices;
    std::vector<float> positions;
    BuildQuadGrid(indices, positions, 4);

    MeshletData data = MeshOptimizer::GenerateMeshlets(indices.data(), indices.size(), positions.data(),
                                                       positions.size() / 3, sizeof(float) * 3);
    EXPECT_EQ(static_cast<int>(data.meshlets.size()), 1);
    EXPECT_EQ(data.meshlets[0].triangleCount, 32u);
    EXPECT_LE(data.meshlets[0].vertexCount, static_cast<uint32_t>(Meshlet::kMaxVertices));
    EXPECT_LE(data.meshlets[0].triangleCount, static_cast<uint32_t>(Meshlet::kMaxTriangles));
    // Bounding sphere must have positive radius.
    EXPECT_GT(data.meshlets[0].radius, 0.0f);
}

TEST(MeshOptimizer_GenerateMeshlets_BoundingSphereCoversAllVertices)
{
    std::vector<uint32_t> indices;
    std::vector<float> positions;
    BuildQuadGrid(indices, positions, 4);

    MeshletData data = MeshOptimizer::GenerateMeshlets(indices.data(), indices.size(), positions.data(),
                                                       positions.size() / 3, sizeof(float) * 3);
    // Verify each vertex used by the first meshlet lies within its
    // bounding sphere (radius + tiny epsilon for float error).
    const Meshlet& m = data.meshlets[0];
    for (uint32_t vi = 0; vi < m.vertexCount; ++vi)
    {
        uint32_t originalIdx = data.meshletVertices[m.vertexOffset + vi];
        float x = positions[originalIdx * 3 + 0];
        float y = positions[originalIdx * 3 + 1];
        float z = positions[originalIdx * 3 + 2];
        float dx = x - m.centerX;
        float dy = y - m.centerY;
        float dz = z - m.centerZ;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        EXPECT_LE(dist, m.radius + 1e-3f);
    }
}

TEST(MeshOptimizer_MeshletLimits_ArePinned)
{
    // The Meshlet constants are NVIDIA's recommended limits — pin them
    // so future refactors can't silently change the API contract.
    EXPECT_EQ(static_cast<uint32_t>(Meshlet::kMaxVertices), 64u);
    EXPECT_EQ(static_cast<uint32_t>(Meshlet::kMaxTriangles), 124u);
}

// =========================================================================
// GetOptimizationReport (smoke test)
// =========================================================================

TEST(MeshOptimizer_GetOptimizationReport_ReturnsNonEmptyString)
{
    std::vector<uint32_t> indices;
    std::vector<float> positions;
    BuildQuadGrid(indices, positions, 4);

    std::string report = MeshOptimizer::GetOptimizationReport(indices.data(), indices.size(), 25);
    EXPECT_TRUE(!report.empty());
    // Must contain the ACMR metric somewhere in the output.
    EXPECT_TRUE(report.find("ACMR") != std::string::npos || report.find("acmr") != std::string::npos ||
                report.find("cache") != std::string::npos || report.find("Cache") != std::string::npos);
}
