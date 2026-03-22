/**
 * @file DDGIProbeSystem.h
 * @brief Dynamic Diffuse Global Illumination using irradiance probes
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a probe-based DDGI system storing L2 spherical harmonics
 * coefficients on a regular 3D grid. Each probe accumulates irradiance
 * from ray-cast samples and blends with hysteresis for temporal stability.
 *
 * Features:
 * - Regular 3D grid placement with configurable spacing
 * - Per-probe ray dispatch with cosine-weighted hemisphere sampling
 * - Hysteresis blending for smooth temporal updates
 * - Border texel copy for seamless trilinear interpolation
 * - Probe relocation away from geometry to prevent light leaking
 * - Trilinear interpolation of 8 nearest probes at any world position
 *
 * @see SHLighting.h, LightProbeSystem.h, VoxelConeTracing.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    /// @brief Number of L2 SH coefficients per color channel
    static constexpr int DDGI_SH_COEFFICIENTS = 9;

    /// @brief Total floats per probe (9 coefficients x 3 channels)
    static constexpr int DDGI_FLOATS_PER_PROBE = DDGI_SH_COEFFICIENTS * 3;

    // =========================================================================
    // DDGI Configuration
    // =========================================================================

    struct DDGISettings
    {
        float spacingX = 2.0f;            ///< Grid spacing along X axis (meters)
        float spacingY = 2.0f;            ///< Grid spacing along Y axis (meters)
        float spacingZ = 2.0f;            ///< Grid spacing along Z axis (meters)
        uint32_t countX = 8;              ///< Number of probes along X
        uint32_t countY = 4;              ///< Number of probes along Y
        uint32_t countZ = 8;              ///< Number of probes along Z
        float originX = 0.0f;             ///< Grid origin X
        float originY = 0.0f;             ///< Grid origin Y
        float originZ = 0.0f;             ///< Grid origin Z
        int raysPerProbe = 128;           ///< Rays cast per probe per update
        float hysteresis = 0.97f;         ///< Blend factor for temporal stability [0..1]
        float maxRayDistance = 50.0f;     ///< Maximum ray trace distance
        float normalBias = 0.25f;         ///< Bias along surface normal to prevent self-shadowing
        float relocationThreshold = 0.5f; ///< Min distance from geometry before relocation
        float relocationMaxOffset = 1.0f; ///< Maximum relocation offset
    };

    // =========================================================================
    // DDGI Probe Data
    // =========================================================================

    /// @brief Per-probe SH irradiance stored as 27 floats (9 R + 9 G + 9 B)
    struct DDGIProbe
    {
        std::array<float, DDGI_SH_COEFFICIENTS> r{};
        std::array<float, DDGI_SH_COEFFICIENTS> g{};
        std::array<float, DDGI_SH_COEFFICIENTS> b{};
        float offsetX = 0.0f; ///< Relocation offset from grid position
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        bool relocated = false;
    };

    /// @brief Result of a single ray cast for probe irradiance gathering
    struct DDGIRayResult
    {
        float hitDistance = -1.0f; ///< Distance to hit, negative if miss
        float radianceR = 0.0f;    ///< Incoming radiance R at hit point
        float radianceG = 0.0f;    ///< Incoming radiance G at hit point
        float radianceB = 0.0f;    ///< Incoming radiance B at hit point
        float hitNormalX = 0.0f;   ///< Surface normal at hit point
        float hitNormalY = 1.0f;
        float hitNormalZ = 0.0f;
    };

    /// @brief Interpolated irradiance result at a world position
    struct DDGIIrradiance
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    // =========================================================================
    // DDGI Probe System
    // =========================================================================

    /**
     * @brief Dynamic Diffuse Global Illumination probe system
     *
     * CPU reference implementation. Manages a 3D grid of irradiance probes
     * with SH encoding, temporal hysteresis, probe relocation, and
     * trilinear interpolation for querying irradiance at any world position.
     */
    class DDGIProbeSystem
    {
      public:
        DDGIProbeSystem() = default;
        ~DDGIProbeSystem() = default;

        /// @brief Initialize the probe grid with given settings
        bool Initialize(const DDGISettings& settings)
        {
            m_settings = settings;
            uint32_t totalProbes = settings.countX * settings.countY * settings.countZ;
            m_probes.resize(totalProbes);
            m_initialized = true;
            return true;
        }

        /// @brief Shutdown and release probe data
        void Shutdown()
        {
            m_probes.clear();
            m_initialized = false;
        }

        /// @brief Get the world position of a probe at grid index (ix, iy, iz)
        void GetProbePosition(uint32_t ix, uint32_t iy, uint32_t iz, float& outX, float& outY, float& outZ) const
        {
            const DDGIProbe& probe = m_probes[ProbeIndex(ix, iy, iz)];
            outX = m_settings.originX + ix * m_settings.spacingX + probe.offsetX;
            outY = m_settings.originY + iy * m_settings.spacingY + probe.offsetY;
            outZ = m_settings.originZ + iz * m_settings.spacingZ + probe.offsetZ;
        }

        /**
         * @brief Update a single probe with new ray results
         *
         * Projects ray radiance into SH basis and blends with existing
         * coefficients using hysteresis for temporal stability.
         *
         * @param ix, iy, iz  Grid indices of the probe
         * @param rays        Array of ray results from tracing
         * @param rayCount    Number of rays in the array
         */
        void UpdateProbe(uint32_t ix, uint32_t iy, uint32_t iz, const DDGIRayResult* rays, int rayCount)
        {
            if (!m_initialized || rayCount <= 0)
                return;

            uint32_t idx = ProbeIndex(ix, iy, iz);
            DDGIProbe& probe = m_probes[idx];

            // Accumulate new SH from ray results
            std::array<float, DDGI_SH_COEFFICIENTS> newR{};
            std::array<float, DDGI_SH_COEFFICIENTS> newG{};
            std::array<float, DDGI_SH_COEFFICIENTS> newB{};

            float validRayWeight = 0.0f;

            for (int r = 0; r < rayCount; ++r)
            {
                const DDGIRayResult& ray = rays[r];
                if (ray.hitDistance < 0.0f)
                    continue; // Miss: contributes sky radiance (assumed zero here)

                // Compute ray direction from spherical Fibonacci distribution
                float dirX, dirY, dirZ;
                SphericalFibonacci(r, rayCount, dirX, dirY, dirZ);

                // Evaluate SH basis for this direction
                float basis[DDGI_SH_COEFFICIENTS];
                EvaluateSHBasis(dirX, dirY, dirZ, basis);

                // Cosine-weight: radiance * max(dot(normal, direction), 0)
                float cosWeight = std::max(0.0f, ray.hitNormalX * dirX + ray.hitNormalY * dirY + ray.hitNormalZ * dirZ);

                float weight = cosWeight;
                validRayWeight += weight;

                for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
                {
                    float bw = basis[c] * weight;
                    newR[c] += ray.radianceR * bw;
                    newG[c] += ray.radianceG * bw;
                    newB[c] += ray.radianceB * bw;
                }
            }

            // Normalize by total weight
            if (validRayWeight > 0.0f)
            {
                float invWeight = (4.0f * 3.14159265f) / validRayWeight;
                for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
                {
                    newR[c] *= invWeight;
                    newG[c] *= invWeight;
                    newB[c] *= invWeight;
                }
            }

            // Hysteresis blend: lerp(new, old, hysteresis)
            float h = m_settings.hysteresis;
            float oneMinusH = 1.0f - h;
            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                probe.r[c] = probe.r[c] * h + newR[c] * oneMinusH;
                probe.g[c] = probe.g[c] * h + newG[c] * oneMinusH;
                probe.b[c] = probe.b[c] * h + newB[c] * oneMinusH;
            }
        }

        /**
         * @brief Copy border texels for seamless interpolation
         *
         * In the GPU texture atlas, each probe's SH data occupies a small tile.
         * Border texels must be copied from the opposite edge to enable
         * hardware bilinear filtering across probe boundaries.
         * On CPU, this ensures the trilinear interpolation at grid edges
         * wraps correctly.
         */
        void UpdateProbeBorders()
        {
            if (!m_initialized)
                return;

            // For edge probes, clamp to nearest interior probe's SH data.
            // This prevents interpolation artifacts at grid boundaries.
            uint32_t cx = m_settings.countX;
            uint32_t cy = m_settings.countY;
            uint32_t cz = m_settings.countZ;

            for (uint32_t iy = 0; iy < cy; ++iy)
            {
                for (uint32_t iz = 0; iz < cz; ++iz)
                {
                    // Clamp X borders
                    CopyProbeData(ProbeIndex(0, iy, iz), ProbeIndex(std::min(1u, cx - 1), iy, iz));
                    if (cx > 1)
                    {
                        CopyProbeData(ProbeIndex(cx - 1, iy, iz), ProbeIndex(cx - 2, iy, iz));
                    }
                }
            }

            for (uint32_t ix = 0; ix < cx; ++ix)
            {
                for (uint32_t iz = 0; iz < cz; ++iz)
                {
                    CopyProbeData(ProbeIndex(ix, 0, iz), ProbeIndex(ix, std::min(1u, cy - 1), iz));
                    if (cy > 1)
                    {
                        CopyProbeData(ProbeIndex(ix, cy - 1, iz), ProbeIndex(ix, cy - 2, iz));
                    }
                }
            }

            for (uint32_t ix = 0; ix < cx; ++ix)
            {
                for (uint32_t iy = 0; iy < cy; ++iy)
                {
                    CopyProbeData(ProbeIndex(ix, iy, 0), ProbeIndex(ix, iy, std::min(1u, cz - 1)));
                    if (cz > 1)
                    {
                        CopyProbeData(ProbeIndex(ix, iy, cz - 1), ProbeIndex(ix, iy, cz - 2));
                    }
                }
            }
        }

        /**
         * @brief Relocate a probe away from nearby geometry
         *
         * If a probe is inside or very close to geometry, its irradiance
         * samples will be biased. This nudges the probe along the surface
         * normal of the closest hit to prevent light leaking.
         *
         * @param ix, iy, iz      Grid indices of the probe
         * @param closestHitDist  Distance to closest geometry from probe center
         * @param hitNormalX/Y/Z  Surface normal at the closest hit
         */
        void RelocateProbe(uint32_t ix, uint32_t iy, uint32_t iz, float closestHitDist, float hitNormalX,
                           float hitNormalY, float hitNormalZ)
        {
            if (!m_initialized)
                return;

            uint32_t idx = ProbeIndex(ix, iy, iz);
            DDGIProbe& probe = m_probes[idx];

            if (closestHitDist < m_settings.relocationThreshold)
            {
                // Push probe along the hit normal, clamped to max offset
                float pushDist =
                    std::min(m_settings.relocationThreshold - closestHitDist + 0.1f, m_settings.relocationMaxOffset);

                probe.offsetX += hitNormalX * pushDist;
                probe.offsetY += hitNormalY * pushDist;
                probe.offsetZ += hitNormalZ * pushDist;

                // Clamp total offset magnitude
                float offsetLen = std::sqrt(probe.offsetX * probe.offsetX + probe.offsetY * probe.offsetY +
                                            probe.offsetZ * probe.offsetZ);

                if (offsetLen > m_settings.relocationMaxOffset)
                {
                    float scale = m_settings.relocationMaxOffset / offsetLen;
                    probe.offsetX *= scale;
                    probe.offsetY *= scale;
                    probe.offsetZ *= scale;
                }

                probe.relocated = true;
            }
        }

        /**
         * @brief Query irradiance at an arbitrary world position
         *
         * Performs trilinear interpolation of the 8 nearest probes.
         * Each probe's SH is evaluated at the given surface normal
         * before interpolation.
         *
         * @param worldX, worldY, worldZ  Query position
         * @param normalX, normalY, normalZ  Surface normal for SH evaluation
         * @return Interpolated irradiance (RGB)
         */
        DDGIIrradiance QueryIrradiance(float worldX, float worldY, float worldZ, float normalX, float normalY,
                                       float normalZ) const
        {
            DDGIIrradiance result{};
            if (!m_initialized)
                return result;

            // Convert world position to grid-local continuous coordinates
            float gx = (worldX - m_settings.originX) / m_settings.spacingX;
            float gy = (worldY - m_settings.originY) / m_settings.spacingY;
            float gz = (worldZ - m_settings.originZ) / m_settings.spacingZ;

            // Clamp to valid grid range
            float maxX = static_cast<float>(m_settings.countX - 1);
            float maxY = static_cast<float>(m_settings.countY - 1);
            float maxZ = static_cast<float>(m_settings.countZ - 1);
            gx = std::clamp(gx, 0.0f, maxX);
            gy = std::clamp(gy, 0.0f, maxY);
            gz = std::clamp(gz, 0.0f, maxZ);

            // Integer base indices
            uint32_t ix0 = static_cast<uint32_t>(gx);
            uint32_t iy0 = static_cast<uint32_t>(gy);
            uint32_t iz0 = static_cast<uint32_t>(gz);
            uint32_t ix1 = std::min(ix0 + 1, m_settings.countX - 1);
            uint32_t iy1 = std::min(iy0 + 1, m_settings.countY - 1);
            uint32_t iz1 = std::min(iz0 + 1, m_settings.countZ - 1);

            // Fractional parts for trilinear interpolation
            float fx = gx - ix0;
            float fy = gy - iy0;
            float fz = gz - iz0;

            // Evaluate SH irradiance at all 8 corners and trilinearly interpolate
            float weights[8] = {
                (1 - fx) * (1 - fy) * (1 - fz), fx * (1 - fy) * (1 - fz), (1 - fx) * fy * (1 - fz), fx * fy * (1 - fz),
                (1 - fx) * (1 - fy) * fz,       fx * (1 - fy) * fz,       (1 - fx) * fy * fz,       fx * fy * fz,
            };

            uint32_t indices[8] = {
                ProbeIndex(ix0, iy0, iz0), ProbeIndex(ix1, iy0, iz0), ProbeIndex(ix0, iy1, iz0),
                ProbeIndex(ix1, iy1, iz0), ProbeIndex(ix0, iy0, iz1), ProbeIndex(ix1, iy0, iz1),
                ProbeIndex(ix0, iy1, iz1), ProbeIndex(ix1, iy1, iz1),
            };

            float basis[DDGI_SH_COEFFICIENTS];
            EvaluateSHBasis(normalX, normalY, normalZ, basis);

            // Irradiance convolution weights (Ramamoorthi & Hanrahan 2001)
            static constexpr float A0 = 3.14159265f;
            static constexpr float A1 = 2.09439510f;
            static constexpr float A2 = 0.78539816f;
            static constexpr float convWeights[DDGI_SH_COEFFICIENTS] = {A0, A1, A1, A1, A2, A2, A2, A2, A2};

            for (int corner = 0; corner < 8; ++corner)
            {
                const DDGIProbe& probe = m_probes[indices[corner]];
                float w = weights[corner];

                float ir = 0.0f, ig = 0.0f, ib = 0.0f;
                for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
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

            return result;
        }

        /// @brief Get total number of probes
        uint32_t GetProbeCount() const { return m_settings.countX * m_settings.countY * m_settings.countZ; }

        /// @brief Access the settings
        const DDGISettings& GetSettings() const { return m_settings; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

      private:
        /// @brief Compute flat index from 3D grid coordinates
        uint32_t ProbeIndex(uint32_t ix, uint32_t iy, uint32_t iz) const
        {
            return iz * m_settings.countX * m_settings.countY + iy * m_settings.countX + ix;
        }

        /// @brief Copy SH data from source probe to destination probe (border clamping)
        void CopyProbeData(uint32_t dstIdx, uint32_t srcIdx)
        {
            m_probes[dstIdx].r = m_probes[srcIdx].r;
            m_probes[dstIdx].g = m_probes[srcIdx].g;
            m_probes[dstIdx].b = m_probes[srcIdx].b;
        }

        /**
         * @brief Evaluate the 9 real SH basis functions for a direction
         * @param nx, ny, nz  Normalized direction
         * @param outBasis    Output array of 9 basis values
         */
        static void EvaluateSHBasis(float nx, float ny, float nz, float outBasis[DDGI_SH_COEFFICIENTS])
        {
            outBasis[0] = 0.282095f;                        // Y_0^0
            outBasis[1] = 0.488603f * ny;                   // Y_1^-1
            outBasis[2] = 0.488603f * nz;                   // Y_1^0
            outBasis[3] = 0.488603f * nx;                   // Y_1^1
            outBasis[4] = 1.092548f * nx * ny;              // Y_2^-2
            outBasis[5] = 1.092548f * ny * nz;              // Y_2^-1
            outBasis[6] = 0.315392f * (3.0f * nz * nz - 1); // Y_2^0
            outBasis[7] = 1.092548f * nx * nz;              // Y_2^1
            outBasis[8] = 0.546274f * (nx * nx - ny * ny);  // Y_2^2
        }

        /**
         * @brief Generate a quasi-uniform direction using spherical Fibonacci
         *
         * Produces a near-uniform distribution of points on the unit sphere,
         * used for probe ray directions.
         *
         * @param index     Ray index
         * @param count     Total number of rays
         * @param outX/Y/Z  Output direction (normalized)
         */
        static void SphericalFibonacci(int index, int count, float& outX, float& outY, float& outZ)
        {
            static constexpr float GOLDEN_RATIO = 1.6180339887498948f;
            float theta = 2.0f * 3.14159265f * index / GOLDEN_RATIO;
            float phi = std::acos(1.0f - 2.0f * (index + 0.5f) / count);
            float sinPhi = std::sin(phi);
            outX = std::cos(theta) * sinPhi;
            outY = std::cos(phi);
            outZ = std::sin(theta) * sinPhi;
        }

        DDGISettings m_settings;
        std::vector<DDGIProbe> m_probes;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
