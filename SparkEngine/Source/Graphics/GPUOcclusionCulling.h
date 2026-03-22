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
 * Pipeline: Depth Prepass -> Build HiZ -> Cull objects -> Draw visible
 *
 * @see OcclusionCulling.h, BVHAccelerator.h, FrustumCulling.h
 */

#pragma once

#include "../Core/Platform.h"

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

        /// @brief Build the HiZ mip chain from a full-resolution depth buffer
        void Build(const float* depthBuffer, uint32_t w, uint32_t h);

        /**
         * @brief Sample HiZ at a given mip level
         * @return Maximum depth value at (x,y) in the specified mip
         */
        float Sample(int mip, uint32_t x, uint32_t y) const;
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

        /// @brief Initialize the culler for a given screen resolution
        void Initialize(uint32_t width, uint32_t height);

        /// @brief Release resources
        void Shutdown();

        /// @brief Build the HiZ pyramid from the previous frame's depth buffer
        void BuildHiZ(const float* depthBuffer, uint32_t w, uint32_t h);

        /**
         * @brief Test if an AABB is visible against the HiZ
         *
         * Projects the AABB to screen space, selects the appropriate HiZ mip
         * level based on screen extent, and compares the AABB's nearest depth
         * against the HiZ maximum depth.
         *
         * @param screenMinX  Left edge in screen UV [0,1]
         * @param screenMinY  Top edge in screen UV [0,1]
         * @param screenMaxX  Right edge in screen UV [0,1]
         * @param screenMaxY  Bottom edge in screen UV [0,1]
         * @param nearDepth   Nearest depth of the AABB (in [0,1] depth range)
         * @return true if visible, false if occluded
         */
        bool IsVisible(float screenMinX, float screenMinY, float screenMaxX, float screenMaxY, float nearDepth) const;

        /**
         * @brief Batch cull multiple objects by projecting AABBs through viewProj
         * @param objects        Array of world-space AABBs to test
         * @param viewProjMatrix 4x4 row-major view-projection matrix (16 floats)
         * @return Indices of visible objects
         */
        std::vector<uint32_t> CullBatch(const std::vector<OcclusionAABB>& objects, const float* viewProjMatrix) const;

        /// @brief Access the HiZ pyramid data
        const HiZPyramid& GetHiZ() const { return m_hiZ; }

        /// @brief Check if the culler has been initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Get a human-readable status string for the console
        std::string Console_GetStatus() const;

      private:
        HiZPyramid m_hiZ;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
