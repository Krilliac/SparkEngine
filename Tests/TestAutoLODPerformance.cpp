// TestAutoLODPerformance.cpp - Auto-LOD import performance regression checks

#include "Graphics/LODGenerator.h"
#include "TestFramework.h"


#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace
{
    struct SceneMesh
    {
        uint32_t triangles;
        float vramMB;
    };

    std::vector<float> BuildStripVertices(uint32_t vertexCount)
    {
        std::vector<float> verts;
        verts.reserve(static_cast<size_t>(vertexCount) * 3);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            verts.push_back(static_cast<float>(i % 256) * 0.01f);
            verts.push_back(static_cast<float>((i / 256) % 256) * 0.01f);
            verts.push_back(static_cast<float>(i / (256 * 256)) * 0.01f);
        }
        return verts;
    }

    std::vector<uint32_t> BuildTriangleList(uint32_t triangleCount)
    {
        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(triangleCount) * 3);
        uint32_t v = 0;
        for (uint32_t t = 0; t < triangleCount; ++t)
        {
            indices.push_back(v + 0);
            indices.push_back(v + 1);
            indices.push_back(v + 2);
            v += 3;
        }
        return indices;
    }

    float EstimateFrameMs(const std::vector<SceneMesh>& meshes)
    {
        // Simple deterministic proxy: CPU setup + GPU raster cost.
        float frameMs = 0.85f; // baseline fixed cost
        for (const SceneMesh& m : meshes)
        {
            frameMs += static_cast<float>(m.triangles) * 0.0000022f;
        }
        return frameMs;
    }

    float EstimateVRAMMB(const std::vector<SceneMesh>& meshes)
    {
        return std::accumulate(meshes.begin(), meshes.end(), 0.0f,
                               [](float acc, const SceneMesh& m) { return acc + m.vramMB; });
    }
} // namespace

TEST(AutoLOD_Perf_FrameTimeImprovesAcrossRepresentativeScenes)
{
    std::vector<SceneMesh> baseline = {{180000, 34.0f}, {95000, 19.0f}, {46000, 11.0f}};
    std::vector<SceneMesh> withAutoLOD;

    for (const SceneMesh& mesh : baseline)
    {
        const uint32_t vertexCount = mesh.triangles * 3;
        std::vector<float> vertices = BuildStripVertices(vertexCount);
        std::vector<uint32_t> indices = BuildTriangleList(mesh.triangles);

        Spark::Graphics::LODGenerationOptions options{};
        options.lodCount = 4;
        options.reductionPerLevel = 0.62f;
        options.maxError = 0.015f;

        auto chain = Spark::Graphics::LODGenerator::GetInstance().Generate(
            vertices.data(), vertexCount, indices.data(), static_cast<uint32_t>(indices.size()), options);

        ASSERT_TRUE(!chain.levels.empty());
        // Approximate runtime mix: LOD0 25%, LOD1 35%, LOD2 30%, LOD3 10%
        const auto l0 = static_cast<float>(chain.levels[0].triangleCount);
        const auto l1 =
            static_cast<float>(chain.levels.size() > 1 ? chain.levels[1].triangleCount : chain.levels[0].triangleCount);
        const auto l2 = static_cast<float>(chain.levels.size() > 2 ? chain.levels[2].triangleCount
                                                                   : chain.levels.back().triangleCount);
        const auto l3 = static_cast<float>(chain.levels.size() > 3 ? chain.levels[3].triangleCount
                                                                   : chain.levels.back().triangleCount);

        const uint32_t weightedTriangles = static_cast<uint32_t>(l0 * 0.25f + l1 * 0.35f + l2 * 0.30f + l3 * 0.10f);
        const float weightedVRAM = mesh.vramMB * (0.25f + 0.35f * (l1 / l0) + 0.30f * (l2 / l0) + 0.10f * (l3 / l0));

        withAutoLOD.push_back({weightedTriangles, weightedVRAM});
    }

    uint32_t baseTris = 0, lodTris = 0;
    for (const SceneMesh& m : baseline)
        baseTris += m.triangles;
    for (const SceneMesh& m : withAutoLOD)
        lodTris += m.triangles;

    // What the LOD system actually controls: the weighted triangle load. The
    // best this scene mix can do is
    //   0.25 + 0.35*0.62 + 0.30*0.62^2 + 0.10*0.62^3 = 0.606
    // and the generator hits it (321,000 -> 194,573 triangles, 0.606x), so this
    // is the real regression guard on Simplify's reduction quality.
    EXPECT_LT(static_cast<float>(lodTris), static_cast<float>(baseTris) * 0.65f);

    const float baselineFrameMs = EstimateFrameMs(baseline);
    const float autoLODFrameMs = EstimateFrameMs(withAutoLOD);

    // EstimateFrameMs charges a FIXED 0.85ms that no amount of LOD can remove,
    // so a 0.606x triangle load only buys 1.5562ms -> 1.2781ms = 0.821x frame
    // time. The original `< 0.80f` here did not follow from this model's own
    // arithmetic and could never pass, however good the simplifier was -- which
    // is part of why this file sat unregistered. Assert the win the model can
    // actually express; the triangle-load bound above is the meaningful check.
    EXPECT_LT(autoLODFrameMs, baselineFrameMs * 0.85f);
}

TEST(AutoLOD_Perf_VRAMImprovesAcrossRepresentativeScenes)
{
    std::vector<SceneMesh> baseline = {{220000, 40.0f}, {110000, 22.0f}, {70000, 15.0f}};
    std::vector<SceneMesh> withAutoLOD;

    for (const SceneMesh& mesh : baseline)
    {
        const uint32_t vertexCount = mesh.triangles * 3;
        std::vector<float> vertices = BuildStripVertices(vertexCount);
        std::vector<uint32_t> indices = BuildTriangleList(mesh.triangles);

        Spark::Graphics::LODGenerationOptions options{};
        options.lodCount = 4;
        options.reductionPerLevel = 0.58f;
        options.maxError = 0.02f;

        auto chain = Spark::Graphics::LODGenerator::GetInstance().Generate(
            vertices.data(), vertexCount, indices.data(), static_cast<uint32_t>(indices.size()), options);

        ASSERT_TRUE(chain.levels.size() >= 2);

        const float lod0 = static_cast<float>(chain.levels[0].triangleCount);
        const float lod2 = static_cast<float>(chain.levels[2].triangleCount);
        const float lod3 = static_cast<float>(chain.levels[3].triangleCount);

        // Streaming keeps low LODs resident for distance buckets.
        const float residentFactor = 0.30f + 0.45f * (lod2 / lod0) + 0.25f * (lod3 / lod0);
        withAutoLOD.push_back({chain.levels[2].triangleCount, mesh.vramMB * residentFactor});
    }

    const float baselineVRAM = EstimateVRAMMB(baseline);
    const float autoLODVRAM = EstimateVRAMMB(withAutoLOD);
    EXPECT_TRUE(autoLODVRAM < baselineVRAM * 0.75f);
}
