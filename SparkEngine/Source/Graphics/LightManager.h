/**
 * @file LightManager.h
 * @brief Light culling, tile binning, and shadow atlas management
 * @author Spark Engine Team
 * @date 2025
 *
 * Manages runtime light instances for efficient rendering. Provides frustum
 * culling to skip off-screen lights, tile-based binning for forward+
 * rendering, and shadow map atlas slot management.
 *
 * Works alongside LightingSystem.h which defines light component data and
 * environment lighting. This manager handles per-frame runtime processing.
 *
 * ## Usage
 * @code
 *   LightManager mgr;
 *   mgr.Initialize(1920, 1080, 16);
 *
 *   mgr.BeginFrame(viewProjMatrix);
 *   mgr.SubmitLight(pointLight);
 *   mgr.SubmitLight(spotLight);
 *   mgr.CullAndBinLights();
 *
 *   auto visible = mgr.GetVisibleLights();
 *   int slot = mgr.AllocateShadowSlot(lightId, 1024);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cmath>

// =============================================================================
// Light Types
// =============================================================================

enum class RuntimeLightType
{
    Directional,
    Point,
    Spot
};

// =============================================================================
// Runtime Light
// =============================================================================

/**
 * @brief Runtime light instance submitted for rendering each frame
 */
struct RuntimeLight
{
    uint32_t id = 0;
    RuntimeLightType type = RuntimeLightType::Point;

    XMFLOAT3 position = {0, 0, 0};
    XMFLOAT3 direction = {0, -1, 0};
    XMFLOAT3 color = {1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerAngle = 30.0f;
    float outerAngle = 45.0f;

    bool castsShadows = false;
    int shadowMapSize = 1024;
    float shadowBias = 0.001f;

    bool enabled = true;
    bool isVisible = true;
};

// =============================================================================
// View Frustum
// =============================================================================

/**
 * @brief 6-plane view frustum for culling
 */
struct ViewFrustum
{
    XMFLOAT4 planes[6];               ///< Left, Right, Bottom, Top, Near, Far
    DirectX::XMMATRIX viewProjMatrix; ///< Stored for screen-space projection in tile assignment

    bool TestSphere(const XMFLOAT3& center, float radius) const
    {
        for (int i = 0; i < 6; ++i)
        {
            float dist = planes[i].x * center.x + planes[i].y * center.y + planes[i].z * center.z + planes[i].w;
            if (dist < -radius)
                return false;
        }
        return true;
    }
};

inline ViewFrustum ExtractFrustumPlanes(const XMFLOAT4X4& vp)
{
    ViewFrustum f;
    f.planes[0] = {vp.m[0][3] + vp.m[0][0], vp.m[1][3] + vp.m[1][0], vp.m[2][3] + vp.m[2][0], vp.m[3][3] + vp.m[3][0]};
    f.planes[1] = {vp.m[0][3] - vp.m[0][0], vp.m[1][3] - vp.m[1][0], vp.m[2][3] - vp.m[2][0], vp.m[3][3] - vp.m[3][0]};
    f.planes[2] = {vp.m[0][3] + vp.m[0][1], vp.m[1][3] + vp.m[1][1], vp.m[2][3] + vp.m[2][1], vp.m[3][3] + vp.m[3][1]};
    f.planes[3] = {vp.m[0][3] - vp.m[0][1], vp.m[1][3] - vp.m[1][1], vp.m[2][3] - vp.m[2][1], vp.m[3][3] - vp.m[3][1]};
    f.planes[4] = {vp.m[0][2], vp.m[1][2], vp.m[2][2], vp.m[3][2]};
    f.planes[5] = {vp.m[0][3] - vp.m[0][2], vp.m[1][3] - vp.m[1][2], vp.m[2][3] - vp.m[2][2], vp.m[3][3] - vp.m[3][2]};

    for (int i = 0; i < 6; ++i)
    {
        float len =
            std::sqrt(f.planes[i].x * f.planes[i].x + f.planes[i].y * f.planes[i].y + f.planes[i].z * f.planes[i].z);
        if (len > 0.0001f)
        {
            f.planes[i].x /= len;
            f.planes[i].y /= len;
            f.planes[i].z /= len;
            f.planes[i].w /= len;
        }
    }
    f.viewProjMatrix = DirectX::XMLoadFloat4x4(&vp);
    return f;
}

// =============================================================================
// Tile Light List
// =============================================================================

struct TileLightList
{
    static constexpr int MAX_LIGHTS_PER_TILE = 256;
    uint32_t lightIndices[MAX_LIGHTS_PER_TILE] = {};
    int lightCount = 0;

