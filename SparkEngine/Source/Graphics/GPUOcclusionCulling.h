/**
 * @file GPUOcclusionCulling.h
 * @brief GPU-driven occlusion culling using previous frame's hierarchical Z-buffer
 * @author Spark Engine Team
 * @date 2025
 *
 * Culls objects on the GPU by testing their bounding boxes against the
 * previous frame's depth buffer (reprojected). Uses a hierarchical Z-buffer
 * (HiZ) pyramid for efficient coarse-to-fine testing.
 *
 * Pipeline: Depth Prepass → Build HiZ → Cull objects → Draw visible
 *
 * @see OcclusionCulling.h, BVHAccelerator.h, FrustumCulling.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // HiZ Pyramid
    // =========================================================================

    /**
     * @brief Hierarchical Z-buffer for GPU occlusion queries
     *
     * Stores maximum depth values at progressively coarser resolutions.
     * Mip 0 = full resolution, Mip N = 1x1 (single max depth value).
     * A bounding box is occluded if its nearest depth > HiZ value at its screen extent.
     */
    struct HiZPyramid
    {
        static constexpr int kMaxMips = 12; // Up to 4096x4096

        uint32_t width = 0;
        uint32_t height = 0;
        int mipCount = 0;

        // CPU-side HiZ data (for testing; GPU version uses UAV textures)
        struct MipLevel
        {
            std::vector<float> depths;
            uint32_t width = 0;
            uint32_t height = 0;
        };
        MipLevel mips[kMaxMips];

        void Build(const float* depthBuffer, uint32_t w, uint32_t h)
        {
            width = w;
            height = h;
            mipCount = 0;

            // Mip 0: copy source depth
            uint32_t mw = w;
            uint32_t mh = h;
            while (mw >= 1 && mh >= 1 && mipCount < kMaxMips)
            {
                auto& mip = mips[mipCount];
                mip.width = mw;
                mip.height = mh;
                mip.depths.resize(mw * mh);

                if (mipCount == 0)
                {
                    // Copy source depth
                    for (uint32_t i = 0; i < mw * mh; ++i)
                        mip.depths[i] = depthBuffer[i];
                }
                else
                {
                    // Downsample: take max of 2x2 block from previous mip
                    const auto& prev = mips[mipCount - 1];
                    for (uint32_t y = 0; y < mh; ++y)
                    {
                        for (uint32_t x = 0; x < mw; ++x)
                        {
                            uint32_t px = std::min(x * 2, prev.width - 1);
                            uint32_t py = std::min(y * 2, prev.height - 1);
                            uint32_t px1 = std::min(px + 1, prev.width - 1);
                            uint32_t py1 = std::min(py + 1, prev.height - 1);

                            float d00 = prev.depths[py * prev.width + px];
                            float d10 = prev.depths[py * prev.width + px1];
                            float d01 = prev.depths[py1 * prev.width + px];
                            float d11 = prev.depths[py1 * prev.width + px1];

                            mip.depths[y * mw + x] = std::max({d00, d10, d01, d11});
                        }
                    }
                }

                mipCount++;
                if (mw == 1 && mh == 1)
                    break;
                mw = std::max(mw / 2, 1u);
                mh = std::max(mh / 2, 1u);
            }
        }

        /**
         * @brief Sample HiZ at a given mip level
         * @return Maximum depth value at (x,y) in the specified mip
         */
        float Sample(int mip, uint32_t x, uint32_t y) const
        {
            mip = std::clamp(mip, 0, mipCount - 1);
            const auto& m = mips[mip];
            x = std::min(x, m.width - 1);
            y = std::min(y, m.height - 1);
            return m.depths[y * m.width + x];
        }
    };

    // =========================================================================
    // AABB for occlusion testing
    // =========================================================================

    struct OcclusionAABB
    {
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
        uint32_t objectId = 0;
    };

    // =========================================================================
    // GPU Occlusion Culler
    // =========================================================================

    /**
     * @brief Culls objects against previous frame's hierarchical Z-buffer
     *
     * CPU reference implementation. The GPU version runs as a compute shader
     * that reads the HiZ texture and writes a visibility buffer.
     */
    class GPUOcclusionCuller
    {
      public:
        GPUOcclusionCuller() = default;
        ~GPUOcclusionCuller() = default;

        void Initialize(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
            m_initialized = true;
        }

        void Shutdown() { m_initialized = false; }

        /** @brief Build the HiZ pyramid from the previous frame's depth buffer */
        void BuildHiZ(const float* depthBuffer, uint32_t w, uint32_t h) { m_hiZ.Build(depthBuffer, w, h); }

        /**
         * @brief Test if an AABB is visible against the HiZ
         *
         * Projects the AABB to screen space, selects the appropriate HiZ mip
         * level based on screen extent, and compares the AABB's nearest depth
         * against the HiZ maximum depth.
         *
         * @param aabb       Object bounding box in clip space [0,1] after projection
         * @param nearDepth  Nearest depth of the AABB (in [0,1] depth range)
         * @return true if visible, false if occluded
         */
        bool IsVisible(float screenMinX, float screenMinY, float screenMaxX, float screenMaxY, float nearDepth) const
        {
            if (!m_initialized || m_hiZ.mipCount == 0)
                return true; // Conservative: visible if no data

            // Clamp to screen
            screenMinX = std::clamp(screenMinX, 0.0f, 1.0f);
            screenMinY = std::clamp(screenMinY, 0.0f, 1.0f);
            screenMaxX = std::clamp(screenMaxX, 0.0f, 1.0f);
            screenMaxY = std::clamp(screenMaxY, 0.0f, 1.0f);

            // Calculate screen extent in pixels
            float extentX = (screenMaxX - screenMinX) * m_hiZ.width;
            float extentY = (screenMaxY - screenMinY) * m_hiZ.height;
            float maxExtent = std::max(extentX, extentY);

            // Select mip level based on extent (1 texel per pixel of extent)
            int mip = 0;
            if (maxExtent > 0)
                mip = static_cast<int>(std::ceil(std::log2(maxExtent)));
            mip = std::clamp(mip, 0, m_hiZ.mipCount - 1);

            // Sample HiZ at the four corners of the projected AABB
            const auto& mipLevel = m_hiZ.mips[mip];
            uint32_t x0 = static_cast<uint32_t>(screenMinX * mipLevel.width);
            uint32_t y0 = static_cast<uint32_t>(screenMinY * mipLevel.height);
            uint32_t x1 = static_cast<uint32_t>(screenMaxX * mipLevel.width);
            uint32_t y1 = static_cast<uint32_t>(screenMaxY * mipLevel.height);

            float maxHiZ = 0.0f;
            maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x0, y0));
            maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x1, y0));
            maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x0, y1));
            maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x1, y1));

            // Object is occluded if its nearest depth is behind the farthest HiZ
            return nearDepth <= maxHiZ;
        }

        /**
         * @brief Batch cull multiple objects
         * @return Indices of visible objects
         */
        std::vector<uint32_t> CullBatch(const std::vector<OcclusionAABB>& objects, const float* viewProjMatrix) const
        {
            std::vector<uint32_t> visible;
            visible.reserve(objects.size());

            for (size_t i = 0; i < objects.size(); ++i)
            {
                // Simplified: assume objects provide screen-space bounds
                // Real implementation would transform AABB by viewProj matrix
                // For now, mark all visible (conservative)
                visible.push_back(static_cast<uint32_t>(i));
            }

            return visible;
        }

        const HiZPyramid& GetHiZ() const { return m_hiZ; }
        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            return "GPUOcclusionCuller: HiZ " + std::to_string(m_hiZ.width) + "x" + std::to_string(m_hiZ.height) +
                   " (" + std::to_string(m_hiZ.mipCount) + " mips)\n";
        }

      private:
        HiZPyramid m_hiZ;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
