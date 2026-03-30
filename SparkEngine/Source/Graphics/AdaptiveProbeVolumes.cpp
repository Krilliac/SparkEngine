/**
 * @file AdaptiveProbeVolumes.cpp
 * @brief Implementation of brick-based hierarchical probe volumes
 *
 * Moves method bodies out of AdaptiveProbeVolumes.h to reduce header
 * compilation cost. Provides brick creation, subdivision, virtual offset
 * application, camera-based streaming, and trilinear SH sampling.
 */

#include "AdaptiveProbeVolumes.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // Public Methods
    // =========================================================================

    bool AdaptiveProbeVolumes::Initialize(const APVSettings& settings)
    {
        m_settings = settings;
        m_bricks.clear();
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "AdaptiveProbeVolumes initialized (streamingDist=%.1f, virtualOffsetDist=%.2f)",
                       settings.streamingDistance, settings.virtualOffsetDistance);
        return true;
    }

    void AdaptiveProbeVolumes::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "AdaptiveProbeVolumes shutting down (%zu bricks)",
                       m_bricks.size());
        m_bricks.clear();
        m_initialized = false;
    }

    bool AdaptiveProbeVolumes::CreateBrick(int32_t gridX, int32_t gridY, int32_t gridZ, BrickLOD lod)
    {
        if (!m_initialized)
            return false;

        BrickID id{gridX, gridY, gridZ, lod};
        if (m_bricks.contains(id))
            return false;

        float spacing = BrickLODSpacing(lod);
        float brickSize = spacing * (APV_PROBES_PER_BRICK_AXIS - 1);

        APVBrick brick;
        brick.id = id;
        brick.spacing = spacing;
        brick.originX = gridX * brickSize;
        brick.originY = gridY * brickSize;
        brick.originZ = gridZ * brickSize;
        brick.loaded = true;

        m_bricks[id] = std::move(brick);
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "APV brick created at (%d,%d,%d) LOD=%d spacing=%.2f", gridX,
                        gridY, gridZ, static_cast<int>(lod), spacing);
        return true;
    }

    bool AdaptiveProbeVolumes::SubdivideBrick(int32_t gridX, int32_t gridY, int32_t gridZ, BrickLOD parentLOD)
    {
        if (!m_initialized || parentLOD == BrickLOD::Fine)
            return false;

        BrickID parentID{gridX, gridY, gridZ, parentLOD};
        auto parentIt = m_bricks.find(parentID);
        if (parentIt == m_bricks.end())
            return false;

        parentIt->second.subdivided = true;
        BrickLOD childLOD = static_cast<BrickLOD>(static_cast<int>(parentLOD) - 1);

        // Each parent brick maps to 8 child bricks (2x2x2)
        for (int cz = 0; cz < 2; ++cz)
        {
            for (int cy = 0; cy < 2; ++cy)
            {
                for (int cx = 0; cx < 2; ++cx)
                {
                    int32_t childX = gridX * 2 + cx;
                    int32_t childY = gridY * 2 + cy;
                    int32_t childZ = gridZ * 2 + cz;

                    CreateBrick(childX, childY, childZ, childLOD);

                    // Initialize child probes from parent via trilinear interpolation
                    BrickID childID{childX, childY, childZ, childLOD};
                    auto childIt = m_bricks.find(childID);
                    if (childIt != m_bricks.end())
                    {
                        InterpolateBrickFromParent(childIt->second, parentIt->second);
                    }
                }
            }
        }

        return true;
    }

    void AdaptiveProbeVolumes::ApplyVirtualOffsets(const BrickID& id, const float* geometryDistances,
                                                   const float* geometryNormals)
    {
        if (!m_initialized)
            return;

        auto it = m_bricks.find(id);
        if (it == m_bricks.end())
            return;

        APVBrick& brick = it->second;

        for (int i = 0; i < APV_PROBES_PER_BRICK; ++i)
        {
            APVProbeData& probe = brick.probes[i];
            float dist = geometryDistances[i];

            if (dist < 0.0f)
            {
                // Probe is inside geometry: mark invalid
                probe.valid = false;
                probe.offsetX = 0.0f;
                probe.offsetY = 0.0f;
                probe.offsetZ = 0.0f;
                continue;
            }

            if (dist < m_settings.virtualOffsetDistance)
            {
                float nx = geometryNormals[i * 3 + 0];
                float ny = geometryNormals[i * 3 + 1];
                float nz = geometryNormals[i * 3 + 2];

                float pushDist = m_settings.virtualOffsetDistance - dist + 0.05f;
                pushDist = std::min(pushDist, m_settings.virtualOffsetSearchRadius);

                probe.offsetX = nx * pushDist;
                probe.offsetY = ny * pushDist;
                probe.offsetZ = nz * pushDist;
            }

            probe.valid = true;
        }
    }

    void AdaptiveProbeVolumes::UpdateStreaming(float cameraPosX, float cameraPosY, float cameraPosZ)
    {
        if (!m_initialized)
            return;

        float maxDistSq = m_settings.streamingDistance * m_settings.streamingDistance;

        // Unload bricks that are too far from camera
        std::vector<BrickID> toUnload;
        for (auto& [id, brick] : m_bricks)
        {
            float halfExtent = brick.spacing * (APV_PROBES_PER_BRICK_AXIS - 1) * 0.5f;
            float cx = brick.originX + halfExtent;
            float cy = brick.originY + halfExtent;
            float cz = brick.originZ + halfExtent;

            float dx = cx - cameraPosX;
            float dy = cy - cameraPosY;
            float dz = cz - cameraPosZ;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq > maxDistSq)
            {
                toUnload.push_back(id);
            }
        }

        for (const BrickID& id : toUnload)
        {
            m_bricks.erase(id);
        }

        if (!toUnload.empty())
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "APV streaming: unloaded %zu bricks, %zu remaining",
                            toUnload.size(), m_bricks.size());
        }
    }

    void AdaptiveProbeVolumes::SetProbeData(const BrickID& id, int probeX, int probeY, int probeZ,
                                            const float shR[APV_SH_COEFFICIENTS], const float shG[APV_SH_COEFFICIENTS],
                                            const float shB[APV_SH_COEFFICIENTS])
    {
        auto it = m_bricks.find(id);
        if (it == m_bricks.end())
            return;

        int localIdx = LocalProbeIndex(probeX, probeY, probeZ);
        APVProbeData& probe = it->second.probes[localIdx];

        for (int i = 0; i < APV_SH_COEFFICIENTS; ++i)
        {
            probe.r[i] = shR[i];
            probe.g[i] = shG[i];
            probe.b[i] = shB[i];
        }
    }

    APVIrradiance AdaptiveProbeVolumes::SampleIrradiance(float worldX, float worldY, float worldZ, float normalX,
                                                         float normalY, float normalZ) const
    {
        APVIrradiance result{};
        if (!m_initialized)
            return result;

        // Search from finest to coarsest LOD
        for (int lodIdx = 0; lodIdx < static_cast<int>(BrickLOD::Count); ++lodIdx)
        {
            BrickLOD lod = static_cast<BrickLOD>(lodIdx);
            float spacing = BrickLODSpacing(lod);
            float brickSize = spacing * (APV_PROBES_PER_BRICK_AXIS - 1);

            int32_t bx = static_cast<int32_t>(std::floor(worldX / brickSize));
            int32_t by = static_cast<int32_t>(std::floor(worldY / brickSize));
            int32_t bz = static_cast<int32_t>(std::floor(worldZ / brickSize));

            BrickID id{bx, by, bz, lod};
            auto it = m_bricks.find(id);
            if (it == m_bricks.end() || it->second.subdivided)
                continue;

            const APVBrick& brick = it->second;

            // Local position within brick
            float localX = (worldX - brick.originX) / brick.spacing;
            float localY = (worldY - brick.originY) / brick.spacing;
            float localZ = (worldZ - brick.originZ) / brick.spacing;

            float maxIdx = static_cast<float>(APV_PROBES_PER_BRICK_AXIS - 1);
            localX = std::clamp(localX, 0.0f, maxIdx);
            localY = std::clamp(localY, 0.0f, maxIdx);
            localZ = std::clamp(localZ, 0.0f, maxIdx);

            int px0 = static_cast<int>(localX);
            int py0 = static_cast<int>(localY);
            int pz0 = static_cast<int>(localZ);
            int px1 = std::min(px0 + 1, APV_PROBES_PER_BRICK_AXIS - 1);
            int py1 = std::min(py0 + 1, APV_PROBES_PER_BRICK_AXIS - 1);
            int pz1 = std::min(pz0 + 1, APV_PROBES_PER_BRICK_AXIS - 1);

            float fx = localX - px0;
            float fy = localY - py0;
            float fz = localZ - pz0;

            float weights[8] = {
                (1 - fx) * (1 - fy) * (1 - fz), fx * (1 - fy) * (1 - fz), (1 - fx) * fy * (1 - fz), fx * fy * (1 - fz),
                (1 - fx) * (1 - fy) * fz,       fx * (1 - fy) * fz,       (1 - fx) * fy * fz,       fx * fy * fz,
            };

            int localIndices[8] = {
                LocalProbeIndex(px0, py0, pz0), LocalProbeIndex(px1, py0, pz0), LocalProbeIndex(px0, py1, pz0),
                LocalProbeIndex(px1, py1, pz0), LocalProbeIndex(px0, py0, pz1), LocalProbeIndex(px1, py0, pz1),
                LocalProbeIndex(px0, py1, pz1), LocalProbeIndex(px1, py1, pz1),
            };

            // Evaluate SH basis for the surface normal
            float basis[APV_SH_COEFFICIENTS];
            EvaluateSHBasis(normalX, normalY, normalZ, basis);

            static constexpr float A0 = 3.14159265f;
            static constexpr float A1 = 2.09439510f;
            static constexpr float A2 = 0.78539816f;
            static constexpr float convWeights[APV_SH_COEFFICIENTS] = {A0, A1, A1, A1, A2, A2, A2, A2, A2};

            float totalWeight = 0.0f;

            for (int corner = 0; corner < 8; ++corner)
            {
                const APVProbeData& probe = brick.probes[localIndices[corner]];
                if (!probe.valid)
                    continue;

                float w = weights[corner];
                totalWeight += w;

                float ir = 0.0f, ig = 0.0f, ib = 0.0f;
                for (int c = 0; c < APV_SH_COEFFICIENTS; ++c)
                {
                    float bw = basis[c] * convWeights[c];
                    ir += probe.r[c] * bw;
                    ig += probe.g[c] * bw;
                    ib += probe.b[c] * bw;
                }

                result.r += std::max(0.0f, ir) * w;
                result.g += std::max(0.0f, ig) * w;
                result.b += std::max(0.0f, ib) * w;
            }

            // Normalize if some probes were invalid
            if (totalWeight > 0.0f && totalWeight < 0.999f)
            {
                float invW = 1.0f / totalWeight;
                result.r *= invW;
                result.g *= invW;
                result.b *= invW;
            }

            return result;
        }

        return result;
    }

    // =========================================================================
    // Private Helpers
    // =========================================================================

    int AdaptiveProbeVolumes::LocalProbeIndex(int px, int py, int pz)
    {
        return pz * APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS + py * APV_PROBES_PER_BRICK_AXIS + px;
    }

    void AdaptiveProbeVolumes::EvaluateSHBasis(float nx, float ny, float nz, float outBasis[APV_SH_COEFFICIENTS])
    {
        outBasis[0] = 0.282095f;
        outBasis[1] = 0.488603f * ny;
        outBasis[2] = 0.488603f * nz;
        outBasis[3] = 0.488603f * nx;
        outBasis[4] = 1.092548f * nx * ny;
        outBasis[5] = 1.092548f * ny * nz;
        outBasis[6] = 0.315392f * (3.0f * nz * nz - 1.0f);
        outBasis[7] = 1.092548f * nx * nz;
        outBasis[8] = 0.546274f * (nx * nx - ny * ny);
    }

    void AdaptiveProbeVolumes::InterpolateBrickFromParent(APVBrick& child, const APVBrick& parent) const
    {
        for (int pz = 0; pz < APV_PROBES_PER_BRICK_AXIS; ++pz)
        {
            for (int py = 0; py < APV_PROBES_PER_BRICK_AXIS; ++py)
            {
                for (int px = 0; px < APV_PROBES_PER_BRICK_AXIS; ++px)
                {
                    int childIdx = LocalProbeIndex(px, py, pz);
                    APVProbeData& childProbe = child.probes[childIdx];

                    // Child probe world position
                    float worldX = child.originX + px * child.spacing;
                    float worldY = child.originY + py * child.spacing;
                    float worldZ = child.originZ + pz * child.spacing;

                    // Map to parent local coordinates
                    float lx = (worldX - parent.originX) / parent.spacing;
                    float ly = (worldY - parent.originY) / parent.spacing;
                    float lz = (worldZ - parent.originZ) / parent.spacing;

                    float maxP = static_cast<float>(APV_PROBES_PER_BRICK_AXIS - 1);
                    lx = std::clamp(lx, 0.0f, maxP);
                    ly = std::clamp(ly, 0.0f, maxP);
                    lz = std::clamp(lz, 0.0f, maxP);

                    int x0 = std::min(static_cast<int>(lx), APV_PROBES_PER_BRICK_AXIS - 1);
                    int y0 = std::min(static_cast<int>(ly), APV_PROBES_PER_BRICK_AXIS - 1);
                    int z0 = std::min(static_cast<int>(lz), APV_PROBES_PER_BRICK_AXIS - 1);
                    int x1 = std::min(x0 + 1, APV_PROBES_PER_BRICK_AXIS - 1);
                    int y1 = std::min(y0 + 1, APV_PROBES_PER_BRICK_AXIS - 1);
                    int z1 = std::min(z0 + 1, APV_PROBES_PER_BRICK_AXIS - 1);

                    float fx = lx - x0;
                    float fy = ly - y0;
                    float fz = lz - z0;

                    // Trilinear interpolation of parent SH data
                    float w[8] = {
                        (1 - fx) * (1 - fy) * (1 - fz),
                        fx * (1 - fy) * (1 - fz),
                        (1 - fx) * fy * (1 - fz),
                        fx * fy * (1 - fz),
                        (1 - fx) * (1 - fy) * fz,
                        fx * (1 - fy) * fz,
                        (1 - fx) * fy * fz,
                        fx * fy * fz,
                    };

                    int pi[8] = {
                        LocalProbeIndex(x0, y0, z0), LocalProbeIndex(x1, y0, z0), LocalProbeIndex(x0, y1, z0),
                        LocalProbeIndex(x1, y1, z0), LocalProbeIndex(x0, y0, z1), LocalProbeIndex(x1, y0, z1),
                        LocalProbeIndex(x0, y1, z1), LocalProbeIndex(x1, y1, z1),
                    };

                    childProbe.r.fill(0.0f);
                    childProbe.g.fill(0.0f);
                    childProbe.b.fill(0.0f);

                    for (int corner = 0; corner < 8; ++corner)
                    {
                        const APVProbeData& pp = parent.probes[pi[corner]];
                        for (int c = 0; c < APV_SH_COEFFICIENTS; ++c)
                        {
                            childProbe.r[c] += pp.r[c] * w[corner];
                            childProbe.g[c] += pp.g[c] * w[corner];
                            childProbe.b[c] += pp.b[c] * w[corner];
                        }
                    }

                    childProbe.valid = true;
                }
            }
        }
    }

} // namespace Spark::Graphics
