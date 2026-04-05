/**
 * @file LODGenerator.cpp
 * @brief QEM mesh simplification and LOD chain generation
 */

#include "LODGenerator.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_set>
#include <vector>

namespace Spark::Graphics
{

    LODGenerator& LODGenerator::GetInstance()
    {
        static LODGenerator instance;
        return instance;
    }

    // ============================================================================
    // Quadric computation
    // ============================================================================

    LODGenerator::Quadric LODGenerator::ComputeFaceQuadric(const float* p0, const float* p1, const float* p2)
    {
        // Compute face normal via cross product
        double e1x = static_cast<double>(p1[0]) - p0[0];
        double e1y = static_cast<double>(p1[1]) - p0[1];
        double e1z = static_cast<double>(p1[2]) - p0[2];
        double e2x = static_cast<double>(p2[0]) - p0[0];
        double e2y = static_cast<double>(p2[1]) - p0[1];
        double e2z = static_cast<double>(p2[2]) - p0[2];

        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;

        double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-12)
        {
            return Quadric{};
        }

        nx /= len;
        ny /= len;
        nz /= len;
        double d = -(nx * p0[0] + ny * p0[1] + nz * p0[2]);

        // Quadric Q = p * p^T where p = (a, b, c, d) is the plane equation
        // Stored as upper triangle of 4x4 symmetric matrix (10 elements)
        Quadric q;
        q.data[0] = nx * nx; // (0,0)
        q.data[1] = nx * ny; // (0,1)
        q.data[2] = nx * nz; // (0,2)
        q.data[3] = nx * d;  // (0,3)
        q.data[4] = ny * ny; // (1,1)
        q.data[5] = ny * nz; // (1,2)
        q.data[6] = ny * d;  // (1,3)
        q.data[7] = nz * nz; // (2,2)
        q.data[8] = nz * d;  // (2,3)
        q.data[9] = d * d;   // (3,3)
        return q;
    }

    double LODGenerator::EvaluateQuadric(const Quadric& q, const float* pos)
    {
        double x = pos[0], y = pos[1], z = pos[2];
        // v^T * Q * v where v = (x, y, z, 1)
        double result = q.data[0] * x * x + 2.0 * q.data[1] * x * y + 2.0 * q.data[2] * x * z + 2.0 * q.data[3] * x +
                        q.data[4] * y * y + 2.0 * q.data[5] * y * z + 2.0 * q.data[6] * y + q.data[7] * z * z +
                        2.0 * q.data[8] * z + q.data[9];
        return std::max(result, 0.0);
    }

    // ============================================================================
    // Normal recomputation
    // ============================================================================

    std::vector<float> LODGenerator::RecomputeNormals(const float* vertices, uint32_t vertexCount,
                                                      const uint32_t* indices, uint32_t indexCount)
    {
        std::vector<float> normals(static_cast<size_t>(vertexCount) * 3, 0.0f);
        uint32_t triCount = indexCount / 3;

        for (uint32_t t = 0; t < triCount; ++t)
        {
            uint32_t i0 = indices[t * 3 + 0];
            uint32_t i1 = indices[t * 3 + 1];
            uint32_t i2 = indices[t * 3 + 2];

            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                continue;

            const float* p0 = &vertices[i0 * 3];
            const float* p1 = &vertices[i1 * 3];
            const float* p2 = &vertices[i2 * 3];

            float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
            float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];

            // Face normal (area-weighted via cross product magnitude)
            float nx = e1y * e2z - e1z * e2y;
            float ny = e1z * e2x - e1x * e2z;
            float nz = e1x * e2y - e1y * e2x;

            normals[i0 * 3 + 0] += nx;
            normals[i0 * 3 + 1] += ny;
            normals[i0 * 3 + 2] += nz;
            normals[i1 * 3 + 0] += nx;
            normals[i1 * 3 + 1] += ny;
            normals[i1 * 3 + 2] += nz;
            normals[i2 * 3 + 0] += nx;
            normals[i2 * 3 + 1] += ny;
            normals[i2 * 3 + 2] += nz;
        }

        // Normalize
        for (uint32_t v = 0; v < vertexCount; ++v)
        {
            float* n = &normals[v * 3];
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-6f)
            {
                n[0] /= len;
                n[1] /= len;
                n[2] /= len;
            }
            else
            {
                n[0] = 0.0f;
                n[1] = 1.0f;
                n[2] = 0.0f;
            }
        }

        return normals;
    }

    // ============================================================================
    // Single-mesh simplification
    // ============================================================================

    GeneratedLOD LODGenerator::Simplify(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                        uint32_t indexCount, uint32_t targetTriangles, float maxError)
    {
        GeneratedLOD result;

        if (!vertices || !indices || vertexCount == 0 || indexCount < 3)
        {
            SPARK_LOG_WARN(
                Spark::LogCategory::Graphics, "LODGenerator::Simplify — invalid input (verts=%p, idx=%p, vc=%u, ic=%u)",
                static_cast<const void*>(vertices), static_cast<const void*>(indices), vertexCount, indexCount);
            result.triangleCount = 0;
            result.geometricError = 0.0f;
            return result;
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics,
                        "LODGenerator::Simplify — %u verts, %u indices, target=%u tris, maxErr=%.4f", vertexCount,
                        indexCount, targetTriangles, maxError);

        uint32_t triCount = indexCount / 3;
        if (triCount <= targetTriangles)
        {
            // Already at or below target — return copy of input
            result.vertices.assign(vertices, vertices + vertexCount * 3);
            result.indices.assign(indices, indices + indexCount);
            result.triangleCount = triCount;
            result.geometricError = 0.0f;
            result.normals = RecomputeNormals(vertices, vertexCount, indices, indexCount);
            return result;
        }

        // Build working copies
        std::vector<float> verts(vertices, vertices + vertexCount * 3);
        std::vector<uint32_t> tris(indices, indices + indexCount);

        // Compute per-vertex quadrics
        std::vector<Quadric> quadrics(vertexCount);
        for (uint32_t t = 0; t < triCount; ++t)
        {
            uint32_t i0 = tris[t * 3 + 0];
            uint32_t i1 = tris[t * 3 + 1];
            uint32_t i2 = tris[t * 3 + 2];

            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                continue;

            Quadric fq = ComputeFaceQuadric(&verts[i0 * 3], &verts[i1 * 3], &verts[i2 * 3]);
            quadrics[i0] += fq;
            quadrics[i1] += fq;
            quadrics[i2] += fq;
        }

        // Build edge collapse priority queue
        // For each edge, cost = evaluate combined quadric at midpoint
        struct EdgeCollapse
        {
            double cost;
            uint32_t v0, v1;
            float midpoint[3];
            bool operator>(const EdgeCollapse& other) const { return cost > other.cost; }
        };

        std::priority_queue<EdgeCollapse, std::vector<EdgeCollapse>, std::greater<>> pq;
        std::vector<bool> vertexRemoved(vertexCount, false);

        // Collect unique edges from triangles
        auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t
        {
            if (a > b)
                std::swap(a, b);
            return (static_cast<uint64_t>(a) << 32) | b;
        };

        std::unordered_set<uint64_t> seenEdges;
        for (uint32_t t = 0; t < triCount; ++t)
        {
            uint32_t idx[3] = {tris[t * 3], tris[t * 3 + 1], tris[t * 3 + 2]};
            for (int e = 0; e < 3; ++e)
            {
                uint32_t a = idx[e], b = idx[(e + 1) % 3];
                uint64_t key = edgeKey(a, b);
                if (seenEdges.insert(key).second)
                {
                    Quadric combined = quadrics[a];
                    combined += quadrics[b];

                    float mid[3] = {(verts[a * 3] + verts[b * 3]) * 0.5f, (verts[a * 3 + 1] + verts[b * 3 + 1]) * 0.5f,
                                    (verts[a * 3 + 2] + verts[b * 3 + 2]) * 0.5f};

                    double cost = EvaluateQuadric(combined, mid);
                    pq.push({cost, a, b, {mid[0], mid[1], mid[2]}});
                }
            }
        }

        // Collapse edges until target is reached
        uint32_t currentTriCount = triCount;
        float maxObservedError = 0.0f;

        while (currentTriCount > targetTriangles && !pq.empty())
        {
            EdgeCollapse top = pq.top();
            pq.pop();

            if (vertexRemoved[top.v0] || vertexRemoved[top.v1])
                continue;

            if (top.cost > static_cast<double>(maxError) && currentTriCount <= targetTriangles)
                break;

            // Collapse v1 into v0: move v0 to midpoint, remove v1
            verts[top.v0 * 3 + 0] = top.midpoint[0];
            verts[top.v0 * 3 + 1] = top.midpoint[1];
            verts[top.v0 * 3 + 2] = top.midpoint[2];

            quadrics[top.v0] += quadrics[top.v1];
            vertexRemoved[top.v1] = true;

            maxObservedError = std::max(maxObservedError, static_cast<float>(top.cost));

            // Update all triangles referencing v1 to reference v0
            uint32_t removedTris = 0;
            for (uint32_t t = 0; t < triCount; ++t)
            {
                uint32_t* tri = &tris[t * 3];
                bool hasV0 = false, hasV1 = false;

                for (int i = 0; i < 3; ++i)
                {
                    if (tri[i] == top.v0)
                        hasV0 = true;
                    if (tri[i] == top.v1)
                    {
                        tri[i] = top.v0;
                        hasV1 = true;
                    }
                }

                // Degenerate triangle (has v0 twice now)
                if (hasV0 && hasV1)
                {
                    tri[0] = tri[1] = tri[2] = 0;
                    removedTris++;
                }
            }

            currentTriCount -= removedTris;
        }

        // Compact: remove degenerate triangles and unused vertices
        std::vector<uint32_t> finalIndices;
        finalIndices.reserve(currentTriCount * 3);

        for (uint32_t t = 0; t < triCount; ++t)
        {
            uint32_t i0 = tris[t * 3], i1 = tris[t * 3 + 1], i2 = tris[t * 3 + 2];
            if (i0 == i1 || i1 == i2 || i0 == i2)
                continue;
            if (vertexRemoved[i0] || vertexRemoved[i1] || vertexRemoved[i2])
                continue;
            finalIndices.push_back(i0);
            finalIndices.push_back(i1);
            finalIndices.push_back(i2);
        }

        // Remap vertices to remove gaps
        std::vector<uint32_t> remap(vertexCount, UINT32_MAX);
        uint32_t newVertCount = 0;
        for (uint32_t idx : finalIndices)
        {
            if (remap[idx] == UINT32_MAX)
            {
                remap[idx] = newVertCount++;
            }
        }

        result.vertices.resize(static_cast<size_t>(newVertCount) * 3);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            if (remap[i] != UINT32_MAX)
            {
                result.vertices[remap[i] * 3 + 0] = verts[i * 3 + 0];
                result.vertices[remap[i] * 3 + 1] = verts[i * 3 + 1];
                result.vertices[remap[i] * 3 + 2] = verts[i * 3 + 2];
            }
        }

        result.indices.resize(finalIndices.size());
        for (size_t i = 0; i < finalIndices.size(); ++i)
        {
            result.indices[i] = remap[finalIndices[i]];
        }

        result.triangleCount = static_cast<uint32_t>(result.indices.size() / 3);
        result.geometricError = maxObservedError;
        result.normals = RecomputeNormals(result.vertices.data(), newVertCount, result.indices.data(),
                                          static_cast<uint32_t>(result.indices.size()));

        return result;
    }

    // ============================================================================
    // LOD Chain Generation
    // ============================================================================

    LODChainResult LODGenerator::Generate(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                          uint32_t indexCount, const LODGenerationOptions& options)
    {
        LODChainResult result;

        if (!vertices || !indices || vertexCount == 0 || indexCount < 3)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "LODGenerator::Generate — invalid mesh data");
            return result;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LODGenerator::Generate — %u verts, %u tris, %u LOD levels",
                       vertexCount, indexCount / 3, options.lodCount);

        uint32_t originalTriCount = indexCount / 3;
        uint32_t lodCount = std::max(options.lodCount, 1u);

        result.levels.reserve(lodCount);
        result.screenSizeThresholds.resize(lodCount);

        // LOD0 = original mesh
        GeneratedLOD lod0;
        lod0.vertices.assign(vertices, vertices + vertexCount * 3);
        lod0.indices.assign(indices, indices + indexCount);
        lod0.triangleCount = originalTriCount;
        lod0.geometricError = 0.0f;
        lod0.normals = RecomputeNormals(vertices, vertexCount, indices, indexCount);
        result.levels.push_back(std::move(lod0));

        if (lodCount < options.screenSizeThresholds.size())
        {
            result.screenSizeThresholds[0] = options.screenSizeThresholds[0];
        }
        else
        {
            result.screenSizeThresholds[0] = 1.0f;
        }

        // Generate subsequent LOD levels
        float cumulativeReduction = 1.0f;
        for (uint32_t level = 1; level < lodCount; ++level)
        {
            cumulativeReduction *= options.reductionPerLevel;
            uint32_t targetTris = std::max(static_cast<uint32_t>(originalTriCount * cumulativeReduction), 1u);

            GeneratedLOD lod = Simplify(vertices, vertexCount, indices, indexCount, targetTris, options.maxError);
            result.levels.push_back(std::move(lod));

            if (level < options.screenSizeThresholds.size())
            {
                result.screenSizeThresholds[level] = options.screenSizeThresholds[level];
            }
            else
            {
                result.screenSizeThresholds[level] = 1.0f / static_cast<float>(1 << level);
            }
        }

        // Calculate total reduction
        if (!result.levels.empty() && originalTriCount > 0)
        {
            uint32_t lowestTris = result.levels.back().triangleCount;
            result.totalReduction = 1.0f - static_cast<float>(lowestTris) / static_cast<float>(originalTriCount);
        }

        return result;
    }

} // namespace Spark::Graphics