    void AddLight(uint32_t index)
    {
        if (lightCount < MAX_LIGHTS_PER_TILE)
            lightIndices[lightCount++] = index;
    }
    void Clear() { lightCount = 0; }
};

// =============================================================================
// Shadow Atlas Slot
// =============================================================================

struct ShadowAtlasSlot
{
    uint32_t lightId = 0;
    int size = 0;
    bool inUse = false;
    float lastUpdateTime = 0.0f;
};

// =============================================================================
// Metrics
// =============================================================================

struct LightManagerMetrics
{
    int totalSubmitted = 0;
    int visibleAfterCull = 0;
    int shadowCasters = 0;
    int tilesUsed = 0;
    int maxLightsPerTile = 0;
    int shadowAtlasUsage = 0;
};

// =============================================================================
// Light Manager
// =============================================================================

/**
 * @class LightManager
 * @brief Per-frame light culling, tile binning, and shadow atlas management
 */
class LightManager
{
  public:
    LightManager() = default;
    ~LightManager() = default;

    /**
     * @brief Initialize with screen dimensions and tile size
     */
    bool Initialize(uint32_t width = 1920, uint32_t height = 1080, int tileSize = 16)
    {
        m_screenWidth = width;
        m_screenHeight = height;
        m_tileSize = tileSize;
        m_tilesX = (width + tileSize - 1) / tileSize;
        m_tilesY = (height + tileSize - 1) / tileSize;
        m_tileLightLists.resize(m_tilesX * m_tilesY);
        m_shadowAtlas.resize(MAX_SHADOW_SLOTS);
        m_initialized = true;
        return true;
    }

    void Shutdown()
    {
        m_lights.clear();
        m_tileLightLists.clear();
        m_shadowAtlas.clear();
        m_initialized = false;
    }

    void Update(float deltaTime) { m_totalTime += deltaTime; }

    /**
     * @brief Begin a new frame - clears submitted lights and tile lists
     */
    void BeginFrame(const XMFLOAT4X4& viewProjMatrix)
    {
        m_lights.clear();
        m_frustum = ExtractFrustumPlanes(viewProjMatrix);
        for (auto& tile : m_tileLightLists)
            tile.Clear();
        m_metrics = {};
    }

    /**
     * @brief Submit a light for this frame's rendering
     */
    void SubmitLight(const RuntimeLight& light)
    {
        if (!light.enabled)
            return;
        m_lights.push_back(light);
        m_metrics.totalSubmitted++;
    }

    /**
     * @brief Cull lights against frustum and bin into tiles
     */
    void CullAndBinLights()
    {
        if (!m_initialized)
            return;

        // Frustum culling
        for (auto& light : m_lights)
        {
            if (light.type == RuntimeLightType::Directional)
            {
                light.isVisible = true;
            }
            else
            {
                light.isVisible = m_frustum.TestSphere(light.position, light.range);
            }
            if (light.isVisible)
            {
                m_metrics.visibleAfterCull++;
                if (light.castsShadows)
                    m_metrics.shadowCasters++;
            }
        }

        // Tile binning
        for (uint32_t i = 0; i < m_lights.size(); ++i)
        {
            if (!m_lights[i].isVisible)
                continue;
            if (m_lights[i].type == RuntimeLightType::Directional)
            {
                for (auto& tile : m_tileLightLists)
                    tile.AddLight(i);
            }
            else
            {
                AssignLightToTiles(i, m_lights[i]);
            }
        }

        // Compute stats
        for (const auto& tile : m_tileLightLists)
        {
            if (tile.lightCount > 0)
                m_metrics.tilesUsed++;
            m_metrics.maxLightsPerTile = std::max(m_metrics.maxLightsPerTile, tile.lightCount);
        }
    }

    void ProcessLights() { CullAndBinLights(); }

    // ---- Accessors ----

    const std::vector<RuntimeLight>& GetLights() const { return m_lights; }

    std::vector<const RuntimeLight*> GetVisibleLights() const
    {
        std::vector<const RuntimeLight*> visible;
        for (const auto& l : m_lights)
            if (l.isVisible)
                visible.push_back(&l);
        return visible;
    }

    std::vector<const RuntimeLight*> GetShadowCasters() const
    {
        std::vector<const RuntimeLight*> casters;
        for (const auto& l : m_lights)
            if (l.isVisible && l.castsShadows)
                casters.push_back(&l);
        return casters;
    }

    const TileLightList& GetTileLightList(int tileX, int tileY) const
    {
        int idx = tileY * m_tilesX + tileX;
        if (idx >= 0 && idx < static_cast<int>(m_tileLightLists.size()))
            return m_tileLightLists[idx];
        static TileLightList empty;
        return empty;
    }

    int GetTilesX() const { return m_tilesX; }
    int GetTilesY() const { return m_tilesY; }

