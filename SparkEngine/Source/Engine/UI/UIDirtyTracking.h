/**
 * @file UIDirtyTracking.h
 * @brief Dirty region tracking for UI rendering optimization
 * @author Spark Engine Team
 * @date 2025
 *
 * Tracks which UI elements have changed and which screen regions need
 * re-rendering. Unchanged regions can reuse the previous frame's render
 * output, avoiding redundant GPU work.
 *
 * Inspired by ThorVG's 16x16 grid dirty region system with double-buffered
 * dirty lists and per-widget change flags.
 *
 * @see UISystem.h
 */

#pragma once

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::UI
{

    // =========================================================================
    // Widget Dirty Flags
    // =========================================================================

    /**
     * @brief Bitfield flags indicating what changed on a widget
     */
    enum class DirtyFlag : uint32_t
    {
        None = 0,
        Transform = 1 << 0,  ///< Position or size changed
        Color = 1 << 1,      ///< Color or opacity changed
        Visibility = 1 << 2, ///< Visible/hidden state changed
        Content = 1 << 3,    ///< Text, image, or value changed
        Children = 1 << 4,   ///< Child widgets added/removed
        Layout = 1 << 5,     ///< Layout recomputed
        All = 0x3F
    };

    inline DirtyFlag operator|(DirtyFlag a, DirtyFlag b)
    {
        return static_cast<DirtyFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline DirtyFlag operator&(DirtyFlag a, DirtyFlag b)
    {
        return static_cast<DirtyFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    inline DirtyFlag& operator|=(DirtyFlag& a, DirtyFlag b)
    {
        a = a | b;
        return a;
    }
    inline bool HasFlag(DirtyFlag flags, DirtyFlag test)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
    }

    // =========================================================================
    // Dirty Region
    // =========================================================================

    /**
     * @brief Axis-aligned rectangle in screen pixels
     */
    struct DirtyRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        bool IsEmpty() const { return width <= 0 || height <= 0; }

        bool Intersects(const DirtyRect& other) const
        {
            return !(x + width <= other.x || other.x + other.width <= x || y + height <= other.y ||
                     other.y + other.height <= y);
        }

        /** @brief Merge another rect into this one (union) */
        void Merge(const DirtyRect& other)
        {
            if (other.IsEmpty())
                return;
            if (IsEmpty())
            {
                *this = other;
                return;
            }
            float minX = std::min(x, other.x);
            float minY = std::min(y, other.y);
            float maxX = std::max(x + width, other.x + other.width);
            float maxY = std::max(y + height, other.y + other.height);
            x = minX;
            y = minY;
            width = maxX - minX;
            height = maxY - minY;
        }
    };

    // =========================================================================
    // Dirty Region Grid
    // =========================================================================

    /**
     * @brief Grid-based dirty tracking for efficient partial UI updates
     *
     * Divides the screen into a grid of cells (default 16x16). When a widget
     * is marked dirty, the cells it overlaps are flagged. On render, only
     * dirty cells are redrawn. Uses double-buffering so the previous frame's
     * dirty state is preserved for clearing.
     *
     * Workflow:
     * 1. BeginFrame() — swap dirty buffers
     * 2. MarkDirty(rect) — flag cells overlapping the rect
     * 3. GetDirtyRects() — get merged dirty regions for scissor rendering
     * 4. EndFrame() — clear current frame dirty state
     */
    class DirtyRegionGrid
    {
      public:
        static constexpr int kGridSize = 16; ///< 16x16 = 256 cells
        static constexpr int kMaxCells = kGridSize * kGridSize;

        DirtyRegionGrid() = default;
        ~DirtyRegionGrid() = default;

        void Initialize(uint32_t screenWidth, uint32_t screenHeight)
        {
            m_screenWidth = screenWidth;
            m_screenHeight = screenHeight;
            m_cellWidth = static_cast<float>(screenWidth) / kGridSize;
            m_cellHeight = static_cast<float>(screenHeight) / kGridSize;
            m_currentDirty.reset();
            m_previousDirty.reset();
            m_fullyDirty = true; // Force full redraw on first frame
            m_initialized = true;
        }

        void Resize(uint32_t width, uint32_t height)
        {
            m_screenWidth = width;
            m_screenHeight = height;
            m_cellWidth = static_cast<float>(width) / kGridSize;
            m_cellHeight = static_cast<float>(height) / kGridSize;
            MarkFullyDirty(); // Resize invalidates everything
        }

        /** @brief Swap dirty buffers for the new frame */
        void BeginFrame()
        {
            m_previousDirty = m_currentDirty;
            m_currentDirty.reset();
        }

        /**
         * @brief Mark screen region as dirty
         * @param rect Screen-space rectangle that needs re-rendering
         */
        void MarkDirty(const DirtyRect& rect)
        {
            if (!m_initialized || rect.IsEmpty())
                return;

            int x0 = std::max(0, static_cast<int>(rect.x / m_cellWidth));
            int y0 = std::max(0, static_cast<int>(rect.y / m_cellHeight));
            int x1 = std::min(kGridSize - 1, static_cast<int>((rect.x + rect.width) / m_cellWidth));
            int y1 = std::min(kGridSize - 1, static_cast<int>((rect.y + rect.height) / m_cellHeight));

            for (int gy = y0; gy <= y1; ++gy)
            {
                for (int gx = x0; gx <= x1; ++gx)
                {
                    m_currentDirty.set(gy * kGridSize + gx);
                }
            }
        }

        /**
         * @brief Mark a widget's bounds as dirty (both old and new positions)
         *
         * When a widget moves, both its previous position (to clear) and new
         * position (to draw) must be marked dirty.
         */
        void MarkWidgetDirty(float oldX, float oldY, float oldW, float oldH, float newX, float newY, float newW,
                             float newH)
        {
            MarkDirty({oldX, oldY, oldW, oldH});
            MarkDirty({newX, newY, newW, newH});
        }

        /** @brief Mark the entire screen as dirty (for first frame, resize, etc.) */
        void MarkFullyDirty()
        {
            m_currentDirty.set(); // All bits = 1
            m_fullyDirty = true;
        }

        /**
         * @brief Get merged dirty regions suitable for scissor rect rendering
         *
         * Merges adjacent dirty cells into larger rectangles to reduce
         * the number of scissor rects needed.
         */
        std::vector<DirtyRect> GetDirtyRects() const
        {
            // Combine current and previous frame dirty cells
            auto combinedDirty = m_currentDirty | m_previousDirty;

            if (combinedDirty.none())
                return {};

            // Merge adjacent dirty cells into rectangles (row-based scan)
            std::vector<DirtyRect> rects;
            std::bitset<kMaxCells> visited;

            for (int gy = 0; gy < kGridSize; ++gy)
            {
                for (int gx = 0; gx < kGridSize; ++gx)
                {
                    int idx = gy * kGridSize + gx;
                    if (!combinedDirty.test(idx) || visited.test(idx))
                        continue;

                    // Expand horizontally
                    int endX = gx;
                    while (endX + 1 < kGridSize && combinedDirty.test(gy * kGridSize + endX + 1) &&
                           !visited.test(gy * kGridSize + endX + 1))
                    {
                        endX++;
                    }

                    // Expand vertically
                    int endY = gy;
                    bool canExpand = true;
                    while (canExpand && endY + 1 < kGridSize)
                    {
                        for (int tx = gx; tx <= endX; ++tx)
                        {
                            int tidx = (endY + 1) * kGridSize + tx;
                            if (!combinedDirty.test(tidx) || visited.test(tidx))
                            {
                                canExpand = false;
                                break;
                            }
                        }
                        if (canExpand)
                            endY++;
                    }

                    // Mark cells as visited
                    for (int ty = gy; ty <= endY; ++ty)
                        for (int tx = gx; tx <= endX; ++tx)
                            visited.set(ty * kGridSize + tx);

                    // Create rect
                    DirtyRect r;
                    r.x = gx * m_cellWidth;
                    r.y = gy * m_cellHeight;
                    r.width = (endX - gx + 1) * m_cellWidth;
                    r.height = (endY - gy + 1) * m_cellHeight;
                    rects.push_back(r);
                }
            }

            return rects;
        }

        /** @brief Check if any region is dirty */
        bool HasDirtyRegions() const { return m_currentDirty.any() || m_previousDirty.any(); }

        /** @brief Check if the entire screen was marked dirty */
        bool IsFullyDirty() const { return m_fullyDirty; }

        /** @brief Get dirty cell count */
        uint32_t GetDirtyCellCount() const { return static_cast<uint32_t>((m_currentDirty | m_previousDirty).count()); }

        void EndFrame() { m_fullyDirty = false; }

        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            uint32_t dirtyCells = GetDirtyCellCount();
            float dirtyPercent = static_cast<float>(dirtyCells) / kMaxCells * 100.0f;
            return "DirtyRegionGrid: " + std::to_string(dirtyCells) + "/" + std::to_string(kMaxCells) +
                   " cells dirty (" + std::to_string(static_cast<int>(dirtyPercent)) + "%)\n";
        }

      private:
        uint32_t m_screenWidth = 1920;
        uint32_t m_screenHeight = 1080;
        float m_cellWidth = 120.0f;
        float m_cellHeight = 67.5f;

        std::bitset<kMaxCells> m_currentDirty;
        std::bitset<kMaxCells> m_previousDirty;
        bool m_fullyDirty = true;
        bool m_initialized = false;
    };

} // namespace Spark::UI
