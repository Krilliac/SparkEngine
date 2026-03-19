/**
 * @file OcclusionCulling.h
 * @brief CPU software occlusion culling with a low-resolution depth buffer
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a CPU-side software rasterizer that renders simplified occluder meshes
 * into a low-resolution depth buffer, then tests AABB bounding boxes against it.
 * Objects fully occluded by the depth buffer are skipped, reducing GPU draw calls.
 *
 * ## Usage
 * @code
 *   auto& occ = Spark::Graphics::OcclusionCullingSystem::GetInstance();
 *   occ.Initialize({.bufferWidth = 256, .bufferHeight = 144});
 *
 *   occ.BeginFrame(viewMatrix, projMatrix);
 *   occ.SubmitOccluder(wallMesh, wallWorldMatrix);
 *   bool visible = occ.TestVisibility({boundsMin, boundsMax});
 *   auto stats = occ.EndFrame();
 * @endcode
 *
 * @see FrustumCulling, RenderPipeline
 */

#pragma once

#include "../Core/Platform.h"
#include "FrustumCulling.h"

#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Occlusion Settings
    // =========================================================================

    /**
     * @brief Configuration for the software occlusion culling system.
     */
    struct OcclusionSettings
    {
        int bufferWidth = 256;         ///< Low-res depth buffer width in pixels
        int bufferHeight = 144;        ///< Low-res depth buffer height in pixels
        int maxOccluders = 64;         ///< Maximum occluders rasterized per frame
        float minOccluderArea = 0.01f; ///< Minimum screen-space area to be an occluder
        bool enabled = true;           ///< Master enable/disable
    };

    // =========================================================================
    // Occluder Mesh
    // =========================================================================

    /**
     * @brief A simplified mesh used as an occluder for depth buffer rasterization.
     *
     * Should be a low-poly proxy (e.g. building bounding shape). Vertices are in
     * local/model space; a world transform is provided at submission time.
     */
    struct OccluderMesh
    {
        std::vector<XMFLOAT3> vertices;
        std::vector<uint32_t> indices;
    };

    // =========================================================================
    // Occlusion Query
    // =========================================================================

    /**
     * @brief An axis-aligned bounding box to test for occlusion.
     */
    struct OcclusionQuery
    {
        XMFLOAT3 boundsMin = {0.0f, 0.0f, 0.0f};
        XMFLOAT3 boundsMax = {0.0f, 0.0f, 0.0f};
    };

    // =========================================================================
    // Frame Statistics
    // =========================================================================

    /**
     * @brief Per-frame occlusion culling statistics.
     */
    struct OcclusionStats
    {
        uint32_t testedCount = 0;         ///< Number of queries tested this frame
        uint32_t occludedCount = 0;       ///< Number of queries determined to be occluded
        uint32_t occludersSubmitted = 0;  ///< Number of occluder meshes rasterized
        uint32_t trianglesRasterized = 0; ///< Total occluder triangles rasterized
    };

    // =========================================================================
    // OcclusionCullingSystem
    // =========================================================================

    /**
     * @class OcclusionCullingSystem
     * @brief CPU software occlusion culling using a low-resolution depth buffer.
     *
     * Each frame:
     * 1. BeginFrame() clears the depth buffer and stores the view-projection matrix.
     * 2. SubmitOccluder() rasterizes large occluder meshes into the depth buffer.
     * 3. TestVisibility() projects AABBs and checks if they are fully behind the depth buffer.
     * 4. EndFrame() returns per-frame statistics.
     */
    class OcclusionCullingSystem
    {
      public:
        /** @brief Singleton access. */
        static OcclusionCullingSystem& GetInstance()
        {
            static OcclusionCullingSystem s;
            return s;
        }

        /**
         * @brief Initialize the depth buffer and internal state.
         * @param settings  Buffer dimensions and limits.
         * @return True on success.
         */
        bool Initialize(const OcclusionSettings& settings = {});

        /** @brief Release the depth buffer. */
        void Shutdown();

        /**
         * @brief Begin a new frame — clear the depth buffer and store matrices.
         * @param viewMatrix  Camera view matrix.
         * @param projMatrix  Camera projection matrix.
         */
        void BeginFrame(const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix);

        /**
         * @brief Rasterize an occluder mesh into the depth buffer.
         * @param mesh         Simplified occluder geometry.
         * @param worldMatrix  Model-to-world transform for the occluder.
         */
        void SubmitOccluder(const OccluderMesh& mesh, const XMFLOAT4X4& worldMatrix);

        /**
         * @brief Test an AABB against the depth buffer for visibility.
         * @param query  The bounding box to test.
         * @return       True if the AABB is potentially visible (not fully occluded).
         */
        bool TestVisibility(const OcclusionQuery& query) const;

        /**
         * @brief Finish the frame and return statistics.
         * @return Per-frame occlusion statistics.
         */
        OcclusionStats EndFrame();

        /** @brief Get the number of objects occluded this frame. */
        uint32_t GetOccludedCount() const { return m_stats.occludedCount; }

        /** @brief Get the number of objects tested this frame. */
        uint32_t GetTestedCount() const { return m_stats.testedCount; }

        /** @brief Check if the system is enabled. */
        bool IsEnabled() const { return m_settings.enabled; }

      private:
        OcclusionCullingSystem() = default;
        ~OcclusionCullingSystem() = default;
        OcclusionCullingSystem(const OcclusionCullingSystem&) = delete;
        OcclusionCullingSystem& operator=(const OcclusionCullingSystem&) = delete;

        /**
         * @brief Transform a point through the full MVP pipeline to screen space.
         * @param point       World-space point.
         * @param outScreenX  Output screen X.
         * @param outScreenY  Output screen Y.
         * @param outDepth    Output normalized depth.
         * @return            True if the point is in front of the camera.
         */
        bool ProjectToScreen(const XMFLOAT3& point, float& outScreenX, float& outScreenY, float& outDepth) const;

        /**
         * @brief Rasterize a single triangle into the depth buffer.
         * @param x0, y0, z0  Screen-space vertex 0.
         * @param x1, y1, z1  Screen-space vertex 1.
         * @param x2, y2, z2  Screen-space vertex 2.
         */
        void RasterizeTriangle(float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2,
                               float z2);

        /**
         * @brief Test if a screen-space rect is fully behind the depth buffer.
         * @param minX, minY, maxX, maxY  Screen-space bounding rectangle.
         * @param minDepth                Nearest depth of the object.
         * @return True if fully occluded.
         */
        bool IsRectOccluded(int minX, int minY, int maxX, int maxY, float minDepth) const;

        bool m_initialized = false;
        OcclusionSettings m_settings;
        mutable OcclusionStats m_stats;

        std::vector<float> m_depthBuffer;
        XMFLOAT4X4 m_viewProj = {};
    };

} // namespace Spark::Graphics
