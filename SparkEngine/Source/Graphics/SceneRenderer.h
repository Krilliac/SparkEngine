/**
 * @file SceneRenderer.h
 * @brief ECS-based scene rendering — collects draw commands and feeds the pipeline (R3.1, R3.5)
 *
 * Extracted from GraphicsEngine to separate scene traversal and draw command
 * collection from device management and pipeline execution. SceneRenderer is
 * the bridge between the ECS world and the render pipeline.
 *
 * Consolidates the dual render path (R3.5): both legacy GameObject-based and
 * ECS-based rendering flow through the same draw command system.
 *
 * @threadsafety Main thread only for submission. Draw list is single-writer.
 */

#pragma once

#include "../Core/Platform.h"
#include "../Core/AssetHandle.h"
#include "../Utils/FrameAllocator.h"
#include "BVHAccelerator.h"
#include "PortalCulling.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Data for a single mesh draw command.
     *
     * Uses AssetHandle (hashed uint64_t) instead of strings to eliminate
     * per-frame heap allocations. Includes a sort key for front-to-back
     * or material-based draw order optimization.
     */
    struct DrawCommand
    {
        AssetHandle mesh;                ///< Hashed mesh asset path
        AssetHandle material;            ///< Hashed material asset path
        DirectX::XMFLOAT4X4 worldMatrix; ///< World transformation
        uint32_t sortKey = 0;            ///< Draw sort key for batching
        float distanceToCamera = 0.0f;   ///< Distance for depth sorting
        bool castShadows = true;         ///< Include in shadow pass
        bool receiveShadows = true;      ///< Receive shadows
        uint8_t lodLevel = 0;            ///< Level of detail index
    };

    /**
     * @brief Collects and manages per-frame draw commands from the ECS.
     *
     * SceneRenderer is responsible for:
     * - Accepting draw submissions from the ECS RenderSystem
     * - Performing frustum culling using the BVH accelerator
     * - Sorting draw commands for optimal GPU state changes
     * - Providing the sorted draw list to the RenderPipeline
     *
     * ### Draw submission flow
     * ```
     * ECS RenderSystem → SceneRenderer::Submit() → Cull → Sort → RenderPipeline::Execute()
     * ```
     *
     * ### Thread safety
     * Submit() must be called from the main thread only (single writer).
     * The draw list uses a FrameAllocator to avoid per-frame heap allocation.
     */
    class SceneRenderer
    {
      public:
        SceneRenderer() = default;
        ~SceneRenderer() = default;

        /**
         * @brief Initialize the scene renderer.
         * @param maxDrawCommands  Maximum draw commands per frame.
         * @return true on success.
         */
        bool Initialize(uint32_t maxDrawCommands = 8192);

        /**
         * @brief Shut down and release resources.
         */
        void Shutdown();

        /**
         * @brief Begin a new frame — resets the draw list.
         *
         * Must be called at the start of each frame before any Submit() calls.
         */
        void BeginFrame();

        /**
         * @brief Submit a mesh draw command for the current frame.
         *
         * @param meshHandle     Hashed mesh asset handle.
         * @param materialHandle Hashed material asset handle.
         * @param worldMatrix    World transformation matrix.
         * @param castShadows    Whether this mesh casts shadows.
         */
        void Submit(AssetHandle meshHandle, AssetHandle materialHandle, const DirectX::XMMATRIX& worldMatrix,
                    bool castShadows = true);

        /**
         * @brief Submit using string paths (convenience, hashes internally).
         */
        void Submit(const std::string& meshPath, const std::string& materialPath, const DirectX::XMMATRIX& worldMatrix,
                    bool castShadows = true);

        /**
         * @brief Perform frustum culling on the submitted draw commands.
         *
         * Uses the BVH accelerator to reject draw commands outside the
         * camera frustum. Must be called after all Submit() calls and
         * before GetVisibleCommands().
         *
         * @param viewMatrix  Camera view matrix.
         * @param projMatrix  Camera projection matrix.
         * @param cameraPos   Camera world position (for distance sorting).
         */
        void CullAndSort(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                         const DirectX::XMFLOAT3& cameraPos);

        /**
         * @brief End the frame — resets the frame allocator.
         */
        void EndFrame();

        // ====================================================================
        // Accessors
        // ====================================================================

        /** @brief Get all submitted draw commands (before culling). */
        const std::vector<DrawCommand>& GetAllCommands() const { return m_drawCommands; }

        /** @brief Get visible draw commands (after culling and sorting). */
        const std::vector<DrawCommand>& GetVisibleCommands() const { return m_visibleCommands; }

        /** @brief Get total submitted count this frame. */
        uint32_t GetSubmittedCount() const { return static_cast<uint32_t>(m_drawCommands.size()); }

        /** @brief Get visible count after culling this frame. */
        uint32_t GetVisibleCount() const { return static_cast<uint32_t>(m_visibleCommands.size()); }

        /** @brief Get culled count this frame. */
        uint32_t GetCulledCount() const { return GetSubmittedCount() - GetVisibleCount(); }

        // ====================================================================
        // Asset path resolution
        // ====================================================================

        // ====================================================================
        // Portal culling
        // ====================================================================

        /** @brief Access the portal culling system for graph construction. */
        PortalCullingSystem& GetPortalCulling() { return m_portalCulling; }

        /** @brief Access the portal culling system (const). */
        const PortalCullingSystem& GetPortalCulling() const { return m_portalCulling; }

        // ====================================================================
        // Asset path resolution
        // ====================================================================

        /**
         * @brief Resolve an AssetHandle back to its string path.
         *
         * Used by the asset pipeline to look up resources. Returns empty
         * string if the handle has not been registered.
         */
        const std::string& ResolvePath(AssetHandle handle) const;

        /**
         * @brief Register a handle→path mapping for asset resolution.
         */
        void RegisterAssetPath(AssetHandle handle, const std::string& path);

      private:
        std::vector<DrawCommand> m_drawCommands;                 ///< All submitted commands this frame
        std::vector<DrawCommand> m_visibleCommands;              ///< Visible after culling
        Spark::FrameAllocator m_frameAllocator{4 * 1024 * 1024}; ///< 4 MB per-frame allocator

        std::unordered_map<AssetHandle, std::string> m_pathLookup; ///< Handle→path resolution

        BVHAccelerator m_bvh;                ///< Hierarchical culling accelerator
        PortalCullingSystem m_portalCulling; ///< Portal-based visibility for indoor scenes
        uint32_t m_maxDrawCommands = 8192;

        static const std::string s_emptyString; ///< Returned for unknown handles
    };

} // namespace Spark::Graphics
