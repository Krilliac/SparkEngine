// TestMeshShaderPipeline.cpp - Tests for meshlet generation and mesh shader pipeline
// Validates meshlet building, bounding sphere computation, and normal cones

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace
{

    struct GPUMeshlet
    {
        uint32_t vertexOffset;
        uint32_t triangleOffset;
        uint32_t vertexCount;
        uint32_t triangleCount;
        float boundCenter[3];
        float boundRadius;
        float coneAxis[3];
        float coneCutoff;
    };

    // Simplified meshlet builder (mirrors MeshShaderPipeline::BuildMeshlets)
    std::vector<GPUMeshlet> BuildMeshlets(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                          uint32_t indexCount, uint32_t maxVerts = 64, uint32_t maxTris = 124)
    {
        std::vector<GPUMeshlet> meshlets;
        uint32_t triCount = indexCount / 3;
        std::vector<bool> triUsed(triCount, false);

        uint32_t globalVertexOffset = 0;
        uint32_t globalTriOffset = 0;

        for (uint32_t startTri = 0; startTri < triCount;)
        {
            GPUMeshlet meshlet = {};
            std::unordered_set<uint32_t> uniqueVerts;
            uint32_t meshletTriCount = 0;

            for (uint32_t tri = startTri; tri < triCount; tri++)
            {
                if (triUsed[tri])
                    continue;

                uint32_t i0 = indices[tri * 3 + 0];
                uint32_t i1 = indices[tri * 3 + 1];
                uint32_t i2 = indices[tri * 3 + 2];

                uint32_t newVerts = 0;
                if (uniqueVerts.find(i0) == uniqueVerts.end())
                    newVerts++;
                if (uniqueVerts.find(i1) == uniqueVerts.end())
                    newVerts++;
                if (uniqueVerts.find(i2) == uniqueVerts.end())
                    newVerts++;

                if (uniqueVerts.size() + newVerts > maxVerts)
                    continue;
                if (meshletTriCount >= maxTris)
                    break;

                uniqueVerts.insert(i0);
                uniqueVerts.insert(i1);
                uniqueVerts.insert(i2);
                triUsed[tri] = true;
                meshletTriCount++;
            }

            if (uniqueVerts.empty())
            {
                while (startTri < triCount && triUsed[startTri])
                    startTri++;
                continue;
            }

            meshlet.vertexOffset = globalVertexOffset;
            meshlet.triangleOffset = globalTriOffset;
            meshlet.vertexCount = static_cast<uint32_t>(uniqueVerts.size());
            meshlet.triangleCount = meshletTriCount;

            // Compute centroid bounding sphere
            float cx = 0, cy = 0, cz = 0;
            for (uint32_t idx : uniqueVerts)
            {
                cx += vertices[idx * 3 + 0];
                cy += vertices[idx * 3 + 1];
                cz += vertices[idx * 3 + 2];
            }
            float inv = 1.0f / static_cast<float>(uniqueVerts.size());
            meshlet.boundCenter[0] = cx * inv;
            meshlet.boundCenter[1] = cy * inv;
            meshlet.boundCenter[2] = cz * inv;

            float maxDistSq = 0;
            for (uint32_t idx : uniqueVerts)
            {
                float dx = vertices[idx * 3 + 0] - meshlet.boundCenter[0];
                float dy = vertices[idx * 3 + 1] - meshlet.boundCenter[1];
                float dz = vertices[idx * 3 + 2] - meshlet.boundCenter[2];
                maxDistSq = std::max(maxDistSq, dx * dx + dy * dy + dz * dz);
            }
            meshlet.boundRadius = std::sqrt(maxDistSq);

            meshlet.coneCutoff = 1.0f; // disabled

            meshlets.push_back(meshlet);
            globalVertexOffset += meshlet.vertexCount;
            globalTriOffset += meshlet.triangleCount;

            while (startTri < triCount && triUsed[startTri])
                startTri++;
        }

        return meshlets;
    }

} // namespace

