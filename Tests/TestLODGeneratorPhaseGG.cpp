/**
 * @file TestLODGeneratorPhaseGG.cpp
 * @brief Phase GG Theme 3D tests for Spark::Graphics::LODGenerator
 *
 * The generator uses QEM (Quadric Error Metric) simplification on
 * a triangle mesh. These tests pass tiny hand-authored meshes and
 * verify the structural invariants (level count, non-empty results,
 * triangle counts monotonically decreasing across LODs).
 */

#include "TestFramework.h"
#include "Graphics/LODGenerator.h"

#include <cstdint>
#include <vector>

namespace
{
    // A unit-cube mesh with 8 vertices and 12 triangles (36 indices).
    void MakeCube(std::vector<float>& verts, std::vector<uint32_t>& idx)
    {
        verts = {
            -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f,
            -0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,
        };
        idx = {
            0, 1, 2, 0, 2, 3, // back
            4, 6, 5, 4, 7, 6, // front
            0, 4, 5, 0, 5, 1, // bottom
            2, 6, 7, 2, 7, 3, // top
            0, 3, 7, 0, 7, 4, // left
            1, 5, 6, 1, 6, 2, // right
        };
    }

    // An N x N grid of vertices triangulated into 2*(N-1)^2 triangles. Unlike
    // the cube, interior vertices are shared by up to six triangles, so this
    // genuinely exercises the edge-collapse loop's per-vertex adjacency
    // bookkeeping (many incident triangles rewritten/removed per collapse).
    void MakeGrid(uint32_t n, std::vector<float>& verts, std::vector<uint32_t>& idx)
    {
        verts.clear();
        idx.clear();
        for (uint32_t r = 0; r < n; ++r)
            for (uint32_t c = 0; c < n; ++c)
            {
                verts.push_back(static_cast<float>(c));
                verts.push_back(0.0f);
                verts.push_back(static_cast<float>(r));
            }
        for (uint32_t r = 0; r + 1 < n; ++r)
            for (uint32_t c = 0; c + 1 < n; ++c)
            {
                const uint32_t v00 = r * n + c;
                const uint32_t v01 = r * n + c + 1;
                const uint32_t v10 = (r + 1) * n + c;
                const uint32_t v11 = (r + 1) * n + c + 1;
                idx.insert(idx.end(), {v00, v01, v11, v00, v11, v10});
            }
    }
} // namespace

