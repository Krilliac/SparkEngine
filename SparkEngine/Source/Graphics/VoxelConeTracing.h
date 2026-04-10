/**
 * @file VoxelConeTracing.h
 * @brief Voxel Cone Traced Global Illumination (VCTGI) for D3D11
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides approximate global illumination without hardware ray tracing.
 * Voxelizes the scene into a 3D texture, then traces cones through the
 * voxel grid to gather indirect diffuse and specular lighting.
 *
 * Pipeline:
 * 1. Voxelization: rasterize scene into 3D RGBA voxel grid
 * 2. Mipmap generation: build mip chain for cone filtering
 * 3. Cone tracing: trace diffuse/specular cones per pixel
 * 4. Composite: blend indirect lighting with direct lighting
 *
 * @see LightingSystem.h, ClusteredLightCulling.h, HybridRT/
 *
 * @note **Intentional reusable graphics utility** — Voxel-cone-traced global illumination — voxelizes the scene into a 3D texture and cone-traces against it for approximate GI. A future render pipeline feature; a render-side consumer will drive the voxelization + trace passes. Kept header-only so the CPU-side data layout can be adopted piece-wise.
 *
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // VCT Settings
    // =========================================================================

    struct VCTSettings
    {
        bool enabled = false;
        uint32_t voxelResolution = 128;   ///< Voxel grid resolution (128³ = 2M voxels)
        float worldExtent = 100.0f;       ///< Half-extent of the voxel grid in world units
        int diffuseCones = 6;             ///< Number of diffuse hemisphere cones
        float diffuseConeAngle = 1.047f;  ///< Diffuse cone half-angle (60°)
        float specularConeAngle = 0.087f; ///< Specular cone half-angle (5°)
        float maxTraceDistance = 50.0f;   ///< Maximum cone trace distance
        float stepMultiplier = 1.0f;      ///< Step size = voxelSize * stepMultiplier
        float indirectDiffuseStrength = 1.0f;
        float indirectSpecularStrength = 1.0f;
        float aoStrength = 1.0f;           ///< Cone-traced AO strength
        bool secondBounce = false;         ///< Enable second-bounce GI (expensive)
        bool revoxelizeEveryFrame = false; ///< Full revox each frame (dynamic scenes)
    };

    // =========================================================================
    // Voxel Grid
    // =========================================================================

    /**
     * @brief CPU-side 3D voxel grid for scene representation
     *
     * Each voxel stores RGBA (radiance + opacity). The GPU version uses
     * a 3D texture with mip chain. This CPU version is for testing and
     * validation.
     */
    class VoxelGrid
    {
      public:
        VoxelGrid() = default;
        ~VoxelGrid() = default;

        bool Initialize(uint32_t resolution, float worldExtent)
        {
            m_resolution = resolution;
            m_worldExtent = worldExtent;
            m_voxelSize = (worldExtent * 2.0f) / resolution;

            size_t totalVoxels = static_cast<size_t>(resolution) * resolution * resolution;
            m_data.resize(totalVoxels * 4, 0); // RGBA uint8

            // Build mip chain
            m_mipCount = 0;
            uint32_t mipRes = resolution;
            while (mipRes >= 1 && m_mipCount < kMaxMips)
            {
                MipLevel mip;
                mip.resolution = mipRes;
                mip.data.resize(static_cast<size_t>(mipRes) * mipRes * mipRes * 4, 0);
                m_mips[m_mipCount] = std::move(mip);
                m_mipCount++;
                if (mipRes == 1)
                    break;
                mipRes /= 2;
            }

            m_initialized = true;
            return true;
        }

        void Clear()
        {
            std::fill(m_data.begin(), m_data.end(), 0);
            for (int i = 0; i < m_mipCount; ++i)
                std::fill(m_mips[i].data.begin(), m_mips[i].data.end(), 0);
        }

        /**
         * @brief Inject a light contribution into a voxel
         * @param worldX,worldY,worldZ World position
         * @param r,g,b,a Radiance and opacity [0,255]
         */
        void InjectLight(float worldX, float worldY, float worldZ, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            int vx, vy, vz;
            if (!WorldToVoxel(worldX, worldY, worldZ, vx, vy, vz))
                return;

            size_t idx = VoxelIndex(vx, vy, vz) * 4;
            // Additive blend (saturate)
            m_data[idx + 0] = static_cast<uint8_t>(std::min(255, m_data[idx + 0] + r));
            m_data[idx + 1] = static_cast<uint8_t>(std::min(255, m_data[idx + 1] + g));
            m_data[idx + 2] = static_cast<uint8_t>(std::min(255, m_data[idx + 2] + b));
            m_data[idx + 3] = static_cast<uint8_t>(std::min(255, m_data[idx + 3] + a));
        }

        /**
         * @brief Build mip chain from mip 0 data
         */
        void BuildMipChain()
        {
            // Copy base data to mip 0
            if (m_mipCount > 0 && m_mips[0].resolution == m_resolution)
                m_mips[0].data = m_data;

            // Downsample each mip from the previous
            for (int mip = 1; mip < m_mipCount; ++mip)
            {
                auto& src = m_mips[mip - 1];
                auto& dst = m_mips[mip];
                uint32_t srcRes = src.resolution;
                uint32_t dstRes = dst.resolution;

                for (uint32_t z = 0; z < dstRes; ++z)
                {
                    for (uint32_t y = 0; y < dstRes; ++y)
                    {
                        for (uint32_t x = 0; x < dstRes; ++x)
                        {
                            // Average 2x2x2 block from source
                            float sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                            for (int dz = 0; dz < 2; ++dz)
                            {
                                for (int dy = 0; dy < 2; ++dy)
                                {
                                    for (int dx = 0; dx < 2; ++dx)
                                    {
                                        uint32_t sx = std::min(x * 2 + dx, srcRes - 1);
                                        uint32_t sy = std::min(y * 2 + dy, srcRes - 1);
                                        uint32_t sz = std::min(z * 2 + dz, srcRes - 1);
                                        size_t si = (static_cast<size_t>(sz) * srcRes * srcRes + sy * srcRes + sx) * 4;
                                        sumR += src.data[si + 0];
                                        sumG += src.data[si + 1];
                                        sumB += src.data[si + 2];
                                        sumA += src.data[si + 3];
                                    }
                                }
                            }
                            size_t di = (static_cast<size_t>(z) * dstRes * dstRes + y * dstRes + x) * 4;
                            dst.data[di + 0] = static_cast<uint8_t>(sumR / 8.0f);
                            dst.data[di + 1] = static_cast<uint8_t>(sumG / 8.0f);
                            dst.data[di + 2] = static_cast<uint8_t>(sumB / 8.0f);
                            dst.data[di + 3] = static_cast<uint8_t>(sumA / 8.0f);
                        }
                    }
                }
            }
        }

        /**
         * @brief Trace a cone through the voxel grid
         *
         * @param originX,originY,originZ Start position (world space)
         * @param dirX,dirY,dirZ Cone direction (normalized)
         * @param coneAngle Half-angle of the cone in radians
         * @param maxDist Maximum trace distance
         * @param outR,outG,outB Accumulated radiance output
         * @param outOcclusion Accumulated opacity (cone-traced AO)
         */
        void TraceCone(float originX, float originY, float originZ, float dirX, float dirY, float dirZ, float coneAngle,
                       float maxDist, float& outR, float& outG, float& outB, float& outOcclusion) const
        {
            outR = outG = outB = outOcclusion = 0.0f;
            float t = m_voxelSize; // Start one voxel away to avoid self-intersection
            float tanAngle = std::tan(coneAngle);

            while (t < maxDist && outOcclusion < 0.99f)
            {
                // Current position along the cone
                float px = originX + dirX * t;
                float py = originY + dirY * t;
                float pz = originZ + dirZ * t;

                // Cone diameter at this distance → determines mip level
                float diameter = 2.0f * t * tanAngle;
                float mipLevel = std::log2(std::max(diameter / m_voxelSize, 1.0f));
                int mip = std::clamp(static_cast<int>(mipLevel), 0, m_mipCount - 1);

                // Sample the voxel grid at this mip
                float sr, sg, sb, sa;
                SampleMip(mip, px, py, pz, sr, sg, sb, sa);

                // Front-to-back compositing
                float opacity = sa * (1.0f - outOcclusion);
                outR += sr * opacity;
                outG += sg * opacity;
                outB += sb * opacity;
                outOcclusion += opacity;

                // Step size proportional to cone diameter (coarser = larger steps)
                t += std::max(diameter, m_voxelSize) * m_stepMultiplier;
            }
        }

        // --- Accessors ---
        uint32_t GetResolution() const { return m_resolution; }
        float GetVoxelSize() const { return m_voxelSize; }
        float GetWorldExtent() const { return m_worldExtent; }
        int GetMipCount() const { return m_mipCount; }
        bool IsInitialized() const { return m_initialized; }

        void SetStepMultiplier(float mult) { m_stepMultiplier = mult; }

        std::string Console_GetStatus() const
        {
            size_t totalBytes = m_data.size();
            for (int i = 0; i < m_mipCount; ++i)
                totalBytes += m_mips[i].data.size();
            return "VoxelGrid: " + std::to_string(m_resolution) + "³, " + std::to_string(m_mipCount) + " mips, " +
                   std::to_string(totalBytes / 1024) + "KB\n";
        }

      private:
        static constexpr int kMaxMips = 8;

        bool WorldToVoxel(float wx, float wy, float wz, int& vx, int& vy, int& vz) const
        {
            vx = static_cast<int>((wx + m_worldExtent) / (m_worldExtent * 2.0f) * m_resolution);
            vy = static_cast<int>((wy + m_worldExtent) / (m_worldExtent * 2.0f) * m_resolution);
            vz = static_cast<int>((wz + m_worldExtent) / (m_worldExtent * 2.0f) * m_resolution);
            return vx >= 0 && vx < static_cast<int>(m_resolution) && vy >= 0 && vy < static_cast<int>(m_resolution) &&
                   vz >= 0 && vz < static_cast<int>(m_resolution);
        }

        size_t VoxelIndex(int x, int y, int z) const
        {
            return static_cast<size_t>(z) * m_resolution * m_resolution + y * m_resolution + x;
        }

        void SampleMip(int mip, float wx, float wy, float wz, float& r, float& g, float& b, float& a) const
        {
            r = g = b = a = 0.0f;
            if (mip < 0 || mip >= m_mipCount)
                return;

            const auto& ml = m_mips[mip];
            float normX = (wx + m_worldExtent) / (m_worldExtent * 2.0f);
            float normY = (wy + m_worldExtent) / (m_worldExtent * 2.0f);
            float normZ = (wz + m_worldExtent) / (m_worldExtent * 2.0f);

            int vx = std::clamp(static_cast<int>(normX * ml.resolution), 0, static_cast<int>(ml.resolution - 1));
            int vy = std::clamp(static_cast<int>(normY * ml.resolution), 0, static_cast<int>(ml.resolution - 1));
            int vz = std::clamp(static_cast<int>(normZ * ml.resolution), 0, static_cast<int>(ml.resolution - 1));

            size_t idx = (static_cast<size_t>(vz) * ml.resolution * ml.resolution + vy * ml.resolution + vx) * 4;
            r = ml.data[idx + 0] / 255.0f;
            g = ml.data[idx + 1] / 255.0f;
            b = ml.data[idx + 2] / 255.0f;
            a = ml.data[idx + 3] / 255.0f;
        }

        struct MipLevel
        {
            uint32_t resolution = 0;
            std::vector<uint8_t> data;
        };

        uint32_t m_resolution = 0;
        float m_worldExtent = 0.0f;
        float m_voxelSize = 0.0f;
        float m_stepMultiplier = 1.0f;
        int m_mipCount = 0;
        std::vector<uint8_t> m_data;
        MipLevel m_mips[kMaxMips];
        bool m_initialized = false;
    };

    // =========================================================================
    // VCT GI System
    // =========================================================================

    /**
     * @brief Voxel Cone Traced Global Illumination system
     *
     * Coordinates the full VCT pipeline: voxelization, mip generation,
     * cone tracing, and compositing. Works on D3D11 (no RT hardware needed).
     */
    class VCTSystem
    {
      public:
        VCTSystem() = default;
        ~VCTSystem() = default;

        bool Initialize(const VCTSettings& settings = {})
        {
            m_settings = settings;
            return m_grid.Initialize(settings.voxelResolution, settings.worldExtent);
        }

        void Shutdown() { m_grid = VoxelGrid(); }

        /** @brief Clear voxel grid for revoxelization */
        void BeginVoxelization() { m_grid.Clear(); }

        /**
         * @brief Inject a light into the voxel grid
         */
        void InjectLight(float x, float y, float z, float r, float g, float b, float intensity)
        {
            uint8_t ur = static_cast<uint8_t>(std::clamp(r * intensity * 255.0f, 0.0f, 255.0f));
            uint8_t ug = static_cast<uint8_t>(std::clamp(g * intensity * 255.0f, 0.0f, 255.0f));
            uint8_t ub = static_cast<uint8_t>(std::clamp(b * intensity * 255.0f, 0.0f, 255.0f));
            m_grid.InjectLight(x, y, z, ur, ug, ub, 255);
        }

        /** @brief Inject geometry (opacity) into the voxel grid */
        void InjectGeometry(float x, float y, float z, uint8_t opacity = 255)
        {
            m_grid.InjectLight(x, y, z, 0, 0, 0, opacity);
        }

        /** @brief Finalize voxelization and build mip chain */
        void EndVoxelization() { m_grid.BuildMipChain(); }

        /**
         * @brief Trace indirect diffuse illumination at a surface point
         *
         * Traces multiple cones in a hemisphere around the normal to gather
         * indirect diffuse lighting from the voxel grid.
         */
        void TraceDiffuse(float posX, float posY, float posZ, float normalX, float normalY, float normalZ, float& outR,
                          float& outG, float& outB, float& outAO) const
        {
            outR = outG = outB = outAO = 0.0f;

            // Generate cone directions around the normal
            // Using a fixed set of directions in the hemisphere
            float tangentX, tangentY, tangentZ;
            float bitangentX, bitangentY, bitangentZ;
            ComputeTangentFrame(normalX, normalY, normalZ, tangentX, tangentY, tangentZ, bitangentX, bitangentY,
                                bitangentZ);

            // 6 cones spread over the hemisphere
            static const float coneOffsets[6][2] = {{0, 0},      {0.866f, 0},  {-0.866f, 0},
                                                    {0, 0.866f}, {0, -0.866f}, {0.433f, 0.75f}};

            for (int i = 0; i < m_settings.diffuseCones; ++i)
            {
                float u = (i < 6) ? coneOffsets[i][0] : 0.0f;
                float v = (i < 6) ? coneOffsets[i][1] : 0.0f;
                float w = std::sqrt(std::max(0.0f, 1.0f - u * u - v * v));

                float dirX = tangentX * u + bitangentX * v + normalX * w;
                float dirY = tangentY * u + bitangentY * v + normalY * w;
                float dirZ = tangentZ * u + bitangentZ * v + normalZ * w;

                // Normalize
                float len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
                if (len > 0.0001f)
                {
                    dirX /= len;
                    dirY /= len;
                    dirZ /= len;
                }

                float cr, cg, cb, cocc;
                m_grid.TraceCone(posX, posY, posZ, dirX, dirY, dirZ, m_settings.diffuseConeAngle,
                                 m_settings.maxTraceDistance, cr, cg, cb, cocc);

                outR += cr;
                outG += cg;
                outB += cb;
                outAO += cocc;
            }

            float invCones = 1.0f / std::max(m_settings.diffuseCones, 1);
            outR *= m_settings.indirectDiffuseStrength * invCones;
            outG *= m_settings.indirectDiffuseStrength * invCones;
            outB *= m_settings.indirectDiffuseStrength * invCones;
            outAO *= m_settings.aoStrength * invCones;
        }

        /**
         * @brief Trace indirect specular reflection at a surface point
         */
        void TraceSpecular(float posX, float posY, float posZ, float reflectX, float reflectY, float reflectZ,
                           float roughness, float& outR, float& outG, float& outB) const
        {
            float coneAngle = std::max(roughness * 1.047f, m_settings.specularConeAngle);
            float occ;
            m_grid.TraceCone(posX, posY, posZ, reflectX, reflectY, reflectZ, coneAngle, m_settings.maxTraceDistance,
                             outR, outG, outB, occ);
            outR *= m_settings.indirectSpecularStrength;
            outG *= m_settings.indirectSpecularStrength;
            outB *= m_settings.indirectSpecularStrength;
        }

        VCTSettings& GetSettings() { return m_settings; }
        const VCTSettings& GetSettings() const { return m_settings; }
        const VoxelGrid& GetGrid() const { return m_grid; }

        std::string Console_GetStatus() const
        {
            return "VCTSystem: " + std::string(m_settings.enabled ? "ON" : "OFF") + " " + m_grid.Console_GetStatus();
        }

      private:
        static void ComputeTangentFrame(float nx, float ny, float nz, float& tx, float& ty, float& tz, float& bx,
                                        float& by, float& bz)
        {
            if (std::abs(nx) > 0.9f)
            {
                tx = 0;
                ty = 1;
                tz = 0;
            }
            else
            {
                tx = 1;
                ty = 0;
                tz = 0;
            }
            // Cross product: t = t x n
            float cx = ty * nz - tz * ny;
            float cy = tz * nx - tx * nz;
            float cz = tx * ny - ty * nx;
            float len = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (len > 0.0001f)
            {
                tx = cx / len;
                ty = cy / len;
                tz = cz / len;
            }
            // Bitangent = n x t
            bx = ny * tz - nz * ty;
            by = nz * tx - nx * tz;
            bz = nx * ty - ny * tx;
        }

        VCTSettings m_settings;
        VoxelGrid m_grid;
    };

} // namespace Spark::Graphics