TEST(MeshShader_BuildMeshlets_SingleTriangle)
{
    float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    uint32_t indices[] = {0, 1, 2};

    auto meshlets = BuildMeshlets(vertices, 3, indices, 3);

    EXPECT_EQ(meshlets.size(), 1u);
    EXPECT_EQ(meshlets[0].vertexCount, 3u);
    EXPECT_EQ(meshlets[0].triangleCount, 1u);
}

TEST(MeshShader_BuildMeshlets_TwoTriangles)
{
    float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    uint32_t indices[] = {0, 1, 2, 1, 3, 2};

    auto meshlets = BuildMeshlets(vertices, 4, indices, 6);

    EXPECT_EQ(meshlets.size(), 1u);
    EXPECT_EQ(meshlets[0].triangleCount, 2u);
    EXPECT_EQ(meshlets[0].vertexCount, 4u);
}

TEST(MeshShader_BuildMeshlets_SplitsOnVertexLimit)
{
    // Create a mesh with 5 vertices and max 3 verts per meshlet
    float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 0, 0};
    uint32_t indices[] = {0, 1, 2, 2, 1, 3, 3, 1, 4};

    auto meshlets = BuildMeshlets(vertices, 5, indices, 9, 3, 124);

    // Should split into at least 2 meshlets due to 3-vertex limit
    EXPECT_TRUE(meshlets.size() >= 2u);
}

TEST(MeshShader_BuildMeshlets_SplitsOnTriLimit)
{
    // Create 3 triangles with max 2 tris per meshlet
    float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 0, 0, 2, 1, 0};
    uint32_t indices[] = {0, 1, 2, 1, 3, 2, 1, 4, 3};

    auto meshlets = BuildMeshlets(vertices, 6, indices, 9, 64, 2);

    EXPECT_TRUE(meshlets.size() >= 2u);
}

TEST(MeshShader_BoundingSphere_Correct)
{
    // Square from (0,0,0) to (2,2,0)
    float vertices[] = {0, 0, 0, 2, 0, 0, 2, 2, 0, 0, 2, 0};
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};

    auto meshlets = BuildMeshlets(vertices, 4, indices, 6);

    EXPECT_EQ(meshlets.size(), 1u);

    // Center should be roughly (1, 1, 0)
    EXPECT_TRUE(std::abs(meshlets[0].boundCenter[0] - 1.0f) < 0.01f);
    EXPECT_TRUE(std::abs(meshlets[0].boundCenter[1] - 1.0f) < 0.01f);

    // Radius should be sqrt(2) ~= 1.414
    EXPECT_TRUE(meshlets[0].boundRadius > 1.3f);
    EXPECT_TRUE(meshlets[0].boundRadius < 1.5f);
}

TEST(MeshShader_EmptyMesh_NoMeshlets)
{
    auto meshlets = BuildMeshlets(nullptr, 0, nullptr, 0);
    EXPECT_EQ(meshlets.size(), 0u);
}

TEST(MeshShader_GPUMeshlet_Size)
{
    // Must fit in GPU structured buffer efficiently
    EXPECT_EQ(sizeof(GPUMeshlet), 48u);
}

TEST(MeshShader_ConeCutoff_DisabledByDefault)
{
    float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    uint32_t indices[] = {0, 1, 2};

    auto meshlets = BuildMeshlets(vertices, 3, indices, 3);

    // coneCutoff = 1.0 means backface test always passes (disabled)
    EXPECT_NEAR(meshlets[0].coneCutoff, 1.0f, 0.001f);
}

TEST(MeshShader_VertexOffsetsAccumulate)
{
    // Two separate triangles with max 3 verts = 2 meshlets
    float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 5, 0, 0, 6, 0, 0, 5, 1, 0};
    uint32_t indices[] = {0, 1, 2, 3, 4, 5};

    auto meshlets = BuildMeshlets(vertices, 6, indices, 6, 3, 1);

    EXPECT_TRUE(meshlets.size() >= 2u);
    EXPECT_EQ(meshlets[0].vertexOffset, 0u);
    EXPECT_EQ(meshlets[1].vertexOffset, meshlets[0].vertexCount);
}