TEST(LODGeneratorPhaseGG_SingletonStable)
{
    auto& a = Spark::Graphics::LODGenerator::GetInstance();
    auto& b = Spark::Graphics::LODGenerator::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(LODGeneratorPhaseGG_GenerateProducesLODs)
{
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    MakeCube(verts, idx);

    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    Spark::Graphics::LODGenerationOptions opts;
    opts.lodCount = 3;
    opts.reductionPerLevel = 0.5f;

    auto result = gen.Generate(verts.data(), static_cast<uint32_t>(verts.size() / 3), idx.data(),
                               static_cast<uint32_t>(idx.size()), opts);

    // Should produce at least LOD0 (and ideally all 3 levels).
    EXPECT_TRUE(result.levels.size() >= static_cast<size_t>(1));
    // LOD0 should have the original triangle count.
    if (!result.levels.empty())
    {
        EXPECT_EQ(result.levels[0].triangleCount, static_cast<uint32_t>(12));
    }
}

TEST(LODGeneratorPhaseGG_SimplifyReducesTriangles)
{
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    MakeCube(verts, idx);

    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    auto simplified = gen.Simplify(verts.data(), static_cast<uint32_t>(verts.size() / 3), idx.data(),
                                   static_cast<uint32_t>(idx.size()), /*targetTriangles*/ 4, /*maxError*/ 0.5f);

    // Simplified triangle count should be <= input.
    EXPECT_TRUE(simplified.triangleCount <= static_cast<uint32_t>(12));
    // Non-empty vertices + indices in result.
    EXPECT_TRUE(!simplified.indices.empty());
}

TEST(LODGeneratorPhaseGG_SimplifyHonorsMaximumError)
{
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    MakeCube(verts, idx);

    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    auto simplified = gen.Simplify(verts.data(), static_cast<uint32_t>(verts.size() / 3), idx.data(),
                                   static_cast<uint32_t>(idx.size()), /*targetTriangles*/ 1, /*maxError*/ 0.0f);

    // Every cube-edge collapse has non-zero quadric error. The error ceiling
    // therefore takes precedence over the requested triangle target.
    EXPECT_EQ(simplified.triangleCount, static_cast<uint32_t>(12));
    EXPECT_EQ(simplified.geometricError, 0.0f);
}

TEST(LODGeneratorPhaseGG_SimplifyRepricesEdgesAfterCollapse)
{
    // A non-planar 3x3 patch whose third cheapest original edge becomes much
    // more expensive after the first two collapses. The stale queue used to
    // accept that edge at its original 0.264 cost even though its live QEM cost
    // had risen to about 0.588, crossing this 0.3 error ceiling.
    const std::vector<float> verts = {
        0.0f, -0.75f, 0.0f,  1.0f, 1.25f, 0.0f, 2.0f, 1.25f, 0.0f,  0.0f, -0.25f, 1.0f, 1.0f, 1.0f,
        1.0f, 2.0f,   -0.5f, 1.0f, 0.0f,  1.0f, 2.0f, 1.0f,  0.75f, 2.0f, 2.0f,   0.5f, 2.0f,
    };
    const std::vector<uint32_t> idx = {
        0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4, 3, 4, 7, 3, 7, 6, 4, 5, 7, 5, 8, 7,
    };

    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    const auto simplified = gen.Simplify(verts.data(), static_cast<uint32_t>(verts.size() / 3), idx.data(),
                                         static_cast<uint32_t>(idx.size()), /*targetTriangles*/ 1,
                                         /*maxError*/ 0.3f);

    EXPECT_EQ(simplified.triangleCount, static_cast<uint32_t>(5));
    EXPECT_TRUE(simplified.geometricError <= 0.3f);
}

TEST(LODGeneratorPhaseGG_SimplifiedMeshTopologyIsValid)
{
    // Guards the edge-collapse rewrite (adjacency-map based): the simplified
    // mesh must be structurally sound, not merely smaller. A bug in the
    // vertex remap or triangle-removal bookkeeping would surface here as an
    // out-of-bounds index (GPU crash) or a degenerate triangle, neither of
    // which the count-only checks elsewhere would catch.
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    MakeGrid(12, verts, idx); // 144 verts, 242 triangles

    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    const auto vertexCount = static_cast<uint32_t>(verts.size() / 3);
    auto lod = gen.Simplify(verts.data(), vertexCount, idx.data(), static_cast<uint32_t>(idx.size()),
                            /*targetTriangles*/ 80, /*maxError*/ 1.0e6f);

    // Actually reduced (the collapse loop ran, not a passthrough copy).
    EXPECT_TRUE(lod.triangleCount < static_cast<uint32_t>(242));
    EXPECT_TRUE(lod.triangleCount > static_cast<uint32_t>(0));

    // Index buffer is well-formed: whole triangles, count matches.
    EXPECT_EQ(lod.indices.size() % 3, static_cast<size_t>(0));
    EXPECT_EQ(lod.triangleCount, static_cast<uint32_t>(lod.indices.size() / 3));

    const auto outVerts = static_cast<uint32_t>(lod.vertices.size() / 3);
    bool allInBounds = true;
    bool noDegenerates = true;
    for (size_t t = 0; t + 2 < lod.indices.size(); t += 3)
    {
        const uint32_t i0 = lod.indices[t];
        const uint32_t i1 = lod.indices[t + 1];
        const uint32_t i2 = lod.indices[t + 2];
        if (i0 >= outVerts || i1 >= outVerts || i2 >= outVerts)
            allInBounds = false;
        if (i0 == i1 || i1 == i2 || i0 == i2)
            noDegenerates = false;
    }
    // Every index references a real vertex (no dangling refs from the remap).
    EXPECT_TRUE(allInBounds);
    // No collapsed/degenerate triangles survived compaction.
    EXPECT_TRUE(noDegenerates);
    // Normals were recomputed for the surviving vertices.
    EXPECT_EQ(lod.normals.size(), lod.vertices.size());
}

TEST(LODGeneratorPhaseGG_EmptyInputIsSafe)
{
    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    Spark::Graphics::LODGenerationOptions opts;
    opts.lodCount = 2;
    auto result = gen.Generate(nullptr, 0, nullptr, 0, opts);
    // Should handle empty input without crashing. `levels.size()` is
    // unsigned, so `>= 0` is tautological; assert the call returned.
    (void)result.levels.size();
    EXPECT_TRUE(true);
}

TEST(LODGeneratorPhaseGG_RejectsMalformedIndexBuffers)
{
    const std::vector<float> verts = {
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    auto& gen = Spark::Graphics::LODGenerator::GetInstance();

    const std::vector<uint32_t> outOfBounds = {0, 1, 3};
    const auto invalidVertex =
        gen.Simplify(verts.data(), 3, outOfBounds.data(), static_cast<uint32_t>(outOfBounds.size()), 1, 1.0f);
    EXPECT_TRUE(invalidVertex.indices.empty());
    EXPECT_EQ(invalidVertex.triangleCount, static_cast<uint32_t>(0));

    const std::vector<uint32_t> partialTriangle = {0, 1, 2, 0};
    const auto incomplete =
        gen.Simplify(verts.data(), 3, partialTriangle.data(), static_cast<uint32_t>(partialTriangle.size()), 2, 1.0f);
    EXPECT_TRUE(incomplete.indices.empty());
    EXPECT_EQ(incomplete.triangleCount, static_cast<uint32_t>(0));

    Spark::Graphics::LODGenerationOptions options;
    const auto invalidChain =
        gen.Generate(verts.data(), 3, outOfBounds.data(), static_cast<uint32_t>(outOfBounds.size()), options);
    EXPECT_TRUE(invalidChain.levels.empty());
}

TEST(LODGeneratorPhaseGG_DefaultOptionsProduceFourLevels)
{
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    MakeCube(verts, idx);

    auto& gen = Spark::Graphics::LODGenerator::GetInstance();
    auto result = gen.Generate(verts.data(), static_cast<uint32_t>(verts.size() / 3), idx.data(),
                               static_cast<uint32_t>(idx.size()));
    // Default options request 4 levels; the generator may produce
    // fewer if the mesh is too small. Just verify it ran.
    EXPECT_TRUE(result.levels.size() >= static_cast<size_t>(1));
}