    int AllocateShadowSlot(uint32_t lightId, int mapSize)
    {
        for (int i = 0; i < static_cast<int>(m_shadowAtlas.size()); ++i)
            if (m_shadowAtlas[i].inUse && m_shadowAtlas[i].lightId == lightId)
                return i;
        for (int i = 0; i < static_cast<int>(m_shadowAtlas.size()); ++i)
        {
            if (!m_shadowAtlas[i].inUse)
            {
                m_shadowAtlas[i] = {lightId, mapSize, true, m_totalTime};
                m_metrics.shadowAtlasUsage++;
                return i;
            }
        }
        return -1;
    }

    void FreeShadowSlot(uint32_t lightId)
    {
        for (auto& slot : m_shadowAtlas)
        {
            if (slot.inUse && slot.lightId == lightId)
            {
                slot = {};
                m_metrics.shadowAtlasUsage--;
                return;
            }
        }
    }

    const LightManagerMetrics& GetMetrics() const { return m_metrics; }

    void Resize(uint32_t width, uint32_t height)
    {
        m_screenWidth = width;
        m_screenHeight = height;
        m_tilesX = (width + m_tileSize - 1) / m_tileSize;
        m_tilesY = (height + m_tileSize - 1) / m_tileSize;
        m_tileLightLists.resize(m_tilesX * m_tilesY);
    }

    std::string Console_GetStatus() const
    {
        std::string s = "Light Manager:\n";
        s += "  Submitted: " + std::to_string(m_metrics.totalSubmitted) + "\n";
        s += "  Visible: " + std::to_string(m_metrics.visibleAfterCull) + "\n";
        s += "  Shadow casters: " + std::to_string(m_metrics.shadowCasters) + "\n";
        s += "  Tiles: " + std::to_string(m_tilesX) + "x" + std::to_string(m_tilesY) + "\n";
        s += "  Max lights/tile: " + std::to_string(m_metrics.maxLightsPerTile) + "\n";
        s += "  Shadow atlas: " + std::to_string(m_metrics.shadowAtlasUsage) + "/" + std::to_string(MAX_SHADOW_SLOTS) +
             "\n";
        return s;
    }

  private:
    static constexpr int MAX_SHADOW_SLOTS = 16;

    /// Project a light's bounding sphere to screen space and assign to overlapping tiles.
    /// Directional lights affect all tiles. Point/spot lights use screen-space AABB.
    void AssignLightToTiles(uint32_t lightIndex, const RuntimeLight& light)
    {
        if (light.type == RuntimeLightType::Directional)
        {
            for (auto& tile : m_tileLightLists)
                tile.AddLight(lightIndex);
            return;
        }

        // Transform light center to clip space using the stored view-projection matrix
        DirectX::XMVECTOR lightPos = DirectX::XMVectorSet(light.position.x, light.position.y, light.position.z, 1.0f);
        DirectX::XMVECTOR clipPos = DirectX::XMVector3Transform(lightPos, m_frustum.viewProjMatrix);
        float w = DirectX::XMVectorGetW(clipPos);
        if (w <= 0.0f)
            return;

        float ndcX = DirectX::XMVectorGetX(clipPos) / w;
        float ndcY = DirectX::XMVectorGetY(clipPos) / w;

        // Estimate screen-space radius from light range
        float screenRadius = (light.range / w) * static_cast<float>(m_screenWidth) * 0.5f;

        // NDC to pixel coordinates
        float pixelX = (ndcX * 0.5f + 0.5f) * static_cast<float>(m_screenWidth);
        float pixelY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(m_screenHeight);

        // Tile range for the light's screen-space AABB
        int minTX = std::max(0, static_cast<int>((pixelX - screenRadius) / m_tileSize));
        int maxTX = std::min(m_tilesX - 1, static_cast<int>((pixelX + screenRadius) / m_tileSize));
        int minTY = std::max(0, static_cast<int>((pixelY - screenRadius) / m_tileSize));
        int maxTY = std::min(m_tilesY - 1, static_cast<int>((pixelY + screenRadius) / m_tileSize));

        for (int ty = minTY; ty <= maxTY; ++ty)
        {
            for (int tx = minTX; tx <= maxTX; ++tx)
            {
                m_tileLightLists[static_cast<size_t>(ty * m_tilesX + tx)].AddLight(lightIndex);
            }
        }
    }

    bool m_initialized = false;
    uint32_t m_screenWidth = 1920, m_screenHeight = 1080;
    int m_tileSize = 16;
    int m_tilesX = 0, m_tilesY = 0;
    float m_totalTime = 0.0f;

    ViewFrustum m_frustum;
    std::vector<RuntimeLight> m_lights;
    std::vector<TileLightList> m_tileLightLists;
    std::vector<ShadowAtlasSlot> m_shadowAtlas;
    LightManagerMetrics m_metrics;
};
