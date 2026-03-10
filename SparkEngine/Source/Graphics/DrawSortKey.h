/**
 * @file DrawSortKey.h
 * @brief 64-bit composite sort key for draw call batching and radix sort
 * @author Spark Engine Team
 * @date 2026
 *
 * Encodes
 * pipeline state, material, and depth into a single 64-bit key so that
 * draw calls can be sorted with a cache-friendly radix sort, minimizing
 * GPU state changes per frame.
 *
 * ## Key Layout (64 bits)
 * ```
 *   [63..56]  8 bits  - Render layer (opaque=0, alpha-test=1, transparent=2, overlay=3)
 *   [55..40] 16 bits  - Shader/pipeline hash (groups by shader program)
 *   [39..24] 16 bits  - Material hash (groups by material within same shader)
 *   [23..0 ] 24 bits  - Depth key (front-to-back for opaque, back-to-front for transparent)
 * ```
 *
 * ## Usage
 * @code
 *   std::vector<DrawSortEntry> drawList;
 *
 *   for (auto& cmd : meshCommands)
 *   {
 *       DrawSortEntry entry;
 *       entry.key = DrawSortKey::Build(
 *           RenderLayer::Opaque,
 *           cmd.shaderHash,
 *           cmd.materialHash,
 *           cmd.viewSpaceDepth,
 *           camera.nearPlane,
 *           camera.farPlane
 *       );
 *       entry.drawIndex = cmd.index;
 *       drawList.push_back(entry);
 *   }
 *
 *   DrawSortKey::RadixSort(drawList);
 *   // Draw calls are now ordered to minimize state changes
 * @endcode
 *
 * @see GraphicsEngine::ProcessDrawList, MaterialSystem
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Render layer determines major sort bucket and depth direction.
     */
    enum class RenderLayer : uint8_t
    {
        Opaque = 0,      ///< Front-to-back for early-Z efficiency
        AlphaTest = 1,   ///< Front-to-back, alpha-tested geometry
        Transparent = 2, ///< Back-to-front for correct blending
        Overlay = 3,     ///< UI / debug overlays, rendered last
        PostProcess = 4, ///< Full-screen post-processing passes
        Shadow = 5,      ///< Shadow map rendering passes
        MaxLayers = 8
    };

    /**
     * @brief A draw list entry pairing a sort key with an index into the draw command array.
     */
    struct DrawSortEntry
    {
        uint64_t key = 0;
        uint32_t drawIndex = 0;
    };

    /**
     * @brief Utilities for building and sorting 64-bit draw call sort keys.
     */
    class DrawSortKey
    {
      public:
        // Bit field layout
        static constexpr int DEPTH_BITS = 24;
        static constexpr int MATERIAL_BITS = 16;
        static constexpr int SHADER_BITS = 16;
        static constexpr int LAYER_BITS = 8;

        static constexpr int DEPTH_SHIFT = 0;
        static constexpr int MATERIAL_SHIFT = DEPTH_BITS;
        static constexpr int SHADER_SHIFT = DEPTH_BITS + MATERIAL_BITS;
        static constexpr int LAYER_SHIFT = DEPTH_BITS + MATERIAL_BITS + SHADER_BITS;

        static constexpr uint64_t DEPTH_MASK = (1ULL << DEPTH_BITS) - 1;
        static constexpr uint64_t MATERIAL_MASK = (1ULL << MATERIAL_BITS) - 1;
        static constexpr uint64_t SHADER_MASK = (1ULL << SHADER_BITS) - 1;
        static constexpr uint64_t LAYER_MASK = (1ULL << LAYER_BITS) - 1;

        /**
         * @brief Build a 64-bit sort key from components.
         *
         * @param layer         Render layer (determines sort bucket and depth direction)
         * @param shaderHash    Hash of the shader/pipeline state (truncated to 16 bits)
         * @param materialHash  Hash of the material (truncated to 16 bits)
         * @param viewDepth     View-space depth of the object
         * @param nearPlane     Camera near plane distance
         * @param farPlane      Camera far plane distance
         * @return Composite 64-bit sort key
         */
        static uint64_t Build(RenderLayer layer, uint32_t shaderHash, uint32_t materialHash, float viewDepth,
                              float nearPlane = 0.1f, float farPlane = 1000.0f)
        {
            uint64_t layerBits = static_cast<uint64_t>(layer) & LAYER_MASK;
            uint64_t shaderBits = static_cast<uint64_t>(shaderHash) & SHADER_MASK;
            uint64_t matBits = static_cast<uint64_t>(materialHash) & MATERIAL_MASK;

            // Normalize depth to [0, 1] and quantize to 24 bits
            float t = (viewDepth - nearPlane) / (farPlane - nearPlane);
            t = std::clamp(t, 0.0f, 1.0f);
            uint32_t depthQuantized = static_cast<uint32_t>(t * static_cast<float>(DEPTH_MASK));

            // For transparent layers, reverse depth for back-to-front sorting
            bool isTransparent = (layer == RenderLayer::Transparent);
            if (isTransparent)
            {
                depthQuantized = static_cast<uint32_t>(DEPTH_MASK) - depthQuantized;
            }

            uint64_t depthBits = static_cast<uint64_t>(depthQuantized) & DEPTH_MASK;

            return (layerBits << LAYER_SHIFT) | (shaderBits << SHADER_SHIFT) | (matBits << MATERIAL_SHIFT) |
                   (depthBits << DEPTH_SHIFT);
        }

        /**
         * @brief Build a sort key for opaque geometry with default near/far planes.
         */
        static uint64_t BuildOpaque(uint32_t shaderHash, uint32_t materialHash, float viewDepth)
        {
            return Build(RenderLayer::Opaque, shaderHash, materialHash, viewDepth);
        }

        /**
         * @brief Build a sort key for transparent geometry.
         */
        static uint64_t BuildTransparent(uint32_t shaderHash, uint32_t materialHash, float viewDepth)
        {
            return Build(RenderLayer::Transparent, shaderHash, materialHash, viewDepth);
        }

        // =====================================================================
        // Key extraction
        // =====================================================================

        static RenderLayer ExtractLayer(uint64_t key)
        {
            return static_cast<RenderLayer>((key >> LAYER_SHIFT) & LAYER_MASK);
        }

        static uint16_t ExtractShaderHash(uint64_t key)
        {
            return static_cast<uint16_t>((key >> SHADER_SHIFT) & SHADER_MASK);
        }

        static uint16_t ExtractMaterialHash(uint64_t key)
        {
            return static_cast<uint16_t>((key >> MATERIAL_SHIFT) & MATERIAL_MASK);
        }

        static uint32_t ExtractDepth(uint64_t key) { return static_cast<uint32_t>((key >> DEPTH_SHIFT) & DEPTH_MASK); }

        // =====================================================================
        // Hashing helpers
        // =====================================================================

        /**
         * @brief Fast string hash suitable for sort key embedding (FNV-1a, 32-bit).
         */
        static uint32_t HashString(const char* str)
        {
            uint32_t hash = 2166136261u;
            while (*str)
            {
                hash ^= static_cast<uint32_t>(*str++);
                hash *= 16777619u;
            }
            return hash;
        }

        /**
         * @brief Hash from a string object.
         */
        static uint32_t HashString(const std::string& str) { return HashString(str.c_str()); }

        /**
         * @brief Combine two hashes (for shader + variant combos).
         */
        static uint32_t CombineHash(uint32_t a, uint32_t b) { return a ^ (b * 2654435761u); }

        // =====================================================================
        // Radix sort (8-pass, LSB-first, O(n) stable sort)
        // =====================================================================

        /**
         * @brief Sort draw entries by key using radix sort.
         *
         * Radix sort is O(n) and cache-friendly for large draw lists,
         * outperforming std::sort for >256 entries. Falls back to std::sort
         * for small arrays.
         *
         * @param entries Draw list entries to sort in-place.
         */
        static void RadixSort(std::vector<DrawSortEntry>& entries)
        {
            if (entries.size() <= 64)
            {
                // std::sort is faster for very small arrays
                std::sort(entries.begin(), entries.end(),
                          [](const DrawSortEntry& a, const DrawSortEntry& b) { return a.key < b.key; });
                return;
            }

            static constexpr int RADIX_BITS = 8;
            static constexpr int RADIX_SIZE = 1 << RADIX_BITS;
            static constexpr uint64_t RADIX_MASK = RADIX_SIZE - 1;
            static constexpr int NUM_PASSES = sizeof(uint64_t); // 8 passes

            std::vector<DrawSortEntry> temp(entries.size());
            std::array<uint32_t, RADIX_SIZE> histogram{};

            DrawSortEntry* src = entries.data();
            DrawSortEntry* dst = temp.data();
            const size_t count = entries.size();

            for (int pass = 0; pass < NUM_PASSES; ++pass)
            {
                const int shift = pass * RADIX_BITS;

                // Build histogram
                histogram.fill(0);
                for (size_t i = 0; i < count; ++i)
                {
                    uint32_t bucket = static_cast<uint32_t>((src[i].key >> shift) & RADIX_MASK);
                    ++histogram[bucket];
                }

                // Check if this pass is a no-op (all same bucket)
                bool skip = false;
                for (int b = 0; b < RADIX_SIZE; ++b)
                {
                    if (histogram[b] == static_cast<uint32_t>(count))
                    {
                        skip = true;
                        break;
                    }
                }
                if (skip)
                    continue;

                // Prefix sum
                uint32_t sum = 0;
                for (int b = 0; b < RADIX_SIZE; ++b)
                {
                    uint32_t prev = histogram[b];
                    histogram[b] = sum;
                    sum += prev;
                }

                // Scatter
                for (size_t i = 0; i < count; ++i)
                {
                    uint32_t bucket = static_cast<uint32_t>((src[i].key >> shift) & RADIX_MASK);
                    dst[histogram[bucket]++] = src[i];
                }

                // Swap src and dst
                std::swap(src, dst);
            }

            // If the result ended up in temp, copy back
            if (src != entries.data())
            {
                std::memcpy(entries.data(), src, count * sizeof(DrawSortEntry));
            }
        }

        // =====================================================================
        // Statistics
        // =====================================================================

        /**
         * @brief Count the number of unique shader+material combinations in a sorted list.
         *
         * Useful for measuring how effective sorting is at reducing state changes.
         */
        static uint32_t CountStateSwitches(const std::vector<DrawSortEntry>& sortedEntries)
        {
            if (sortedEntries.empty())
                return 0;

            uint32_t switches = 0;
            uint64_t prevStateBits = sortedEntries[0].key & ~DEPTH_MASK;

            for (size_t i = 1; i < sortedEntries.size(); ++i)
            {
                uint64_t stateBits = sortedEntries[i].key & ~DEPTH_MASK;
                if (stateBits != prevStateBits)
                {
                    ++switches;
                    prevStateBits = stateBits;
                }
            }

            return switches;
        }
    };

} // namespace Spark::Graphics
