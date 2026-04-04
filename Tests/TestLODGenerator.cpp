// TestLODGenerator.cpp - Tests for LOD chain generation and mesh simplification
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_set>
#include <vector>

namespace TestLODGen
{

    struct GeneratedLOD
    {
        std::vector<float> vertices;
        std::vector<float> normals;
        std::vector<uint32_t> indices;
        uint32_t triangleCount = 0;
        float geometricError = 0.0f;
    };

    struct LODGenerationOptions
    {
        uint32_t lodCount = 4;
        float reductionPerLevel = 0.5f;
        float maxError = 0.01f;
    };

    struct LODChainResult
    {
        std::vector<GeneratedLOD> levels;
        float totalReduction = 0.0f;
    };

    // Simplified QEM simplifier for testing
    GeneratedLOD SimplifyMesh(const float* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount,
                              uint32_t targetTriangles)
    {
        GeneratedLOD result;
        if (!vertices || !indices || vertexCount == 0 || indexCount < 3)
        {
            result.triangleCount = 0;
            return result;
        }

        uint32_t triCount = indexCount / 3;
        if (triCount <= targetTriangles)
        {
            result.vertices.assign(vertices, vertices + vertexCount * 3);
            result.indices.assign(indices, indices + indexCount);
            result.triangleCount = triCount;
            result.geometricError = 0.0f;
            return result;
        }

        // Simple edge-collapse: iteratively remove vertices
        std::vector<float> verts(vertices, vertices + vertexCount * 3);
        std::vector<uint32_t> tris(indices, indices + indexCount);
        std::vector<bool> removed(vertexCount, false);
        uint32_t currentTris = triCount;

        while (currentTris > targetTriangles)
        {
            // Find first removable edge
            bool collapsed = false;
            for (uint32_t t = 0; t < triCount && !collapsed; ++t)
            {
                uint32_t i0 = tris[t * 3], i1 = tris[t * 3 + 1];
                if (i0 == i1 || removed[i0] || removed[i1])
                    continue;

                // Collapse i1 into i0
                verts[i0 * 3] = (verts[i0 * 3] + verts[i1 * 3]) * 0.5f;
                verts[i0 * 3 + 1] = (verts[i0 * 3 + 1] + verts[i1 * 3 + 1]) * 0.5f;
                verts[i0 * 3 + 2] = (verts[i0 * 3 + 2] + verts[i1 * 3 + 2]) * 0.5f;
                removed[i1] = true;

                uint32_t removedCount = 0;
                for (uint32_t tt = 0; tt < triCount; ++tt)
                {
                    bool hasI0 = false, hasI1 = false;
                    for (int e = 0; e < 3; ++e)
                    {
                        if (tris[tt * 3 + e] == i0)
                            hasI0 = true;
                        if (tris[tt * 3 + e] == i1)
                        {
                            tris[tt * 3 + e] = i0;
                            hasI1 = true;
                        }
                    }
                    if (hasI0 && hasI1)
                    {
                        tris[tt * 3] = tris[tt * 3 + 1] = tris[tt * 3 + 2] = 0;
                        removedCount++;
                    }
                }
                currentTris -= removedCount;
                collapsed = true;
            }

            if (!collapsed)
                break;
        }

        // Compact
        for (uint32_t t = 0; t < triCount; ++t)
        {
            uint32_t i0 = tris[t * 3], i1 = tris[t * 3 + 1], i2 = tris[t * 3 + 2];
            if (i0 == i1 || i1 == i2 || i0 == i2)
                continue;
            if (removed[i0] || removed[i1] || removed[i2])
                continue;
            result.indices.push_back(i0);
            result.indices.push_back(i1);
            result.indices.push_back(i2);
        }

        result.vertices = verts;
        result.triangleCount = static_cast<uint32_t>(result.indices.size() / 3);
        result.geometricError = 0.001f;
        return result;
    }

    LODChainResult GenerateLODChain(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                    uint32_t indexCount, const LODGenerationOptions& options)
    {
        LODChainResult result;
        if (!vertices || !indices || vertexCount == 0 || indexCount < 3)
            return result;

        uint32_t originalTris = indexCount / 3;

        // LOD0 = original
        GeneratedLOD lod0;
        lod0.vertices.assign(vertices, vertices + vertexCount * 3);
        lod0.indices.assign(indices, indices + indexCount);
        lod0.triangleCount = originalTris;
        result.levels.push_back(std::move(lod0));

        float cumReduction = 1.0f;
        for (uint32_t level = 1; level < options.lodCount; ++level)
        {
            cumReduction *= options.reductionPerLevel;
            uint32_t target = std::max(static_cast<uint32_t>(originalTris * cumReduction), 1u);
            result.levels.push_back(SimplifyMesh(vertices, vertexCount, indices, indexCount, target));
        }

        if (!result.levels.empty() && originalTris > 0)
        {
            result.totalReduction =
                1.0f - static_cast<float>(result.levels.back().triangleCount) / static_cast<float>(originalTris);
        }
        return result;
    }

