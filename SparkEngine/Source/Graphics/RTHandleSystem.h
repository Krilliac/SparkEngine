/**
 * @file RTHandleSystem.h
 * @brief Scale-based render texture allocation with automatic resolution adaptation
 * @author Spark Engine Team
 * @date 2025
 *
 * Render textures are declared as fractions of a reference resolution rather
 * than fixed pixel sizes. When the window resizes or dynamic resolution changes,
 * all RTHandles automatically adapt without reallocation.
 *
 * Inspired by Unity's RTHandleSystem.
 *
 * @see RenderTargetPool.h, RenderGraph.h
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // RT Handle
    // =========================================================================

    /**
     * @brief Handle to a scale-based render texture
     *
     * The actual texture size is computed as: referenceSize * scaleFactor.
     * Textures only reallocate when the reference size grows beyond the
     * maximum historical size (textures never shrink, only grow).
     */
    struct RTHandle
    {
        uint32_t id = 0;
        float scaleX = 1.0f;  ///< Width scale relative to reference [0,1]
        float scaleY = 1.0f;  ///< Height scale relative to reference [0,1]
        uint32_t format = 28; ///< DXGI_FORMAT (default: R8G8B8A8_UNORM)
        bool isDepth = false;

        // Actual allocated size (may be larger than current reference * scale)
        uint32_t allocatedWidth = 0;
        uint32_t allocatedHeight = 0;

        // Current usable size (reference * scale, clamped to allocated)
        uint32_t currentWidth = 0;
        uint32_t currentHeight = 0;

        /** @brief Get UV scale factor to map full-screen UVs to the usable portion */
        float GetUVScaleX() const
        {
            return allocatedWidth > 0 ? static_cast<float>(currentWidth) / allocatedWidth : 1.0f;
        }
        float GetUVScaleY() const
        {
            return allocatedHeight > 0 ? static_cast<float>(currentHeight) / allocatedHeight : 1.0f;
        }
    };

    // =========================================================================
    // RT Handle System
    // =========================================================================

    /**
     * @brief Manages scale-based render texture allocation
     *
     * Usage:
     * 1. SetReferenceSize(screenWidth, screenHeight) at startup and on resize
     * 2. Allocate(scaleX, scaleY, format) to create handles
     * 3. Before rendering, call ResolveHandles() to update current sizes
     * 4. Use handle.currentWidth/Height for viewport, handle.GetUVScale*() for UVs
     */
    class RTHandleSystem
    {
      public:
        RTHandleSystem() = default;
        ~RTHandleSystem() = default;

        void Initialize(uint32_t referenceWidth, uint32_t referenceHeight)
        {
            m_referenceWidth = referenceWidth;
            m_referenceHeight = referenceHeight;
            m_maxHistoryWidth = referenceWidth;
            m_maxHistoryHeight = referenceHeight;
            m_handles.clear();
            m_nextId = 1;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_handles.clear();
            m_initialized = false;
        }

        /**
         * @brief Update the reference resolution (e.g., on window resize)
         *
         * Textures grow to accommodate the new size but never shrink.
         * This prevents reallocation when the window is made temporarily smaller.
         */
        void SetReferenceSize(uint32_t width, uint32_t height)
        {
            m_referenceWidth = width;
            m_referenceHeight = height;

            bool needsRealloc = (width > m_maxHistoryWidth || height > m_maxHistoryHeight);
            m_maxHistoryWidth = std::max(m_maxHistoryWidth, width);
            m_maxHistoryHeight = std::max(m_maxHistoryHeight, height);

            // Update all handles
            for (auto& [id, handle] : m_handles)
            {
                handle.currentWidth = static_cast<uint32_t>(width * handle.scaleX);
                handle.currentHeight = static_cast<uint32_t>(height * handle.scaleY);
                handle.currentWidth = std::max(handle.currentWidth, 1u);
                handle.currentHeight = std::max(handle.currentHeight, 1u);

                if (needsRealloc)
                {
                    uint32_t newAllocW = static_cast<uint32_t>(m_maxHistoryWidth * handle.scaleX);
                    uint32_t newAllocH = static_cast<uint32_t>(m_maxHistoryHeight * handle.scaleY);
                    handle.allocatedWidth = std::max(handle.allocatedWidth, std::max(newAllocW, 1u));
                    handle.allocatedHeight = std::max(handle.allocatedHeight, std::max(newAllocH, 1u));
                }
            }
        }

        /**
         * @brief Set dynamic resolution scale factor
         *
         * Values < 1.0 render at lower resolution (performance mode).
         * The texture allocation stays at max historical size; only the
         * usable viewport changes.
         */
        void SetDynamicResolutionScale(float scale)
        {
            scale = std::clamp(scale, 0.25f, 1.0f);
            m_dynamicScale = scale;

            for (auto& [id, handle] : m_handles)
            {
                handle.currentWidth = static_cast<uint32_t>(m_referenceWidth * handle.scaleX * m_dynamicScale);
                handle.currentHeight = static_cast<uint32_t>(m_referenceHeight * handle.scaleY * m_dynamicScale);
                handle.currentWidth = std::max(handle.currentWidth, 1u);
                handle.currentHeight = std::max(handle.currentHeight, 1u);
            }
        }

        /**
         * @brief Allocate a new render texture handle
         *
         * @param scaleX Width scale relative to reference [0,1]
         * @param scaleY Height scale relative to reference [0,1]
         * @param format DXGI_FORMAT for the texture
         * @param isDepth True for depth textures
         * @return Handle ID (0 on failure)
         */
        uint32_t Allocate(float scaleX, float scaleY, uint32_t format = 28, bool isDepth = false)
        {
            if (!m_initialized)
                return 0;

            RTHandle handle;
            handle.id = m_nextId++;
            handle.scaleX = scaleX;
            handle.scaleY = scaleY;
            handle.format = format;
            handle.isDepth = isDepth;

            uint32_t allocW = static_cast<uint32_t>(m_maxHistoryWidth * scaleX);
            uint32_t allocH = static_cast<uint32_t>(m_maxHistoryHeight * scaleY);
            handle.allocatedWidth = std::max(allocW, 1u);
            handle.allocatedHeight = std::max(allocH, 1u);
            handle.currentWidth = std::max(static_cast<uint32_t>(m_referenceWidth * scaleX * m_dynamicScale), 1u);
            handle.currentHeight = std::max(static_cast<uint32_t>(m_referenceHeight * scaleY * m_dynamicScale), 1u);

            m_handles[handle.id] = handle;
            return handle.id;
        }

        /** @brief Release a render texture handle */
        void Release(uint32_t handleId) { m_handles.erase(handleId); }

        /** @brief Get a handle by ID */
        const RTHandle* GetHandle(uint32_t id) const
        {
            auto it = m_handles.find(id);
            return it != m_handles.end() ? &it->second : nullptr;
        }

        RTHandle* GetHandle(uint32_t id)
        {
            auto it = m_handles.find(id);
            return it != m_handles.end() ? &it->second : nullptr;
        }

        uint32_t GetReferenceWidth() const { return m_referenceWidth; }
        uint32_t GetReferenceHeight() const { return m_referenceHeight; }
        float GetDynamicScale() const { return m_dynamicScale; }
        size_t GetHandleCount() const { return m_handles.size(); }
        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            return "RTHandleSystem: ref=" + std::to_string(m_referenceWidth) + "x" + std::to_string(m_referenceHeight) +
                   " max=" + std::to_string(m_maxHistoryWidth) + "x" + std::to_string(m_maxHistoryHeight) +
                   " dynScale=" + std::to_string(m_dynamicScale) + " handles=" + std::to_string(m_handles.size()) +
                   "\n";
        }

      private:
        std::unordered_map<uint32_t, RTHandle> m_handles;
        uint32_t m_referenceWidth = 0;
        uint32_t m_referenceHeight = 0;
        uint32_t m_maxHistoryWidth = 0;
        uint32_t m_maxHistoryHeight = 0;
        float m_dynamicScale = 1.0f;
        uint32_t m_nextId = 1;
        bool m_initialized = false;
    };

    // =========================================================================
    // Buffered RT Handle (double/triple buffered for history)
    // =========================================================================

    /**
     * @brief Double-buffered RTHandle for temporal effects (TAA, motion blur)
     *
     * Provides Current() and Previous() accessors that auto-swap each frame.
     */
    class BufferedRTHandle
    {
      public:
        void Initialize(RTHandleSystem& system, float scaleX, float scaleY, uint32_t format = 28)
        {
            m_handleIds[0] = system.Allocate(scaleX, scaleY, format);
            m_handleIds[1] = system.Allocate(scaleX, scaleY, format);
            m_system = &system;
            m_currentIndex = 0;
        }

        void Swap() { m_currentIndex = 1 - m_currentIndex; }

        const RTHandle* Current() const
        {
            return m_system ? m_system->GetHandle(m_handleIds[m_currentIndex]) : nullptr;
        }
        const RTHandle* Previous() const
        {
            return m_system ? m_system->GetHandle(m_handleIds[1 - m_currentIndex]) : nullptr;
        }

      private:
        RTHandleSystem* m_system = nullptr;
        uint32_t m_handleIds[2] = {};
        int m_currentIndex = 0;
    };

} // namespace Spark::Graphics
