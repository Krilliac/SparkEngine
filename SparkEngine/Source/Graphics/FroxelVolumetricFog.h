/**
 * @file FroxelVolumetricFog.h
 * @brief Froxel-based volumetric fog using a frustum-aligned 3D grid
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements volumetric fog via a froxel (frustum-voxel) grid. The camera
 * frustum is subdivided into a 3D grid with logarithmic depth distribution
 * for higher near-camera resolution.
 *
 * Three-pass pipeline:
 * 1. Media injection: compute inscattering + extinction per froxel
 * 2. Light scattering: evaluate lights with Henyey-Greenstein phase function
 * 3. Temporal filtering: blend with previous frame using motion vectors
 *
 * Final fog factor is integrated via Beer-Lambert transmittance.
 *
 * @see FogSystem.h, LightingSystem.h, ClusteredLightCulling.h
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

    static constexpr float FROXEL_PI = 3.14159265358979323846f;

    // =========================================================================
    // Froxel Grid Configuration
    // =========================================================================

    struct FroxelFogSettings
    {
        uint32_t gridWidth = 160;     ///< Froxels along X (screen width / 12 at 1080p)
        uint32_t gridHeight = 90;     ///< Froxels along Y (screen height / 12 at 1080p)
        uint32_t gridDepth = 64;      ///< Froxels along Z (depth slices)
        float nearPlane = 0.1f;       ///< Camera near plane
        float farPlane = 200.0f;      ///< Maximum fog distance
        float logDistribution = 0.5f; ///< Blend between linear (0) and logarithmic (1) depth

        float globalDensity = 0.02f;     ///< Base fog density (extinction coefficient)
        float globalScattering = 0.015f; ///< Base scattering coefficient
        float globalAbsorption = 0.005f; ///< Base absorption coefficient
        float phaseG = 0.3f;             ///< Henyey-Greenstein asymmetry parameter [-1, 1]

        float ambientR = 0.05f; ///< Ambient inscattering R
        float ambientG = 0.06f; ///< Ambient inscattering G
        float ambientB = 0.08f; ///< Ambient inscattering B

        float temporalBlend = 0.9f;              ///< History blend factor [0..1]
        float temporalRejectionThreshold = 0.1f; ///< Motion rejection threshold
    };

    // =========================================================================
    // Point Light for Fog Scattering
    // =========================================================================

    struct FroxelLight
    {
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f;
        float intensity = 1.0f;
        float radius = 10.0f; ///< Attenuation radius
    };

    // =========================================================================
    // Local Fog Volume
    // =========================================================================

    struct FogVolume
    {
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float extentX = 5.0f, extentY = 5.0f, extentZ = 5.0f;
        float density = 0.05f;
        float scattering = 0.04f;
        float absorption = 0.01f;
    };

    // =========================================================================
    // Per-Froxel Data
    // =========================================================================

    struct FroxelData
    {
        float inscatterR = 0.0f; ///< Accumulated inscattered light R
        float inscatterG = 0.0f; ///< Accumulated inscattered light G
        float inscatterB = 0.0f; ///< Accumulated inscattered light B
        float extinction = 0.0f; ///< Total extinction coefficient
    };

    // =========================================================================
    // Fog Query Result
    // =========================================================================

    struct FroxelFogResult
    {
        float fogR = 0.0f; ///< Inscattered light to blend in
        float fogG = 0.0f;
        float fogB = 0.0f;
        float transmittance = 1.0f; ///< Scene color multiplier (Beer-Lambert)
    };

    // =========================================================================
    // Froxel Volumetric Fog System
    // =========================================================================

    /**
     * @brief CPU reference implementation of froxel-based volumetric fog
     *
     * GPU implementation maps each pass to a compute shader dispatch.
     * The froxel grid is stored as a flat 3D array indexed by
     * (sliceZ * gridWidth * gridHeight + y * gridWidth + x).
     */
    class FroxelVolumetricFog
    {
      public:
        FroxelVolumetricFog() = default;
        ~FroxelVolumetricFog() = default;

        /// @brief Initialize the froxel grid
        bool Initialize(const FroxelFogSettings& settings)
        {
            m_settings = settings;
            uint32_t total = settings.gridWidth * settings.gridHeight * settings.gridDepth;
            m_currentGrid.resize(total);
            m_historyGrid.resize(total);
            m_integratedGrid.resize(total);
            m_frameCount = 0;
            m_initialized = true;
            return true;
        }

        /// @brief Shutdown and release buffers
        void Shutdown()
        {
            m_currentGrid.clear();
            m_historyGrid.clear();
            m_integratedGrid.clear();
            m_initialized = false;
        }

        /**
         * @brief Convert a depth slice index to a linear depth value
         *
         * Uses a blend between linear and logarithmic distribution.
         * Logarithmic gives more resolution near the camera.
         *
         * @param slice  Depth slice index [0, gridDepth)
         * @return Linear depth in world units
         */
        float SliceToDepth(uint32_t slice) const
        {
            float t = static_cast<float>(slice + 0.5f) / static_cast<float>(m_settings.gridDepth);
            float nearZ = m_settings.nearPlane;
            float farZ = m_settings.farPlane;
            float blend = m_settings.logDistribution;

            float linearDepth = nearZ + t * (farZ - nearZ);
            float logDepth = nearZ * std::pow(farZ / nearZ, t);

            return std::lerp(linearDepth, logDepth, blend);
        }

        /**
         * @brief Convert a linear depth to a depth slice index
         * @param depth  Linear depth in world units
         * @return Fractional slice index
         */
        float DepthToSlice(float depth) const
        {
            float nearZ = m_settings.nearPlane;
            float farZ = m_settings.farPlane;
            float blend = m_settings.logDistribution;

            float linearT = (depth - nearZ) / (farZ - nearZ);
            float logT = std::log(depth / nearZ) / std::log(farZ / nearZ);

            float t = std::lerp(linearT, logT, blend);
            return t * static_cast<float>(m_settings.gridDepth);
        }

        /**
         * @brief Pass 1: Inject media properties into the froxel grid
         *
         * Sets base extinction and inscattering from global fog parameters
         * and overlapping local fog volumes.
         *
         * @param volumes     Array of local fog volumes
         * @param volumeCount Number of volumes
         */
        void InjectMedia(const FogVolume* volumes, int volumeCount)
        {
            if (!m_initialized)
                return;

            uint32_t w = m_settings.gridWidth;
            uint32_t h = m_settings.gridHeight;
            uint32_t d = m_settings.gridDepth;

            for (uint32_t z = 0; z < d; ++z)
            {
                float depth = SliceToDepth(z);

                for (uint32_t y = 0; y < h; ++y)
                {
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        uint32_t idx = FroxelIndex(x, y, z);
                        FroxelData& froxel = m_currentGrid[idx];

                        // Base media from global settings
                        froxel.extinction = m_settings.globalDensity;
                        froxel.inscatterR = m_settings.ambientR * m_settings.globalScattering;
                        froxel.inscatterG = m_settings.ambientG * m_settings.globalScattering;
                        froxel.inscatterB = m_settings.ambientB * m_settings.globalScattering;

                        // Approximate world position of froxel center
                        // Normalized screen coordinates
                        float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                        float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);

                        // Simple view-space to world approximation (assumes identity view)
                        float worldX = (u * 2.0f - 1.0f) * depth;
                        float worldY = (v * 2.0f - 1.0f) * depth;
                        float worldZ = depth;

                        // Accumulate local fog volumes
                        for (int vi = 0; vi < volumeCount; ++vi)
                        {
                            const FogVolume& vol = volumes[vi];

                            // Check if froxel center is inside volume AABB
                            float dx = std::abs(worldX - vol.posX);
                            float dy = std::abs(worldY - vol.posY);
                            float dz = std::abs(worldZ - vol.posZ);

                            if (dx <= vol.extentX && dy <= vol.extentY && dz <= vol.extentZ)
                            {
                                // Smooth falloff toward volume edges
                                float fx = 1.0f - (dx / vol.extentX);
                                float fy = 1.0f - (dy / vol.extentY);
                                float fz = 1.0f - (dz / vol.extentZ);
                                float falloff = fx * fy * fz;

                                froxel.extinction += vol.density * falloff;
                                float scatter = vol.scattering * falloff;
                                froxel.inscatterR += scatter;
                                froxel.inscatterG += scatter;
                                froxel.inscatterB += scatter;
                            }
                        }
                    }
                }
            }
        }

        /**
         * @brief Pass 2: Evaluate light scattering per froxel
         *
         * For each froxel, evaluates all lights using the Henyey-Greenstein
         * phase function and accumulates inscattered light.
         *
         * @param lights       Array of point lights
         * @param lightCount   Number of lights
         * @param cameraPosX/Y/Z  Camera world position for phase function
         */
        void ScatterLight(const FroxelLight* lights, int lightCount, float cameraPosX, float cameraPosY,
                          float cameraPosZ)
        {
            if (!m_initialized)
                return;

            uint32_t w = m_settings.gridWidth;
            uint32_t h = m_settings.gridHeight;
            uint32_t d = m_settings.gridDepth;

            for (uint32_t z = 0; z < d; ++z)
            {
                float depth = SliceToDepth(z);

                for (uint32_t y = 0; y < h; ++y)
                {
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        uint32_t idx = FroxelIndex(x, y, z);
                        FroxelData& froxel = m_currentGrid[idx];

                        float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                        float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);

                        float worldX = (u * 2.0f - 1.0f) * depth;
                        float worldY = (v * 2.0f - 1.0f) * depth;
                        float worldZ = depth;

                        // View direction from camera to froxel
                        float viewDirX = worldX - cameraPosX;
                        float viewDirY = worldY - cameraPosY;
                        float viewDirZ = worldZ - cameraPosZ;
                        float viewLen = std::sqrt(viewDirX * viewDirX + viewDirY * viewDirY + viewDirZ * viewDirZ);
                        if (viewLen > 0.0001f)
                        {
                            viewDirX /= viewLen;
                            viewDirY /= viewLen;
                            viewDirZ /= viewLen;
                        }

                        // Evaluate each light
                        for (int li = 0; li < lightCount; ++li)
                        {
                            const FroxelLight& light = lights[li];

                            float toX = light.posX - worldX;
                            float toY = light.posY - worldY;
                            float toZ = light.posZ - worldZ;
                            float dist = std::sqrt(toX * toX + toY * toY + toZ * toZ);

                            if (dist > light.radius || dist < 0.001f)
                                continue;

                            // Normalize light direction
                            float lx = toX / dist;
                            float ly = toY / dist;
                            float lz = toZ / dist;

                            // Henyey-Greenstein phase function
                            float cosTheta = viewDirX * lx + viewDirY * ly + viewDirZ * lz;
                            float phase = HenyeyGreenstein(cosTheta, m_settings.phaseG);

                            // Distance attenuation (inverse square with radius falloff)
                            float normDist = dist / light.radius;
                            float attenuation = std::max(0.0f, 1.0f - normDist * normDist);
                            attenuation *= attenuation; // Smooth falloff
                            attenuation *= light.intensity / (dist * dist + 1.0f);

                            float scatter = froxel.extinction * phase * attenuation;
                            froxel.inscatterR += light.colorR * scatter;
                            froxel.inscatterG += light.colorG * scatter;
                            froxel.inscatterB += light.colorB * scatter;
                        }
                    }
                }
            }
        }

        /**
         * @brief Pass 3: Temporal reprojection and filtering
         *
         * Blends current frame's froxel data with the previous frame's history
         * to reduce temporal noise. Uses a simple blend factor with optional
         * motion-based rejection.
         *
         * @param motionScale  Global motion magnitude for rejection (0 = static)
         */
        void TemporalFilter(float motionScale = 0.0f)
        {
            if (!m_initialized)
                return;

            m_frameCount++;
            uint32_t total = m_settings.gridWidth * m_settings.gridHeight * m_settings.gridDepth;

            float blend = m_settings.temporalBlend;

            // Reduce blend when camera is moving fast
            if (motionScale > m_settings.temporalRejectionThreshold)
            {
                float rejection = std::exp(-motionScale * 10.0f);
                blend *= rejection;
            }

            for (uint32_t i = 0; i < total; ++i)
            {
                if (m_frameCount <= 1)
                {
                    m_historyGrid[i] = m_currentGrid[i];
                }
                else
                {
                    FroxelData& curr = m_currentGrid[i];
                    FroxelData& hist = m_historyGrid[i];

                    hist.inscatterR = std::lerp(curr.inscatterR, hist.inscatterR, blend);
                    hist.inscatterG = std::lerp(curr.inscatterG, hist.inscatterG, blend);
                    hist.inscatterB = std::lerp(curr.inscatterB, hist.inscatterB, blend);
                    hist.extinction = std::lerp(curr.extinction, hist.extinction, blend);

                    curr = hist;
                }
            }
        }

        /**
         * @brief Integrate fog along view rays using Beer-Lambert
         *
         * Front-to-back integration: accumulates inscattered light and
         * transmittance along each screen-space column of froxels.
         * Must be called after InjectMedia + ScatterLight + TemporalFilter.
         */
        void Integrate()
        {
            if (!m_initialized)
                return;

            uint32_t w = m_settings.gridWidth;
            uint32_t h = m_settings.gridHeight;
            uint32_t d = m_settings.gridDepth;

            for (uint32_t y = 0; y < h; ++y)
            {
                for (uint32_t x = 0; x < w; ++x)
                {
                    float accumR = 0.0f;
                    float accumG = 0.0f;
                    float accumB = 0.0f;
                    float transmittance = 1.0f;

                    float prevDepth = m_settings.nearPlane;

                    for (uint32_t z = 0; z < d; ++z)
                    {
                        uint32_t idx = FroxelIndex(x, y, z);
                        const FroxelData& froxel = m_currentGrid[idx];

                        float currDepth = SliceToDepth(z);
                        float sliceThickness = currDepth - prevDepth;
                        prevDepth = currDepth;

                        // Beer-Lambert transmittance for this slice
                        float opticalDepth = froxel.extinction * sliceThickness;
                        float sliceTransmittance = std::exp(-opticalDepth);

                        // Integrate inscattered light (analytical integration)
                        // integral of S * exp(-sigma * t) dt from 0 to thickness
                        float integrationFactor = (froxel.extinction > 0.0001f)
                                                      ? (1.0f - sliceTransmittance) / froxel.extinction
                                                      : sliceThickness;

                        accumR += transmittance * froxel.inscatterR * integrationFactor;
                        accumG += transmittance * froxel.inscatterG * integrationFactor;
                        accumB += transmittance * froxel.inscatterB * integrationFactor;

                        transmittance *= sliceTransmittance;

                        // Store integrated result at this depth
                        m_integratedGrid[idx].inscatterR = accumR;
                        m_integratedGrid[idx].inscatterG = accumG;
                        m_integratedGrid[idx].inscatterB = accumB;
                        m_integratedGrid[idx].extinction = transmittance;
                    }
                }
            }
        }

        /**
         * @brief Query fog at a given screen UV and depth
         *
         * @param u, v    Screen UV coordinates [0, 1]
         * @param depth   Linear depth in world units
         * @return Fog inscattering (RGB) and transmittance
         */
        FroxelFogResult QueryFog(float u, float v, float depth) const
        {
            FroxelFogResult result{};
            if (!m_initialized)
                return result;

            float sliceF = DepthToSlice(depth);
            float fx = u * static_cast<float>(m_settings.gridWidth) - 0.5f;
            float fy = v * static_cast<float>(m_settings.gridHeight) - 0.5f;

            // Trilinear interpolation
            int x0 = std::clamp(static_cast<int>(fx), 0, static_cast<int>(m_settings.gridWidth - 1));
            int y0 = std::clamp(static_cast<int>(fy), 0, static_cast<int>(m_settings.gridHeight - 1));
            int z0 = std::clamp(static_cast<int>(sliceF), 0, static_cast<int>(m_settings.gridDepth - 1));
            int x1 = std::min(x0 + 1, static_cast<int>(m_settings.gridWidth - 1));
            int y1 = std::min(y0 + 1, static_cast<int>(m_settings.gridHeight - 1));
            int z1 = std::min(z0 + 1, static_cast<int>(m_settings.gridDepth - 1));

            float wx = fx - x0;
            float wy = fy - y0;
            float wz = sliceF - z0;
            wx = std::clamp(wx, 0.0f, 1.0f);
            wy = std::clamp(wy, 0.0f, 1.0f);
            wz = std::clamp(wz, 0.0f, 1.0f);

            // 8-corner trilinear weights
            float weights[8] = {
                (1 - wx) * (1 - wy) * (1 - wz), wx * (1 - wy) * (1 - wz), (1 - wx) * wy * (1 - wz), wx * wy * (1 - wz),
                (1 - wx) * (1 - wy) * wz,       wx * (1 - wy) * wz,       (1 - wx) * wy * wz,       wx * wy * wz,
            };

            uint32_t indices[8] = {
                FroxelIndex(x0, y0, z0), FroxelIndex(x1, y0, z0), FroxelIndex(x0, y1, z0), FroxelIndex(x1, y1, z0),
                FroxelIndex(x0, y0, z1), FroxelIndex(x1, y0, z1), FroxelIndex(x0, y1, z1), FroxelIndex(x1, y1, z1),
            };

            for (int i = 0; i < 8; ++i)
            {
                const FroxelData& f = m_integratedGrid[indices[i]];
                result.fogR += f.inscatterR * weights[i];
                result.fogG += f.inscatterG * weights[i];
                result.fogB += f.inscatterB * weights[i];
                result.transmittance += f.extinction * weights[i];
            }

            result.transmittance = std::clamp(result.transmittance, 0.0f, 1.0f);
            return result;
        }

        /// @brief Get current settings
        const FroxelFogSettings& GetSettings() const { return m_settings; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Get total froxel count
        uint32_t GetFroxelCount() const { return m_settings.gridWidth * m_settings.gridHeight * m_settings.gridDepth; }

      private:
        /// @brief Compute flat index from 3D froxel coordinates
        uint32_t FroxelIndex(uint32_t x, uint32_t y, uint32_t z) const
        {
            return z * m_settings.gridWidth * m_settings.gridHeight + y * m_settings.gridWidth + x;
        }

        /// @brief Henyey-Greenstein phase function
        static float HenyeyGreenstein(float cosTheta, float g)
        {
            float g2 = g * g;
            float denom = 1.0f + g2 - 2.0f * g * cosTheta;
            if (denom < 0.0001f)
                return 1.0f / (4.0f * FROXEL_PI);
            return (1.0f - g2) / (4.0f * FROXEL_PI * std::pow(denom, 1.5f));
        }

        FroxelFogSettings m_settings;
        std::vector<FroxelData> m_currentGrid;
        std::vector<FroxelData> m_historyGrid;
        std::vector<FroxelData> m_integratedGrid;
        uint32_t m_frameCount = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
