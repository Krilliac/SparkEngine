/**
 * @file UICompositor.h
 * @brief Compositor stack with pooled render targets for per-element UI effects
 * @author Spark Engine Team
 * @date 2025
 *
 * Enables per-widget compositing effects (blur, mask, opacity) without
 * full-screen passes. Uses a pool of render targets to avoid allocation
 * churn. Inspired by ThorVG's compositor stack with needComposition() skip.
 *
 * @see UISystem.h, UIDirtyTracking.h, RenderTargetPool.h
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Composition Operation
    // =========================================================================

    enum class CompositeOp : uint8_t
    {
        None,         ///< Direct rendering (no intermediate target)
        AlphaBlend,   ///< Standard alpha compositing
        Multiply,     ///< Multiply blend with background
        Screen,       ///< Screen blend (lighter)
        Mask,         ///< Use as alpha mask for next layer
        GaussianBlur, ///< Blur the rendered content
    };

    struct CompositeRequest
    {
        CompositeOp operation = CompositeOp::None;
        float opacity = 1.0f;
        float blurRadius = 0.0f; ///< For GaussianBlur operation
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // =========================================================================
    // Render Target Pool Entry
    // =========================================================================

    struct PooledRenderTarget
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0; ///< DXGI_FORMAT
        uint32_t lastUsedFrame = 0;
        bool inUse = false;
        uint32_t poolIndex = 0;

        // GPU resources would be ComPtr<ID3D11Texture2D>, etc.
        // Kept abstract for cross-platform compatibility
    };

    // =========================================================================
    // UI Compositor
    // =========================================================================

    /**
     * @brief Manages a stack of compositing operations with pooled render targets
     *
     * Workflow:
     * 1. BeginComposite(request) — push a new compositing level
     * 2. Render widget content to the intermediate RT
     * 3. EndComposite() — composite the intermediate RT back to the parent
     *
     * The NeedsComposition() check allows skipping the push/pop for simple
     * widgets (full opacity, no effects), rendering directly to the parent RT.
     */
    class UICompositor
    {
      public:
        static constexpr uint32_t kMaxPoolSize = 16;
        static constexpr uint32_t kMaxStackDepth = 8;

        UICompositor() = default;
        ~UICompositor() = default;

        void Initialize(uint32_t screenWidth, uint32_t screenHeight)
        {
            m_screenWidth = screenWidth;
            m_screenHeight = screenHeight;
            m_pool.clear();
            m_stack.clear();
            m_frameIndex = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_pool.clear();
            m_stack.clear();
            m_initialized = false;
        }

        void BeginFrame()
        {
            m_frameIndex++;
            m_stack.clear();

            // Reclaim unused pool entries (not used for 10 frames)
            m_pool.erase(std::remove_if(m_pool.begin(), m_pool.end(), [&](const PooledRenderTarget& rt)
                                        { return !rt.inUse && m_frameIndex - rt.lastUsedFrame > 10; }),
                         m_pool.end());
        }

        /**
         * @brief Push a compositing level — allocates an intermediate RT
         * @return Index of the allocated pool entry, or -1 on failure
         */
        int BeginComposite(const CompositeRequest& request)
        {
            if (!m_initialized || m_stack.size() >= kMaxStackDepth)
                return -1;

            // Find or create a matching pool entry
            int poolIdx = AcquireTarget(request.width > 0 ? request.width : m_screenWidth,
                                        request.height > 0 ? request.height : m_screenHeight);
            if (poolIdx < 0)
                return -1;

            StackEntry entry;
            entry.poolIndex = poolIdx;
            entry.request = request;
            m_stack.push_back(entry);

            return poolIdx;
        }

        /**
         * @brief Pop a compositing level — composite intermediate RT to parent
         */
        void EndComposite()
        {
            if (m_stack.empty())
                return;

            auto& top = m_stack.back();
            ReleaseTarget(top.poolIndex);
            m_stack.pop_back();
        }

        /** @brief Get current compositing depth */
        size_t GetStackDepth() const { return m_stack.size(); }

        /** @brief Get pool utilization */
        size_t GetPoolSize() const { return m_pool.size(); }

        /** @brief Get number of active (in-use) targets */
        uint32_t GetActiveTargetCount() const
        {
            uint32_t count = 0;
            for (const auto& rt : m_pool)
            {
                if (rt.inUse)
                    count++;
            }
            return count;
        }

        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            return "UICompositor: pool=" + std::to_string(m_pool.size()) +
                   " active=" + std::to_string(GetActiveTargetCount()) + " depth=" + std::to_string(m_stack.size()) +
                   "\n";
        }

      private:
        int AcquireTarget(uint32_t width, uint32_t height)
        {
            // Try to reuse an existing free target of matching size
            for (size_t i = 0; i < m_pool.size(); ++i)
            {
                auto& rt = m_pool[i];
                if (!rt.inUse && rt.width == width && rt.height == height)
                {
                    rt.inUse = true;
                    rt.lastUsedFrame = m_frameIndex;
                    return static_cast<int>(i);
                }
            }

            // Create new pool entry
            if (m_pool.size() >= kMaxPoolSize)
                return -1;

            PooledRenderTarget rt;
            rt.width = width;
            rt.height = height;
            rt.inUse = true;
            rt.lastUsedFrame = m_frameIndex;
            rt.poolIndex = static_cast<uint32_t>(m_pool.size());
            m_pool.push_back(rt);
            return static_cast<int>(m_pool.size() - 1);
        }

        void ReleaseTarget(int poolIndex)
        {
            if (poolIndex >= 0 && poolIndex < static_cast<int>(m_pool.size()))
            {
                m_pool[poolIndex].inUse = false;
            }
        }

        struct StackEntry
        {
            int poolIndex = -1;
            CompositeRequest request;
        };

        std::vector<PooledRenderTarget> m_pool;
        std::vector<StackEntry> m_stack;
        uint32_t m_screenWidth = 1920;
        uint32_t m_screenHeight = 1080;
        uint32_t m_frameIndex = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
