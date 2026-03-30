/**
 * @file TerrainData.cpp
 * @brief Implementation of terrain data structures (brush, heightmap, splatmap)
 */

#include "TerrainData.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using namespace DirectX;
namespace SparkEditor
{

    // --- TerrainBrush ---

    float TerrainBrush::EvaluateFalloff(float distance) const
    {
        float d = std::clamp(distance, 0.0f, 1.0f);
        switch (falloffType)
        {
        case LINEAR:
            return 1.0f - d;
        case SMOOTH:
            return 1.0f - (3.0f * d * d - 2.0f * d * d * d); // smoothstep
        case SPHERE:
            return std::sqrt(std::max(0.0f, 1.0f - d * d));
        case SHARP:
            return (1.0f - d) * (1.0f - d);
        case CUSTOM:
        default:
            return 1.0f - d;
        }
    }

    // --- TerrainHeightmap ---

    float TerrainHeightmap::GetHeight(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return 0.0f;
        if (heights.empty())
            return 0.0f;
        return heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }

    void TerrainHeightmap::SetHeight(int x, int y, float h)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;
        if (heights.empty())
            return;
        heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = h;
    }

    float TerrainHeightmap::GetHeightInterpolated(float worldX, float worldZ, float terrainSize) const
    {
        if (heights.empty() || terrainSize <= 0.0f)
            return 0.0f;

        float u = (worldX / terrainSize + 0.5f) * static_cast<float>(width - 1);
        float v = (worldZ / terrainSize + 0.5f) * static_cast<float>(height - 1);

        int x0 = static_cast<int>(std::floor(u));
        int y0 = static_cast<int>(std::floor(v));
        int x1 = std::min(x0 + 1, width - 1);
        int y1 = std::min(y0 + 1, height - 1);
        x0 = std::clamp(x0, 0, width - 1);
        y0 = std::clamp(y0, 0, height - 1);

        float fx = u - std::floor(u);
        float fy = v - std::floor(v);

        float h00 = GetHeight(x0, y0);
        float h10 = GetHeight(x1, y0);
        float h01 = GetHeight(x0, y1);
        float h11 = GetHeight(x1, y1);

        float top = h00 + (h10 - h00) * fx;
        float bot = h01 + (h11 - h01) * fx;
        return (top + (bot - top) * fy) * scale;
    }

    void TerrainHeightmap::Resize(int newWidth, int newHeight, bool preserveData)
    {
        if (newWidth <= 0 || newHeight <= 0)
            return;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Resizing heightmap to %dx%d (preserve=%s)", newWidth, newHeight,
                       preserveData ? "true" : "false");

        std::vector<float> newHeights(static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight), 0.0f);

        if (preserveData && !heights.empty())
        {
            int copyW = std::min(width, newWidth);
            int copyH = std::min(height, newHeight);
            for (int y = 0; y < copyH; ++y)
            {
                for (int x = 0; x < copyW; ++x)
                {
                    newHeights[static_cast<size_t>(y) * static_cast<size_t>(newWidth) + static_cast<size_t>(x)] =
                        heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                }
            }
        }

        width = newWidth;
        height = newHeight;
        heights = std::move(newHeights);
    }

    void TerrainHeightmap::Generate(std::function<float(int, int)> generator)
    {
        if (!generator)
            return;
        heights.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = generator(x, y);
            }
        }
    }

    bool TerrainHeightmap::LoadFromImage(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return false;

        auto fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0);

        size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

        bool is16Bit = (fileSize >= pixelCount * 2);
        bool is8Bit = (fileSize >= pixelCount);
        if (!is16Bit && !is8Bit)
            return false;

        heights.resize(pixelCount);
        if (is16Bit)
        {
            std::vector<uint16_t> raw(pixelCount);
            file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(pixelCount * 2));
            for (size_t i = 0; i < pixelCount; ++i)
                heights[i] = minHeight + (static_cast<float>(raw[i]) / 65535.0f) * (maxHeight - minHeight);
        }
        else
        {
            std::vector<uint8_t> raw(pixelCount);
            file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(pixelCount));
            for (size_t i = 0; i < pixelCount; ++i)
                heights[i] = minHeight + (static_cast<float>(raw[i]) / 255.0f) * (maxHeight - minHeight);
        }
        return file.good();
    }

    bool TerrainHeightmap::SaveToImage(const std::string& filePath) const
    {
        if (heights.empty())
            return false;

        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        float range = maxHeight - minHeight;
        if (range < 1e-6f)
            range = 1.0f;

        std::vector<uint16_t> raw(heights.size());
        for (size_t i = 0; i < heights.size(); ++i)
        {
            float normalized = std::clamp((heights[i] - minHeight) / range, 0.0f, 1.0f);
            raw[i] = static_cast<uint16_t>(normalized * 65535.0f);
        }
        file.write(reinterpret_cast<const char*>(raw.data()),
                   static_cast<std::streamsize>(raw.size() * sizeof(uint16_t)));
        return file.good();
    }

    // --- TerrainData ---

    TerrainTextureLayer* TerrainData::GetTextureLayer(int index)
    {
        if (index < 0 || index >= static_cast<int>(textureLayers.size()))
            return nullptr;
        return textureLayers[static_cast<size_t>(index)].get();
    }

    TerrainTextureLayer* TerrainData::AddTextureLayer(const std::string& name)
    {
        auto layer = std::make_unique<TerrainTextureLayer>();
        layer->name = name;
        auto* ptr = layer.get();
        textureLayers.push_back(std::move(layer));
        return ptr;
    }

    void TerrainData::RemoveTextureLayer(int index)
    {
        if (index < 0 || index >= static_cast<int>(textureLayers.size()))
            return;
        textureLayers.erase(textureLayers.begin() + index);
    }

    uint8_t TerrainData::GetSplatmapWeight(int x, int y, int layer) const
    {
        if (layer < 0 || layer >= 4)
            return 0;
        int idx = (y * splatmapResolution + x) * 4 + layer;
        if (idx < 0 || idx >= static_cast<int>(splatmaps.size()))
            return 0;
        return splatmaps[static_cast<size_t>(idx)];
    }

    void TerrainData::SetSplatmapWeight(int x, int y, int layer, uint8_t weight)
    {
        if (layer < 0 || layer >= 4)
            return;
        int idx = (y * splatmapResolution + x) * 4 + layer;
        if (idx < 0 || idx >= static_cast<int>(splatmaps.size()))
            return;
        splatmaps[static_cast<size_t>(idx)] = weight;
    }

} // namespace SparkEditor
