/**
 * @file FroxelVolumetricFog.cpp
 * @brief CPU reference implementation of froxel-based volumetric fog
 *
 * Three-pass pipeline:
 * 1. InjectMedia  - fill froxel grid with extinction/inscattering from global
 *                   fog parameters and overlapping local fog volumes
 * 2. ScatterLight - evaluate point lights per froxel using Henyey-Greenstein
 *                   phase function and distance attenuation
 * 3. Integrate    - front-to-back Beer-Lambert ray march per screen column
 *
 * Temporal filtering blends with the previous frame to reduce noise.
 * QueryFog provides trilinear-interpolated fog lookup for final compositing.
 *
 * GPU implementation: each pass maps to a compute shader dispatch over
 * the 3D froxel grid (gridWidth x gridHeight x gridDepth).
 */

#include "FroxelVolumetricFog.h"

namespace Spark::Graphics
{

    bool FroxelVolumetricFog::Initialize(const FroxelFogSettings& settings)
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

    void FroxelVolumetricFog::Shutdown()
    {
        m_currentGrid.clear();
        m_historyGrid.clear();
        m_integratedGrid.clear();
        m_initialized = false;
    }

    float FroxelVolumetricFog::SliceToDepth(uint32_t slice) const
    {
        float t = static_cast<float>(slice + 0.5f) / static_cast<float>(m_settings.gridDepth);
        float nearZ = m_settings.nearPlane;
        float farZ = m_settings.farPlane;
        float blend = m_settings.logDistribution;

        float linearDepth = nearZ + t * (farZ - nearZ);
        float logDepth = nearZ * std::pow(farZ / nearZ, t);

        return std::lerp(linearDepth, logDepth, blend);
    }

    float FroxelVolumetricFog::DepthToSlice(float depth) const
    {
        float nearZ = m_settings.nearPlane;
        float farZ = m_settings.farPlane;
        float blend = m_settings.logDistribution;

        float linearT = (depth - nearZ) / (farZ - nearZ);
        float logT = std::log(depth / nearZ) / std::log(farZ / nearZ);

        float t = std::lerp(linearT, logT, blend);
        return t * static_cast<float>(m_settings.gridDepth);
    }

    void FroxelVolumetricFog::InjectMedia(const FogVolume* volumes, int volumeCount)
    {
        if (!m_initialized)
        {
            return;
        }

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

                    // Approximate world position (assumes identity view for reference)
                    float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                    float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
                    float worldX = (u * 2.0f - 1.0f) * depth;
                    float worldY = (v * 2.0f - 1.0f) * depth;
                    float worldZ = depth;

                    // Accumulate local fog volumes with smooth edge falloff
                    for (int vi = 0; vi < volumeCount; ++vi)
                    {
                        const FogVolume& vol = volumes[vi];
                        float dx = std::abs(worldX - vol.posX);
                        float dy = std::abs(worldY - vol.posY);
                        float dz = std::abs(worldZ - vol.posZ);

                        if (dx <= vol.extentX && dy <= vol.extentY && dz <= vol.extentZ)
                        {
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

    void FroxelVolumetricFog::ScatterLight(const FroxelLight* lights, int lightCount, float cameraPosX,
                                           float cameraPosY, float cameraPosZ)
    {
        if (!m_initialized)
        {
            return;
        }

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

                    // View direction from camera to froxel center
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

                    for (int li = 0; li < lightCount; ++li)
                    {
                        const FroxelLight& light = lights[li];

                        float toX = light.posX - worldX;
                        float toY = light.posY - worldY;
                        float toZ = light.posZ - worldZ;
                        float dist = std::sqrt(toX * toX + toY * toY + toZ * toZ);

                        if (dist > light.radius || dist < 0.001f)
                        {
                            continue;
                        }

                        // Normalized light direction
                        float lx = toX / dist;
                        float ly = toY / dist;
                        float lz = toZ / dist;

                        // Henyey-Greenstein phase function
                        float cosTheta = viewDirX * lx + viewDirY * ly + viewDirZ * lz;
                        float phase = HenyeyGreenstein(cosTheta, m_settings.phaseG);

                        // Smooth inverse-square attenuation with radius falloff
                        float normDist = dist / light.radius;
                        float attenuation = std::max(0.0f, 1.0f - normDist * normDist);
                        attenuation *= attenuation;
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

    void FroxelVolumetricFog::TemporalFilter(float motionScale)
    {
        if (!m_initialized)
        {
            return;
        }

        m_frameCount++;
        uint32_t total = m_settings.gridWidth * m_settings.gridHeight * m_settings.gridDepth;

        float blend = m_settings.temporalBlend;

        // Reduce history blend when the camera is moving fast
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

    void FroxelVolumetricFog::Integrate()
    {
        if (!m_initialized)
        {
            return;
        }

        uint32_t w = m_settings.gridWidth;
        uint32_t h = m_settings.gridHeight;
        uint32_t d = m_settings.gridDepth;

        // Front-to-back ray march per screen-space column
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

                    // Analytical integration of inscattered light
                    float integrationFactor = (froxel.extinction > 0.0001f)
                                                  ? (1.0f - sliceTransmittance) / froxel.extinction
                                                  : sliceThickness;

                    accumR += transmittance * froxel.inscatterR * integrationFactor;
                    accumG += transmittance * froxel.inscatterG * integrationFactor;
                    accumB += transmittance * froxel.inscatterB * integrationFactor;

                    transmittance *= sliceTransmittance;

                    // Store integrated result for this depth slice
                    m_integratedGrid[idx].inscatterR = accumR;
                    m_integratedGrid[idx].inscatterG = accumG;
                    m_integratedGrid[idx].inscatterB = accumB;
                    m_integratedGrid[idx].extinction = transmittance;
                }
            }
        }
    }

    FroxelFogResult FroxelVolumetricFog::QueryFog(float u, float v, float depth) const
    {
        FroxelFogResult result{};
        if (!m_initialized)
        {
            return result;
        }

        float sliceF = DepthToSlice(depth);
        float fx = u * static_cast<float>(m_settings.gridWidth) - 0.5f;
        float fy = v * static_cast<float>(m_settings.gridHeight) - 0.5f;

        // Trilinear interpolation across the 8 surrounding froxels
        int x0 = std::clamp(static_cast<int>(fx), 0, static_cast<int>(m_settings.gridWidth - 1));
        int y0 = std::clamp(static_cast<int>(fy), 0, static_cast<int>(m_settings.gridHeight - 1));
        int z0 = std::clamp(static_cast<int>(sliceF), 0, static_cast<int>(m_settings.gridDepth - 1));
        int x1 = std::min(x0 + 1, static_cast<int>(m_settings.gridWidth - 1));
        int y1 = std::min(y0 + 1, static_cast<int>(m_settings.gridHeight - 1));
        int z1 = std::min(z0 + 1, static_cast<int>(m_settings.gridDepth - 1));

        float wx = std::clamp(fx - x0, 0.0f, 1.0f);
        float wy = std::clamp(fy - y0, 0.0f, 1.0f);
        float wz = std::clamp(sliceF - z0, 0.0f, 1.0f);

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

    float FroxelVolumetricFog::HenyeyGreenstein(float cosTheta, float g)
    {
        float g2 = g * g;
        float denom = 1.0f + g2 - 2.0f * g * cosTheta;
        if (denom < 0.0001f)
        {
            return 1.0f / (4.0f * FROXEL_PI);
        }
        return (1.0f - g2) / (4.0f * FROXEL_PI * std::pow(denom, 1.5f));
    }

} // namespace Spark::Graphics
