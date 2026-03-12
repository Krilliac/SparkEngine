/**
 * @file SplatmapSystem.h
 * @brief Terrain splatmap generation and multi-layer texture blending
 *
 * Generates splatmaps (per-texel blend weight maps) for terrain rendering
 * based on height, slope, noise, and manual painting. Each splatmap channel
 * controls the blend weight of a different terrain material layer.
 *
 * ## Usage
 * @code
 *   SplatmapGenerator gen;
 *   gen.SetResolution(512, 512);
 *   gen.AddLayer({"Grass",  0.0f, 0.4f,  0.0f, 30.0f});  // Low flat areas
 *   gen.AddLayer({"Rock",   0.3f, 0.8f, 25.0f, 90.0f});   // Steep mid-height
 *   gen.AddLayer({"Snow",   0.7f, 1.0f,  0.0f, 45.0f});   // High altitude
 *   gen.AddLayer({"Sand",   0.0f, 0.1f,  0.0f, 15.0f});   // Very low, flat
 *
 *   Splatmap splatmap = gen.Generate(heightmapData, width, height);
 *   // splatmap.GetWeight(x, y, layerIndex) → blend weight [0, 1]
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Procedural
{

    // ============================================================================
    // Splatmap Layer Definition
    // ============================================================================

    /**
 * @brief Defines how a terrain material layer is distributed
 */
    struct SplatmapLayer
    {
        std::string name;           ///< Display name (e.g., "Grass", "Rock")
        std::string texturePath;    ///< Path to albedo texture
        std::string normalMapPath;  ///< Path to normal map
        float textureScale = 10.0f; ///< UV tiling scale

        // Height-based distribution
        float minHeight = 0.0f; ///< Normalized height [0, 1] — layer starts appearing
        float maxHeight = 1.0f; ///< Normalized height [0, 1] — layer stops appearing

        // Slope-based distribution (degrees)
        float minSlope = 0.0f;  ///< Minimum slope angle for this layer
        float maxSlope = 90.0f; ///< Maximum slope angle for this layer

        // Noise modulation
        float noiseScale = 0.0f;     ///< If > 0, adds Perlin noise to blend edges
        float noiseAmplitude = 0.0f; ///< Strength of noise modulation

        // Priority (higher = painted on top)
        int priority = 0;
    };

    // ============================================================================
    // Splatmap Data
    // ============================================================================

    /**
 * @brief Per-texel blend weights for up to N layers
 */
    class Splatmap
    {
      public:
        static constexpr int MaxLayers = 8;

        Splatmap() = default;
        Splatmap(uint32_t width, uint32_t height, int layerCount)
            : m_width(width), m_height(height), m_layerCount(std::min(layerCount, MaxLayers))
        {
            m_weights.resize(static_cast<size_t>(m_width) * m_height * m_layerCount, 0.0f);
        }

        /**
     * @brief Get blend weight for a specific texel and layer
     */
        float GetWeight(uint32_t x, uint32_t y, int layer) const
        {
            if (x >= m_width || y >= m_height || layer < 0 || layer >= m_layerCount)
                return 0.0f;
            return m_weights[TexelIndex(x, y, layer)];
        }

        /**
     * @brief Set blend weight for a specific texel and layer
     */
        void SetWeight(uint32_t x, uint32_t y, int layer, float weight)
        {
            if (x >= m_width || y >= m_height || layer < 0 || layer >= m_layerCount)
                return;
            m_weights[TexelIndex(x, y, layer)] = std::clamp(weight, 0.0f, 1.0f);
        }

        /**
     * @brief Normalize weights at each texel so they sum to 1.0
     */
        void Normalize()
        {
            for (uint32_t y = 0; y < m_height; ++y)
            {
                for (uint32_t x = 0; x < m_width; ++x)
                {
                    float sum = 0.0f;
                    for (int l = 0; l < m_layerCount; ++l)
                    {
                        sum += m_weights[TexelIndex(x, y, l)];
                    }
                    if (sum > 0.001f)
                    {
                        for (int l = 0; l < m_layerCount; ++l)
                        {
                            m_weights[TexelIndex(x, y, l)] /= sum;
                        }
                    }
                    else if (m_layerCount > 0)
                    {
                        // No weight at all — assign fully to first layer
                        m_weights[TexelIndex(x, y, 0)] = 1.0f;
                    }
                }
            }
        }

        /**
     * @brief Paint a circular brush at the given position
     * @param cx Center X in texel coordinates
     * @param cy Center Y in texel coordinates
     * @param radius Brush radius in texels
     * @param layer Layer to paint
     * @param strength Brush strength [0, 1]
     * @param falloff Brush falloff — 0 = hard edge, 1 = smooth
     */
        void PaintBrush(float cx, float cy, float radius, int layer, float strength, float falloff = 0.5f)
        {
            if (layer < 0 || layer >= m_layerCount || radius <= 0.0f)
                return;

            int minX = std::max(0, static_cast<int>(cx - radius));
            int maxX = std::min(static_cast<int>(m_width - 1), static_cast<int>(cx + radius));
            int minY = std::max(0, static_cast<int>(cy - radius));
            int maxY = std::min(static_cast<int>(m_height - 1), static_cast<int>(cy + radius));

            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    float dx = static_cast<float>(x) - cx;
                    float dy = static_cast<float>(y) - cy;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist > radius)
                        continue;

                    // Calculate falloff
                    float t = dist / radius;
                    float brushStrength = strength;
                    if (t > (1.0f - falloff))
                    {
                        float falloffT = (t - (1.0f - falloff)) / falloff;
                        brushStrength *= 1.0f - falloffT;
                    }

                    // Blend toward the target layer
                    float current = GetWeight(static_cast<uint32_t>(x), static_cast<uint32_t>(y), layer);
                    float newWeight = current + brushStrength * (1.0f - current);
                    SetWeight(static_cast<uint32_t>(x), static_cast<uint32_t>(y), layer, newWeight);
                }
            }

            // Re-normalize the affected area
            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    NormalizeTexel(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                }
            }
        }

        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        int GetLayerCount() const { return m_layerCount; }
        const std::vector<float>& GetRawWeights() const { return m_weights; }

      private:
        size_t TexelIndex(uint32_t x, uint32_t y, int layer) const
        {
            return (static_cast<size_t>(y) * m_width + x) * m_layerCount + layer;
        }

        void NormalizeTexel(uint32_t x, uint32_t y)
        {
            float sum = 0.0f;
            for (int l = 0; l < m_layerCount; ++l)
            {
                sum += m_weights[TexelIndex(x, y, l)];
            }
            if (sum > 0.001f)
            {
                for (int l = 0; l < m_layerCount; ++l)
                {
                    m_weights[TexelIndex(x, y, l)] /= sum;
                }
            }
        }

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        int m_layerCount = 0;
        std::vector<float> m_weights; ///< Interleaved: [y][x][layer]
    };

    // ============================================================================
    // Splatmap Generator
    // ============================================================================

    /**
 * @brief Generates splatmaps from heightmap data using layer rules
 */
    class SplatmapGenerator
    {
      public:
        void SetResolution(uint32_t width, uint32_t height)
        {
            m_width = width;
            m_height = height;
        }

        void AddLayer(const SplatmapLayer& layer)
        {
            if (m_layers.size() < Splatmap::MaxLayers)
            {
                m_layers.push_back(layer);
            }
        }

        void ClearLayers() { m_layers.clear(); }

        /**
     * @brief Generate a splatmap from heightmap data
     * @param heightmap  Normalized height values [0, 1]
     * @param hmWidth    Heightmap width
     * @param hmHeight   Heightmap height
     * @return Generated splatmap
     */
        Splatmap Generate(const float* heightmap, uint32_t hmWidth, uint32_t hmHeight) const
        {
            if (!heightmap || hmWidth == 0 || hmHeight == 0 || m_layers.empty())
            {
                return Splatmap{};
            }

            uint32_t outW = m_width > 0 ? m_width : hmWidth;
            uint32_t outH = m_height > 0 ? m_height : hmHeight;
            int layerCount = static_cast<int>(m_layers.size());

            Splatmap result(outW, outH, layerCount);

            for (uint32_t y = 0; y < outH; ++y)
            {
                for (uint32_t x = 0; x < outW; ++x)
                {
                    // Sample heightmap (bilinear if resolutions differ)
                    float u = static_cast<float>(x) / static_cast<float>(outW - 1);
                    float v = static_cast<float>(y) / static_cast<float>(outH - 1);
                    float height = SampleHeightmap(heightmap, hmWidth, hmHeight, u, v);
                    float slope = CalculateSlope(heightmap, hmWidth, hmHeight, u, v);

                    // Evaluate each layer
                    for (int l = 0; l < layerCount; ++l)
                    {
                        float weight = EvaluateLayer(m_layers[static_cast<size_t>(l)], height, slope, u, v);
                        result.SetWeight(x, y, l, weight);
                    }
                }
            }

            result.Normalize();
            return result;
        }

        const std::vector<SplatmapLayer>& GetLayers() const { return m_layers; }

      private:
        float SampleHeightmap(const float* hm, uint32_t w, uint32_t h, float u, float v) const
        {
            float fx = u * static_cast<float>(w - 1);
            float fy = v * static_cast<float>(h - 1);
            uint32_t ix = std::min(static_cast<uint32_t>(fx), w - 1);
            uint32_t iy = std::min(static_cast<uint32_t>(fy), h - 1);
            return hm[iy * w + ix];
        }

        float CalculateSlope(const float* hm, uint32_t w, uint32_t h, float u, float v) const
        {
            float fx = u * static_cast<float>(w - 1);
            float fy = v * static_cast<float>(h - 1);
            uint32_t ix = std::clamp(static_cast<uint32_t>(fx), 1u, w - 2);
            uint32_t iy = std::clamp(static_cast<uint32_t>(fy), 1u, h - 2);

            float left = hm[iy * w + (ix - 1)];
            float right = hm[iy * w + (ix + 1)];
            float up = hm[(iy - 1) * w + ix];
            float down = hm[(iy + 1) * w + ix];

            float dx = (right - left) * 0.5f;
            float dy = (down - up) * 0.5f;
            float gradient = std::sqrt(dx * dx + dy * dy);

            // Convert gradient to angle in degrees
            return std::atan(gradient * 10.0f) * (180.0f / 3.14159265f);
        }

        float EvaluateLayer(const SplatmapLayer& layer, float height, float slope, float u, float v) const
        {
            // Height factor: smooth ramp at edges
            float heightFactor = 0.0f;
            float heightRange = layer.maxHeight - layer.minHeight;
            if (heightRange > 0.001f)
            {
                float edgeWidth = heightRange * 0.15f; // 15% edge softness
                if (height >= layer.minHeight && height <= layer.maxHeight)
                {
                    heightFactor = 1.0f;
                    // Smooth lower edge
                    if (height < layer.minHeight + edgeWidth)
                    {
                        heightFactor = (height - layer.minHeight) / edgeWidth;
                    }
                    // Smooth upper edge
                    if (height > layer.maxHeight - edgeWidth)
                    {
                        heightFactor = (layer.maxHeight - height) / edgeWidth;
                    }
                }
            }

            // Slope factor: smooth ramp
            float slopeFactor = 0.0f;
            float slopeRange = layer.maxSlope - layer.minSlope;
            if (slopeRange > 0.001f)
            {
                float slopeEdge = slopeRange * 0.15f;
                if (slope >= layer.minSlope && slope <= layer.maxSlope)
                {
                    slopeFactor = 1.0f;
                    if (slope < layer.minSlope + slopeEdge)
                    {
                        slopeFactor = (slope - layer.minSlope) / slopeEdge;
                    }
                    if (slope > layer.maxSlope - slopeEdge)
                    {
                        slopeFactor = (layer.maxSlope - slope) / slopeEdge;
                    }
                }
            }

            float weight = heightFactor * slopeFactor;

            // Add noise modulation
            if (layer.noiseScale > 0.0f && layer.noiseAmplitude > 0.0f)
            {
                // Simple hash-based noise (deterministic, no external dependencies)
                float noise = SimpleNoise(u * layer.noiseScale, v * layer.noiseScale);
                weight += noise * layer.noiseAmplitude;
                weight = std::clamp(weight, 0.0f, 1.0f);
            }

            return weight;
        }

        static float SimpleNoise(float x, float y)
        {
            // Simple hash-based pseudo-noise for edge variation
            int ix = static_cast<int>(std::floor(x));
            int iy = static_cast<int>(std::floor(y));
            float fx = x - std::floor(x);
            float fy = y - std::floor(y);

            // Smooth interpolation
            fx = fx * fx * (3.0f - 2.0f * fx);
            fy = fy * fy * (3.0f - 2.0f * fy);

            float a = HashFloat(ix, iy);
            float b = HashFloat(ix + 1, iy);
            float c = HashFloat(ix, iy + 1);
            float d = HashFloat(ix + 1, iy + 1);

            float ab = a + fx * (b - a);
            float cd = c + fx * (d - c);
            return ab + fy * (cd - ab);
        }

        static float HashFloat(int x, int y)
        {
            int h = x * 374761393 + y * 668265263;
            h = (h ^ (h >> 13)) * 1274126177;
            h = h ^ (h >> 16);
            return (static_cast<float>(h & 0x7FFFFFFF) / static_cast<float>(0x7FFFFFFF));
        }

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        std::vector<SplatmapLayer> m_layers;
    };

    // ============================================================================
    // Foliage Placement
    // ============================================================================

    /**
 * @brief Configuration for procedural foliage placement on a splatmap layer
 */
    struct FoliageType
    {
        std::string name;              ///< Display name (e.g., "Tall Grass", "Pine Tree")
        std::string meshPath;          ///< Path to foliage mesh
        int splatmapLayer = 0;         ///< Which splatmap layer this foliage grows on
        float minWeight = 0.5f;        ///< Minimum splatmap weight to place foliage
        float density = 1.0f;          ///< Instances per unit area
        float minScale = 0.8f;         ///< Minimum random scale
        float maxScale = 1.2f;         ///< Maximum random scale
        float minSlopeDeg = 0.0f;      ///< Minimum slope for placement
        float maxSlopeDeg = 30.0f;     ///< Maximum slope for placement
        bool alignToSurface = true;    ///< Rotate to match terrain normal
        float randomRotation = 360.0f; ///< Random Y-axis rotation range (degrees)
    };

    /**
 * @brief Result of foliage placement — instance transform data
 */
    struct FoliageInstance
    {
        XMFLOAT3 position;
        XMFLOAT3 rotation; ///< Euler angles (degrees)
        float scale;
        int foliageTypeIndex;
    };

    /**
 * @brief Places foliage instances based on splatmap weights
 */
    class FoliagePlacer
    {
      public:
        void AddFoliageType(const FoliageType& type) { m_types.push_back(type); }

        /**
     * @brief Generate foliage instances for a terrain
     * @param splatmap       Splatmap for weight queries
     * @param heightmap      Terrain heights
     * @param hmWidth        Heightmap width
     * @param hmHeight       Heightmap height
     * @param terrainScale   World-space terrain dimensions
     * @param heightScale    Height multiplier
     * @return Vector of foliage instances to render
     */
        std::vector<FoliageInstance> GenerateInstances(const Splatmap& splatmap, const float* heightmap,
                                                       uint32_t hmWidth, uint32_t hmHeight, float terrainScale,
                                                       float heightScale) const
        {
            std::vector<FoliageInstance> instances;

            if (!heightmap || hmWidth == 0 || hmHeight == 0)
                return instances;

            for (int typeIdx = 0; typeIdx < static_cast<int>(m_types.size()); ++typeIdx)
            {
                const auto& type = m_types[static_cast<size_t>(typeIdx)];

                // Calculate step size based on density
                float step = type.density > 0.0f ? 1.0f / std::sqrt(type.density) : 2.0f;

                for (float fy = 0.0f; fy < static_cast<float>(hmHeight); fy += step)
                {
                    for (float fx = 0.0f; fx < static_cast<float>(hmWidth); fx += step)
                    {
                        // Sample splatmap weight
                        uint32_t sx = static_cast<uint32_t>((fx / static_cast<float>(hmWidth)) *
                                                            static_cast<float>(splatmap.GetWidth() - 1));
                        uint32_t sy = static_cast<uint32_t>((fy / static_cast<float>(hmHeight)) *
                                                            static_cast<float>(splatmap.GetHeight() - 1));
                        float weight = splatmap.GetWeight(sx, sy, type.splatmapLayer);

                        if (weight < type.minWeight)
                            continue;

                        // Jitter position using deterministic hash
                        float jx = fx + HashJitter(static_cast<int>(fx), static_cast<int>(fy), 0) * step * 0.8f;
                        float jy = fy + HashJitter(static_cast<int>(fx), static_cast<int>(fy), 1) * step * 0.8f;

                        // Probability based on weight
                        float prob = HashJitter(static_cast<int>(jx * 100), static_cast<int>(jy * 100), 2);
                        if (prob > weight)
                            continue;

                        // Sample height
                        uint32_t hx = std::min(static_cast<uint32_t>(jx), hmWidth - 1);
                        uint32_t hy = std::min(static_cast<uint32_t>(jy), hmHeight - 1);
                        float h = heightmap[hy * hmWidth + hx];

                        // Random scale
                        float scaleFactor =
                            type.minScale + HashJitter(static_cast<int>(jx * 50), static_cast<int>(jy * 50), 3) *
                                                (type.maxScale - type.minScale);

                        // Random rotation
                        float rotY =
                            HashJitter(static_cast<int>(jx * 75), static_cast<int>(jy * 75), 4) * type.randomRotation;

                        FoliageInstance inst;
                        inst.position = {jx / static_cast<float>(hmWidth) * terrainScale, h * heightScale,
                                         jy / static_cast<float>(hmHeight) * terrainScale};
                        inst.rotation = {0.0f, rotY, 0.0f};
                        inst.scale = scaleFactor;
                        inst.foliageTypeIndex = typeIdx;
                        instances.push_back(inst);
                    }
                }
            }

            return instances;
        }

        const std::vector<FoliageType>& GetFoliageTypes() const { return m_types; }

      private:
        static float HashJitter(int x, int y, int seed)
        {
            int h = x * 374761393 + y * 668265263 + seed * 1103515245;
            h = (h ^ (h >> 13)) * 1274126177;
            h = h ^ (h >> 16);
            return static_cast<float>(h & 0xFFFF) / 65535.0f;
        }

        std::vector<FoliageType> m_types;
    };

} // namespace Spark::Procedural
