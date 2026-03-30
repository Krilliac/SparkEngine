/**
 * @file TerrainEditorTools.cpp
 * @brief Tool application, coordinate conversion, noise, and erosion for the terrain editor
 *
 * Contains the "doing" side of the terrain editor — sculpting brushes, texture painting,
 * procedural generation, and the math helpers they depend on.
 */

#include "TerrainEditor.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace DirectX;
namespace SparkEditor
{

    // =========================================================================
    // Procedural generation tools
    // =========================================================================

    void TerrainEditor::GenerateNoiseHeightmap(int octaves, float frequency, float amplitude, float lacunarity,
                                               float persistence)
    {
        if (!m_currentTerrain)
            return;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Generating noise heightmap: octaves=%d, freq=%.2f, amp=%.2f",
                       octaves, frequency, amplitude);

        auto& hm = m_currentTerrain->heightmap;
        hm.heights.resize(static_cast<size_t>(hm.width) * static_cast<size_t>(hm.height));

        for (int y = 0; y < hm.height; ++y)
        {
            for (int x = 0; x < hm.width; ++x)
            {
                float h = 0.0f;
                float freq = frequency;
                float amp = amplitude;
                for (int o = 0; o < octaves; ++o)
                {
                    h += GeneratePerlinNoise(static_cast<float>(x), static_cast<float>(y), freq) * amp;
                    freq *= lacunarity;
                    amp *= persistence;
                }
                hm.SetHeight(x, y, h);
            }
        }

        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(true);
    }

    void TerrainEditor::SmoothTerrain(int iterations, float strength)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        for (int iter = 0; iter < iterations; ++iter)
        {
            // Work on a copy so reads don't see partially-smoothed data
            std::vector<float> smoothed = hm.heights;
            for (int y = 1; y < hm.height - 1; ++y)
            {
                for (int x = 1; x < hm.width - 1; ++x)
                {
                    float avg = (hm.GetHeight(x - 1, y) + hm.GetHeight(x + 1, y) + hm.GetHeight(x, y - 1) +
                                 hm.GetHeight(x, y + 1)) *
                                0.25f;
                    float current = hm.GetHeight(x, y);
                    smoothed[static_cast<size_t>(y) * static_cast<size_t>(hm.width) + static_cast<size_t>(x)] =
                        current + (avg - current) * strength;
                }
            }
            hm.heights = std::move(smoothed);
        }

        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(true);
    }

    void TerrainEditor::ApplyErosion(int iterations, float strength, float evaporationRate, float depositionRate)
    {
        if (!m_currentTerrain)
            return;

        // Simple thermal erosion: pick random points and erode toward lowest neighbor
        auto& hm = m_currentTerrain->heightmap;
        for (int i = 0; i < iterations; ++i)
        {
            int x = std::rand() % hm.width;
            int y = std::rand() % hm.height;
            ApplyHydraulicErosion(x, y, strength);
        }
        (void)evaporationRate;
        (void)depositionRate;

        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(true);
    }

    void TerrainEditor::AutoGenerateTexturePlacement(int layerIndex)
    {
        if (!m_currentTerrain || layerIndex < 0 ||
            layerIndex >= static_cast<int>(m_currentTerrain->textureLayers.size()))
            return;

        auto* layer = m_currentTerrain->textureLayers[static_cast<size_t>(layerIndex)].get();
        if (!layer->useAutoPlacement)
            return;

        // Walk every splatmap texel, sample the heightmap height and slope,
        // and paint weight where the layer's placement rules are satisfied.
        int res = m_currentTerrain->splatmapResolution;
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                float hmX = static_cast<float>(x) / static_cast<float>(res) *
                            static_cast<float>(m_currentTerrain->heightmap.width - 1);
                float hmY = static_cast<float>(y) / static_cast<float>(res) *
                            static_cast<float>(m_currentTerrain->heightmap.height - 1);

                float h = m_currentTerrain->heightmap.GetHeight(static_cast<int>(hmX), static_cast<int>(hmY));
                float slope = CalculateTerrainSlope(static_cast<int>(hmX), static_cast<int>(hmY));

                bool inRange = h >= layer->minHeight && h <= layer->maxHeight && slope >= layer->minSlope &&
                               slope <= layer->maxSlope;

                if (inRange && layerIndex < 4)
                {
                    auto w = static_cast<uint8_t>(layer->placementStrength * 255.0f);
                    m_currentTerrain->SetSplatmapWeight(x, y, layerIndex, w);
                }
            }
        }
        SetModified(true);
    }

    // --- Tool application ---

    void TerrainEditor::ApplySculptingTool(const XMFLOAT3& worldPosition, float strength)
    {
        if (!m_currentTerrain)
            return;

        int hx = 0, hy = 0;
        if (!WorldToHeightmapCoords(worldPosition, hx, hy))
            return;

        float brushRadius =
            m_brushSettings.radius / m_currentTerrain->size * static_cast<float>(m_currentTerrain->heightmap.width);
        float delta = m_brushSettings.strength * strength;

        switch (m_currentTool)
        {
        case TerrainTool::SCULPT_RAISE:
            ModifyTerrainHeight(hx, hy, brushRadius, delta, m_brushSettings.falloffType);
            break;
        case TerrainTool::SCULPT_LOWER:
            ModifyTerrainHeight(hx, hy, brushRadius, -delta, m_brushSettings.falloffType);
            break;
        case TerrainTool::SCULPT_SMOOTH:
        {
            int r = static_cast<int>(brushRadius);
            for (int dy = -r; dy <= r; ++dy)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / brushRadius;
                    if (dist > 1.0f)
                        continue;

                    int px = hx + dx, py = hy + dy;
                    float avg = (m_currentTerrain->heightmap.GetHeight(px - 1, py) +
                                 m_currentTerrain->heightmap.GetHeight(px + 1, py) +
                                 m_currentTerrain->heightmap.GetHeight(px, py - 1) +
                                 m_currentTerrain->heightmap.GetHeight(px, py + 1)) *
                                0.25f;
                    float current = m_currentTerrain->heightmap.GetHeight(px, py);
                    float falloff = m_brushSettings.EvaluateFalloff(dist);
                    float newH = current + (avg - current) * strength * falloff;
                    m_currentTerrain->heightmap.SetHeight(px, py, newH);
                }
            }
            break;
        }
        case TerrainTool::SCULPT_FLATTEN:
        {
            float targetHeight = m_currentTerrain->heightmap.GetHeight(hx, hy);
            int r = static_cast<int>(brushRadius);
            for (int dy = -r; dy <= r; ++dy)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / brushRadius;
                    if (dist > 1.0f)
                        continue;
                    float falloff = m_brushSettings.EvaluateFalloff(dist);
                    float current = m_currentTerrain->heightmap.GetHeight(hx + dx, hy + dy);
                    float newH = current + (targetHeight - current) * falloff * strength;
                    m_currentTerrain->heightmap.SetHeight(hx + dx, hy + dy, newH);
                }
            }
            break;
        }
        default:
            break;
        }
        m_meshDirty = true;
        m_collisionDirty = true;
        SetModified(true);
    }

    void TerrainEditor::ApplyTexturePaintingTool(const XMFLOAT3& worldPosition, float strength)
    {
        if (!m_currentTerrain || m_selectedTextureLayer < 0 || m_selectedTextureLayer >= 4)
            return;

        int sx = 0, sy = 0;
        if (!WorldToSplatmapCoords(worldPosition, sx, sy))
            return;

        float brushRadius =
            m_brushSettings.radius / m_currentTerrain->size * static_cast<float>(m_currentTerrain->splatmapResolution);
        int r = static_cast<int>(brushRadius);

        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / brushRadius;
                if (dist > 1.0f)
                    continue;
                float falloff = m_brushSettings.EvaluateFalloff(dist);
                PaintTextureWeight(sx + dx, sy + dy, 1.0f, m_selectedTextureLayer, strength * falloff);
            }
        }
        SetModified(true);
    }

    void TerrainEditor::ApplyDetailPlacementTool(const XMFLOAT3& worldPosition, float /*strength*/)
    {
        if (!m_currentTerrain || m_selectedDetailMesh < 0 ||
            m_selectedDetailMesh >= static_cast<int>(m_currentTerrain->detailMeshes.size()))
            return;

        XMFLOAT4 region = {worldPosition.x - m_brushSettings.radius, worldPosition.z - m_brushSettings.radius,
                           worldPosition.x + m_brushSettings.radius, worldPosition.z + m_brushSettings.radius};
        float density = m_currentTerrain->detailMeshes[static_cast<size_t>(m_selectedDetailMesh)]->density;
        PlaceDetailMeshes(m_selectedDetailMesh, region, density);
    }

    void TerrainEditor::ModifyTerrainHeight(int centerX, int centerY, float radius, float heightDelta,
                                            TerrainBrush::FalloffType falloffType)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        int r = static_cast<int>(radius);

        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                int px = centerX + dx;
                int py = centerY + dy;
                if (px < 0 || px >= hm.width || py < 0 || py >= hm.height)
                    continue;

                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / radius;
                if (dist > 1.0f)
                    continue;

                TerrainBrush tempBrush;
                tempBrush.falloffType = falloffType;
                float falloff = tempBrush.EvaluateFalloff(dist);

                float current = hm.GetHeight(px, py);
                hm.SetHeight(px, py, current + heightDelta * falloff);
            }
        }
    }

    void TerrainEditor::PaintTextureWeight(int centerX, int centerY, float /*radius*/, int layerIndex, float strength)
    {
        if (!m_currentTerrain || layerIndex < 0 || layerIndex >= 4)
            return;

        int res = m_currentTerrain->splatmapResolution;
        if (centerX < 0 || centerX >= res || centerY < 0 || centerY >= res)
            return;

        uint8_t current = m_currentTerrain->GetSplatmapWeight(centerX, centerY, layerIndex);
        auto newVal = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(current) + strength * 255.0f));
        m_currentTerrain->SetSplatmapWeight(centerX, centerY, layerIndex, newVal);
    }

    // --- Math helpers ---

    XMFLOAT3 TerrainEditor::CalculateTerrainNormal(int x, int y) const
    {
        if (!m_currentTerrain)
            return {0.0f, 1.0f, 0.0f};

        auto& hm = m_currentTerrain->heightmap;
        float hL = hm.GetHeight(x - 1, y);
        float hR = hm.GetHeight(x + 1, y);
        float hD = hm.GetHeight(x, y - 1);
        float hU = hm.GetHeight(x, y + 1);

        float cellSize = m_currentTerrain->size / static_cast<float>(hm.width);
        XMFLOAT3 normal = {(hL - hR) / (2.0f * cellSize), 1.0f, (hD - hU) / (2.0f * cellSize)};

        float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (len > 0.0f)
        {
            normal.x /= len;
            normal.y /= len;
            normal.z /= len;
        }
        return normal;
    }

    float TerrainEditor::CalculateTerrainSlope(int x, int y) const
    {
        XMFLOAT3 normal = CalculateTerrainNormal(x, y);
        float dot = normal.y;
        return std::acos(std::clamp(dot, -1.0f, 1.0f)) * MathUtils::RAD_TO_DEG;
    }

    bool TerrainEditor::WorldToHeightmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const
    {
        if (!m_currentTerrain)
            return false;

        float relX = worldPosition.x - m_currentTerrain->position.x;
        float relZ = worldPosition.z - m_currentTerrain->position.z;
        float halfSize = m_currentTerrain->size * 0.5f;

        outX = static_cast<int>((relX + halfSize) / m_currentTerrain->size *
                                static_cast<float>(m_currentTerrain->heightmap.width - 1));
        outY = static_cast<int>((relZ + halfSize) / m_currentTerrain->size *
                                static_cast<float>(m_currentTerrain->heightmap.height - 1));

        return outX >= 0 && outX < m_currentTerrain->heightmap.width && outY >= 0 &&
               outY < m_currentTerrain->heightmap.height;
    }

    bool TerrainEditor::WorldToSplatmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const
    {
        if (!m_currentTerrain)
            return false;

        float relX = worldPosition.x - m_currentTerrain->position.x;
        float relZ = worldPosition.z - m_currentTerrain->position.z;
        float halfSize = m_currentTerrain->size * 0.5f;

        int res = m_currentTerrain->splatmapResolution;
        outX = static_cast<int>((relX + halfSize) / m_currentTerrain->size * static_cast<float>(res - 1));
        outY = static_cast<int>((relZ + halfSize) / m_currentTerrain->size * static_cast<float>(res - 1));

        return outX >= 0 && outX < res && outY >= 0 && outY < res;
    }

    float TerrainEditor::GeneratePerlinNoise(float x, float y, float frequency) const
    {
        float fx = x * frequency;
        float fy = y * frequency;

        int ix = static_cast<int>(std::floor(fx));
        int iy = static_cast<int>(std::floor(fy));
        float tx = fx - std::floor(fx);
        float ty = fy - std::floor(fy);

        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);

        auto hash = [](int xi, int yi) -> float
        {
            int n = xi * 73856093 ^ yi * 19349663;
            n = (n << 13) ^ n;
            return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f;
        };

        float c00 = hash(ix, iy);
        float c10 = hash(ix + 1, iy);
        float c01 = hash(ix, iy + 1);
        float c11 = hash(ix + 1, iy + 1);

        float top = c00 + (c10 - c00) * tx;
        float bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    void TerrainEditor::ApplyHydraulicErosion(int x, int y, float strength)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        if (x <= 0 || x >= hm.width - 1 || y <= 0 || y >= hm.height - 1)
            return;

        float center = hm.GetHeight(x, y);

        float minH = center;
        int minDx = 0, minDy = 0;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                float h = hm.GetHeight(x + dx, y + dy);
                if (h < minH)
                {
                    minH = h;
                    minDx = dx;
                    minDy = dy;
                }
            }
        }

        float diff = center - minH;
        if (diff > 0.001f)
        {
            float transfer = diff * strength * 0.5f;
            hm.SetHeight(x, y, center - transfer);
            hm.SetHeight(x + minDx, y + minDy, minH + transfer);
        }
    }

} // namespace SparkEditor
