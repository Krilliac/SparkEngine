/**
 * @file DirtyRectTracker.h
 * @brief Dirty rectangle tracking for partial texture updates
 *
 * Tracks modified sub-regions of textures so only dirty rectangles are
 * re-uploaded, avoiding full texture updates for localized changes.
 *
 * @note **Intentional reusable utility** — pure CPU rectangle-merge
 *       logic with no GPU dependency. There is no singleton to wire
 *       into the engine lifecycle; any future texture-streaming or
 *       UI-atlas system that wants partial-update semantics will
 *       instantiate one of these per tracked texture. Exercised by
 *       `Tests/TestDirtyRectTracker.cpp`.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark
{
    namespace Graphics
    {

        /**
         * @brief Axis-aligned rectangle defining a modified texture region.
         */
        struct DirtyRect
        {
            uint32_t x = 0;
            uint32_t y = 0;
            uint32_t width = 0;
            uint32_t height = 0;

            bool Overlaps(const DirtyRect& other) const
            {
                return x < other.x + other.width && x + width > other.x && y < other.y + other.height &&
                       y + height > other.y;
            }

            bool Contains(const DirtyRect& other) const
            {
                return x <= other.x && y <= other.y && x + width >= other.x + other.width &&
                       y + height >= other.y + other.height;
            }

            uint32_t Area() const { return width * height; }

            /**
             * @brief Merge two rectangles into their bounding box.
             */
            DirtyRect Union(const DirtyRect& other) const
            {
                uint32_t minX = std::min(x, other.x);
                uint32_t minY = std::min(y, other.y);
                uint32_t maxX = std::max(x + width, other.x + other.width);
                uint32_t maxY = std::max(y + height, other.y + other.height);
                return {minX, minY, maxX - minX, maxY - minY};
            }
        };

        /**
         * @brief Manages a list of dirty rectangles for a texture.
         *
         * Merges overlapping/adjacent rectangles to reduce the number
         * of partial upload calls. Supports coalescing when the dirty
         * region list grows too large.
         */
        class DirtyRectTracker
        {
          public:
            static constexpr uint32_t MAX_RECTS = 16;

            /**
             * @brief Mark a rectangular region as dirty.
             *
             * Merges with existing overlapping rects. If the list exceeds
             * MAX_RECTS, coalesces all rects into a single bounding box.
             */
            void AddDirtyRect(const DirtyRect& rect)
            {
                if (rect.width == 0 || rect.height == 0)
                    return;

                // Check if already contained
                for (const auto& existing : m_rects)
                {
                    if (existing.Contains(rect))
                        return;
                }

                // Merge with overlapping rects
                for (auto& existing : m_rects)
                {
                    if (existing.Overlaps(rect))
                    {
                        existing = existing.Union(rect);
                        m_totalDirtyArea = CalculateTotalArea();
                        return;
                    }
                }

                m_rects.push_back(rect);
                m_totalDirtyArea += rect.Area();

                // Coalesce if too many rects
                if (m_rects.size() > MAX_RECTS)
                    Coalesce();
            }

            /**
             * @brief Invalidate the entire texture (full update needed).
             */
            void InvalidateAll(uint32_t textureWidth, uint32_t textureHeight)
            {
                m_rects.clear();
                m_rects.push_back({0, 0, textureWidth, textureHeight});
                m_totalDirtyArea = textureWidth * textureHeight;
            }

            /**
             * @brief Clear all dirty regions (texture is now clean).
             */
            void Clear()
            {
                m_rects.clear();
                m_totalDirtyArea = 0;
            }

            bool IsDirty() const { return !m_rects.empty(); }
            const std::vector<DirtyRect>& GetDirtyRects() const { return m_rects; }
            size_t GetRectCount() const { return m_rects.size(); }
            uint64_t GetTotalDirtyArea() const { return m_totalDirtyArea; }

            /**
             * @brief Check if a full update would be cheaper than partial updates.
             */
            bool ShouldFullUpdate(uint32_t textureWidth, uint32_t textureHeight) const
            {
                uint64_t fullArea = static_cast<uint64_t>(textureWidth) * textureHeight;
                return m_totalDirtyArea > fullArea / 2 || m_rects.size() > MAX_RECTS / 2;
            }

          private:
            void Coalesce()
            {
                if (m_rects.empty())
                    return;
                DirtyRect bounds = m_rects[0];
                for (size_t i = 1; i < m_rects.size(); ++i)
                    bounds = bounds.Union(m_rects[i]);
                m_rects.clear();
                m_rects.push_back(bounds);
                m_totalDirtyArea = bounds.Area();
            }

            uint64_t CalculateTotalArea() const
            {
                uint64_t total = 0;
                for (const auto& r : m_rects)
                    total += r.Area();
                return total;
            }

            std::vector<DirtyRect> m_rects;
            uint64_t m_totalDirtyArea = 0;
        };

    } // namespace Graphics
} // namespace Spark