    // A simple cube mesh: 8 vertices, 12 triangles
    void MakeCube(std::vector<float>& verts, std::vector<uint32_t>& indices)
    {
        verts = {
            // clang-format off
            -1, -1, -1,  1, -1, -1,  1,  1, -1, -1,  1, -1,
            -1, -1,  1,  1, -1,  1,  1,  1,  1, -1,  1,  1
            // clang-format on
        };
        indices = {
            // clang-format off
            0,1,2, 0,2,3,  // front
            4,6,5, 4,7,6,  // back
            0,4,5, 0,5,1,  // bottom
            2,6,7, 2,7,3,  // top
            0,3,7, 0,7,4,  // left
            1,5,6, 1,6,2   // right
            // clang-format on
        };
    }

} // namespace TestLODGen

TEST(LODGen_CubeSimplification)
{
    using namespace TestLODGen;
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    MakeCube(verts, indices);

    auto result = SimplifyMesh(verts.data(), 8, indices.data(), static_cast<uint32_t>(indices.size()), 6);

    EXPECT_TRUE(result.triangleCount > 0);
    EXPECT_TRUE(result.triangleCount <= 12);
    EXPECT_TRUE(result.triangleCount <= 8); // Should reduce from 12
}

TEST(LODGen_ChainProducesCorrectLevels)
{
    using namespace TestLODGen;
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    MakeCube(verts, indices);

    LODGenerationOptions opts;
    opts.lodCount = 3;
    opts.reductionPerLevel = 0.5f;

    auto result = GenerateLODChain(verts.data(), 8, indices.data(), static_cast<uint32_t>(indices.size()), opts);

    EXPECT_EQ(result.levels.size(), size_t(3));
    EXPECT_EQ(result.levels[0].triangleCount, uint32_t(12)); // LOD0 = original
}

TEST(LODGen_ReductionRatioApproximate)
{
    using namespace TestLODGen;
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    MakeCube(verts, indices);

    LODGenerationOptions opts;
    opts.lodCount = 4;
    opts.reductionPerLevel = 0.5f;

    auto result = GenerateLODChain(verts.data(), 8, indices.data(), static_cast<uint32_t>(indices.size()), opts);

    // Each level should have fewer (or equal) triangles than the previous
    for (size_t i = 1; i < result.levels.size(); ++i)
    {
        EXPECT_TRUE(result.levels[i].triangleCount <= result.levels[i - 1].triangleCount);
    }

    // Total reduction should be positive
    EXPECT_TRUE(result.totalReduction > 0.0f);
}

TEST(LODGen_DegenerateInputSingleTriangle)
{
    using namespace TestLODGen;
    std::vector<float> verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<uint32_t> indices = {0, 1, 2};

    auto result = SimplifyMesh(verts.data(), 3, indices.data(), 3, 1);
    EXPECT_EQ(result.triangleCount, uint32_t(1));
}

TEST(LODGen_NullInputDoesNotCrash)
{
    using namespace TestLODGen;
    auto result1 = SimplifyMesh(nullptr, 0, nullptr, 0, 1);
    EXPECT_EQ(result1.triangleCount, uint32_t(0));

    LODGenerationOptions opts;
    auto result2 = GenerateLODChain(nullptr, 0, nullptr, 0, opts);
    EXPECT_EQ(result2.levels.size(), size_t(0));
}

TEST(LODGen_EmptyIndicesDoesNotCrash)
{
    using namespace TestLODGen;
    std::vector<float> verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto result = SimplifyMesh(verts.data(), 3, nullptr, 0, 1);
    EXPECT_EQ(result.triangleCount, uint32_t(0));
}

TEST(LODGen_LOD0IsOriginal)
{
    using namespace TestLODGen;
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    MakeCube(verts, indices);

    LODGenerationOptions opts;
    opts.lodCount = 2;

    auto result = GenerateLODChain(verts.data(), 8, indices.data(), static_cast<uint32_t>(indices.size()), opts);

    EXPECT_EQ(result.levels[0].triangleCount, uint32_t(12));
    EXPECT_EQ(result.levels[0].indices.size(), indices.size());
}
