/**
 * @file AdaptiveProbeVolumes.h
 * @brief Brick-based hierarchical probe system with adaptive subdivision
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements an adaptive probe volume system inspired by Unity's APV.
 * Probes are organized in bricks (4x4x4 = 64 probes each) with multiple
 * LOD levels for varying spatial density. Bricks near geometry are subdivided
 * to finer spacing while open areas use coarser spacing.
 *
 * Features:
 * - Brick-based hierarchy with 3 LOD levels (1m, 2m, 4m spacing)
 * - Adaptive subdivision based on geometry proximity
 * - Per-pixel SH sampling via trilinear interpolation within bricks
 * - Virtual offsets to prevent light leaking near surfaces
 * - Camera-distance-based streaming of brick data
 * - L2 SH storage (9 coefficients x 3 channels = 27 floats per probe)
 *
 * @see DDGIProbeSystem.h, SHLighting.h, LightProbeSystem.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    static constexpr int APV_SH_COEFFICIENTS = 9;
    static constexpr int APV_PROBES_PER_BRICK_AXIS = 4;
    static constexpr int APV_PROBES_PER_BRICK =
        APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS;

    // =========================================================================
    // LOD Levels
    // =========================================================================

    enum class BrickLOD : uint8_t
    {
        Fine = 0,   ///< 1m spacing between probes
        Medium = 1, ///< 2m spacing
        Coarse = 2, ///< 4m spacing
        Count = 3
    };

    /// @brief Base spacing (meters) for each LOD level
    inline float BrickLODSpacing(BrickLOD lod)
    {
        static constexpr float spacings[] = {1.0f, 2.0f, 4.0f};
        return spacings[static_cast<int>(lod)];
    }

    // =========================================================================
    // Probe Data
    // =========================================================================

    /// @brief SH coefficients for a single probe (L2, 3 channels)
    struct APVProbeData
    {
        std::array<float, APV_SH_COEFFICIENTS> r{};
        std::array<float, APV_SH_COEFFICIENTS> g{};
        std::array<float, APV_SH_COEFFICIENTS> b{};

        /// @brief Virtual offset to push probe away from surfaces
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        bool valid = true; ///< False if probe is inside geometry
    };

    // =========================================================================
    // Brick
    // =========================================================================

    /// @brief Unique brick identifier based on grid position and LOD
    struct BrickID
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        BrickLOD lod = BrickLOD::Coarse;

        bool operator==(const BrickID& other) const
        {
            return x == other.x && y == other.y && z == other.z && lod == other.lod;
        }
    };

    struct BrickIDHash
    {
        size_t operator()(const BrickID& id) const
        {
            size_t h = std::hash<int32_t>()(id.x);
            h ^= std::hash<int32_t>()(id.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int32_t>()(id.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>()(static_cast<uint8_t>(id.lod)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    /// @brief A brick containing 4x4x4 = 64 probes
    struct APVBrick
    {
        BrickID id;
        std::array<APVProbeData, APV_PROBES_PER_BRICK> probes;
        float originX = 0.0f; ///< World origin of brick corner (0,0,0)
        float originY = 0.0f;
        float originZ = 0.0f;
        float spacing = 4.0f; ///< Probe spacing within this brick
        bool loaded = false;
        bool subdivided = false; ///< True if this brick has finer children
    };

    // =========================================================================
    // Irradiance Result
    // =========================================================================

    struct APVIrradiance
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    // =========================================================================
    // Adaptive Probe Volume Settings
    // =========================================================================

    struct APVSettings
    {
        float streamingDistance = 100.0f;          ///< Max distance from camera to load bricks
        float subdivisionGeometryThreshold = 2.0f; ///< Distance to geometry to trigger subdivision
        float virtualOffsetDistance = 0.25f;       ///< Push distance for virtual offsets
        float virtualOffsetSearchRadius = 1.0f;    ///< Search radius for offset raycast
        BrickLOD defaultLOD = BrickLOD::Coarse;
    };

    // =========================================================================
    // Adaptive Probe Volume System
    // =========================================================================

    /**
     * @brief Hierarchical adaptive probe volume with brick-based streaming
     *
     * CPU reference implementation. Manages a sparse set of bricks that
     * can be loaded/unloaded based on camera proximity and adaptively
     * subdivided near geometry.
     */
    class AdaptiveProbeVolumes
    {
      public:
        AdaptiveProbeVolumes() = default;
        ~AdaptiveProbeVolumes() = default;

        /// @brief Initialize the system
        bool Initialize(const APVSettings& settings)
        {
            m_settings = settings;
            m_bricks.clear();
            m_initialized = true;
            return true;
        }

        /// @brief Shutdown and release all data
        void Shutdown()
        {
            m_bricks.clear();
            m_initialized = false;
        }

        /**
         * @brief Create a brick at the given grid position and LOD
         *
         * Initializes 64 probes at the appropriate spacing.
         *
         * @param gridX, gridY, gridZ  Brick grid coordinates
         * @param lod  LOD level determining probe spacing
         * @return True if brick was created successfully
         */
        bool CreateBrick(int32_t gridX, int32_t gridY, int32_t gridZ, BrickLOD lod)
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
            return true;
        }

        /**
         * @brief Subdivide a brick into finer LOD children
         *
         * Replaces a coarse brick with 8 finer bricks. The parent
         * is marked as subdivided and its probe data is used to
         * initialize children via interpolation.
         *
         * @param gridX, gridY, gridZ  Parent brick coordinates
         * @param parentLOD  LOD of the parent brick
         * @return True if subdivision succeeded
         */
        bool SubdivideBrick(int32_t gridX, int32_t gridY, int32_t gridZ, BrickLOD parentLOD)
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

        /**
         * @brief Apply virtual offsets to probes in a brick
         *
         * For each probe, checks if geometry is within the search radius.
         * If so, pushes the probe away from the surface to prevent
         * light leaking.
         *
         * @param id          Brick identifier
         * @param geometryDistances  Array of 64 distances to nearest geometry
         *                           (negative if inside geometry)
         * @param geometryNormals    Array of 64 surface normals (nx,ny,nz triples)
         */
        void ApplyVirtualOffsets(const BrickID& id, const float* geometryDistances, const float* geometryNormals)
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

        /**
         * @brief Update streaming: load/unload bricks based on camera distance
         *
         * @param cameraPosX/Y/Z  Camera world position
         */
        void UpdateStreaming(float cameraPosX, float cameraPosY, float cameraPosZ)
        {
            if (!m_initialized)
                return;

            float maxDist = m_settings.streamingDistance;
            float maxDistSq = maxDist * maxDist;

            // Unload bricks that are too far from camera
            std::vector<BrickID> toUnload;
            for (auto& [id, brick] : m_bricks)
            {
                float cx = brick.originX + brick.spacing * (APV_PROBES_PER_BRICK_AXIS - 1) * 0.5f;
                float cy = brick.originY + brick.spacing * (APV_PROBES_PER_BRICK_AXIS - 1) * 0.5f;
                float cz = brick.originZ + brick.spacing * (APV_PROBES_PER_BRICK_AXIS - 1) * 0.5f;

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
        }

        /**
         * @brief Store baked SH data into a specific probe within a brick
         *
         * @param id     Brick identifier
         * @param probeX, probeY, probeZ  Local probe indices [0, 3]
         * @param shR, shG, shB  Arrays of 9 SH coefficients per channel
         */
        void SetProbeData(const BrickID& id, int probeX, int probeY, int probeZ, const float shR[APV_SH_COEFFICIENTS],
                          const float shG[APV_SH_COEFFICIENTS], const float shB[APV_SH_COEFFICIENTS])
        {
            auto it = m_bricks.find(id);
            if (it == m_bricks.end())
                return;

            int localIdx = probeZ * APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS +
                           probeY * APV_PROBES_PER_BRICK_AXIS + probeX;

            APVProbeData& probe = it->second.probes[localIdx];
            for (int i = 0; i < APV_SH_COEFFICIENTS; ++i)
            {
                probe.r[i] = shR[i];
                probe.g[i] = shG[i];
                probe.b[i] = shB[i];
            }
        }

        /**
         * @brief Sample irradiance at a world position and surface normal
         *
         * Finds the finest loaded brick containing the position and
         * performs trilinear interpolation of SH data within the brick.
         *
         * @param worldX/Y/Z    Query position
         * @param normalX/Y/Z   Surface normal for SH evaluation
         * @return Interpolated irradiance
         */
        APVIrradiance SampleIrradiance(float worldX, float worldY, float worldZ, float normalX, float normalY,
                                       float normalZ) const
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
                    (1 - fx) * (1 - fy) * (1 - fz),
                    fx * (1 - fy) * (1 - fz),
                    (1 - fx) * fy * (1 - fz),
                    fx * fy * (1 - fz),
                    (1 - fx) * (1 - fy) * fz,
                    fx * (1 - fy) * fz,
                    (1 - fx) * fy * fz,
                    fx * fy * fz,
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

        /// @brief Get total number of loaded bricks
        uint32_t GetLoadedBrickCount() const { return static_cast<uint32_t>(m_bricks.size()); }

        /// @brief Get total number of probes across all loaded bricks
        uint32_t GetTotalProbeCount() const { return static_cast<uint32_t>(m_bricks.size()) * APV_PROBES_PER_BRICK; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Access settings
        const APVSettings& GetSettings() const { return m_settings; }

      private:
        /// @brief Compute local probe index within a brick
        static int LocalProbeIndex(int px, int py, int pz)
        {
            return pz * APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS + py * APV_PROBES_PER_BRICK_AXIS + px;
        }

        /// @brief Evaluate L2 SH basis functions
        static void EvaluateSHBasis(float nx, float ny, float nz, float outBasis[APV_SH_COEFFICIENTS])
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

        /// @brief Initialize child brick probes from parent via interpolation
        void InterpolateBrickFromParent(APVBrick& child, const APVBrick& parent) const
        {
            float parentBrickSize = parent.spacing * (APV_PROBES_PER_BRICK_AXIS - 1);

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

        APVSettings m_settings;
        std::unordered_map<BrickID, APVBrick, BrickIDHash> m_bricks;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
