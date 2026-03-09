/**
 * @file ShadowAtlas.h
 * @brief Shared shadow atlas system with priority-based updates and static caching
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a single large depth texture atlas that all shadow-casting lights
 * share. Tiles are allocated via a grid-based bin-packer and updated according
 * to a per-frame budget so that only the N highest-priority shadow maps are
 * re-rendered each frame. Static shadows are cached and skipped unless
 * invalidated. Cascaded shadow maps are supported by grouping consecutive
 * tiles for a single directional light.
 *
 * GPU resources consist of one depth texture, one full-atlas SRV for shader
 * sampling, and per-tile viewport descriptors (DSVs are created as slices of
 * the atlas via viewport scissoring).
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Forward declarations
class Light;

// ============================================================================
// Enums
// ============================================================================

/**
 * @brief Shadow filtering mode per tile
 */
enum class ShadowFilterMode
{
    PCF,  ///< Percentage Closer Filtering
    VSM,  ///< Variance Shadow Maps
    PCSS  ///< Percentage Closer Soft Shadows
};

/**
 * @brief Whether a shadow tile caches static geometry or updates every frame
 */
enum class ShadowCacheMode
{
    Dynamic, ///< Re-rendered every frame (or when budget allows)
    Static   ///< Only re-rendered when explicitly invalidated
};

// ============================================================================
// ShadowAtlasSettings
// ============================================================================

/**
 * @brief Configuration for the shadow atlas
 */
struct ShadowAtlasSettings
{
    uint32_t atlasWidth  = 4096; ///< Atlas texture width in pixels
    uint32_t atlasHeight = 4096; ///< Atlas texture height in pixels

    uint32_t maxTiles = 64; ///< Maximum number of simultaneous shadow tiles

    /** Supported tile resolutions (must be power-of-two, sorted ascending). */
    static constexpr uint32_t TILE_SIZES[]     = {256, 512, 1024, 2048};
    static constexpr uint32_t TILE_SIZE_COUNT   = 4;
    static constexpr uint32_t MIN_TILE_SIZE     = 256;
    static constexpr uint32_t MAX_TILE_SIZE     = 2048;

    /** Maximum number of shadow map updates (render passes) per frame. */
    uint32_t updateBudgetPerFrame = 4;
};

// ============================================================================
// ShadowTile
// ============================================================================

/**
 * @brief A single rectangular region inside the shadow atlas
 */
struct ShadowTile
{
    // ---- Atlas location (pixels) ----
    uint32_t x      = 0; ///< Left edge in atlas (pixels)
    uint32_t y      = 0; ///< Top edge in atlas (pixels)
    uint32_t size   = 0; ///< Tile width/height (square, power-of-two)

    // ---- UV transform for shader lookup ----
    XMFLOAT4 uvScaleBias = {0.0f, 0.0f, 0.0f, 0.0f}; ///< (scaleU, scaleV, biasU, biasV)

    // ---- Association ----
    Light* light        = nullptr; ///< Non-owning pointer to the light that owns this tile
    int32_t cascadeIndex = -1;     ///< -1 for non-cascaded; 0..N for CSM cascade index

    // ---- Priority / scheduling ----
    float   priority        = 0.0f;  ///< Higher = more important to update this frame
    uint64_t lastUpdateFrame = 0;    ///< Engine frame number when last rendered
    bool    dirty           = true;  ///< True if the tile needs re-rendering

    // ---- Caching ----
    ShadowCacheMode cacheMode  = ShadowCacheMode::Dynamic;
    ShadowFilterMode filterMode = ShadowFilterMode::PCF;

    // ---- Shadow matrix ----
    XMMATRIX shadowMatrix = XMMatrixIdentity(); ///< World-to-shadow-clip transform

    // ---- Grid allocator bookkeeping ----
    uint32_t gridX = 0; ///< Grid cell column
    uint32_t gridY = 0; ///< Grid cell row
    uint32_t gridSpan = 1; ///< Number of grid cells spanned (size / minCellSize)
    bool     allocated = false;
};

// ============================================================================
// ShadowAtlasMetrics
// ============================================================================

/**
 * @brief Runtime statistics exposed to console / profiler
 */
struct ShadowAtlasMetrics
{
    uint32_t activeTiles      = 0;   ///< Currently allocated tiles
    uint32_t updatesThisFrame = 0;   ///< Shadow maps rendered this frame
    uint32_t cacheHits        = 0;   ///< Tiles skipped due to caching
    float    atlasUtilization = 0.0f; ///< Fraction of atlas area in use [0,1]
    uint32_t fragmentedCells  = 0;   ///< Free cells surrounded by allocated cells
    float    memoryUsageMB    = 0.0f; ///< Approximate GPU memory for the atlas
};

// ============================================================================
// ShadowAtlas
// ============================================================================

/**
 * @brief Shared shadow atlas manager
 *
 * Owns a single large D3D11 depth texture. Individual shadow-casting lights
 * request tiles of varying resolution; the atlas uses a uniform grid allocator
 * (cell size = smallest tile size) to place them. Each frame, a priority queue
 * determines which dirty tiles are re-rendered within the update budget.
 */
class ShadowAtlas
{
public:
    ShadowAtlas() = default;
    ~ShadowAtlas() { Shutdown(); }

    // Non-copyable, movable
    ShadowAtlas(const ShadowAtlas&) = delete;
    ShadowAtlas& operator=(const ShadowAtlas&) = delete;
    ShadowAtlas(ShadowAtlas&&) noexcept = default;
    ShadowAtlas& operator=(ShadowAtlas&&) noexcept = default;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Create the atlas depth texture and internal grid
     * @param device  D3D11 device (non-owning)
     * @param context D3D11 immediate context (non-owning)
     * @param settings Atlas configuration
     * @return S_OK on success
     */
    HRESULT Initialize(ID3D11Device* device,
                       ID3D11DeviceContext* context,
                       const ShadowAtlasSettings& settings = {})
    {
        if (!device || !context)
        {
            return E_INVALIDARG;
        }

        m_device   = device;
        m_context  = context;
        m_settings = settings;

        // Grid dimensions in cells of MIN_TILE_SIZE
        m_gridCols = m_settings.atlasWidth  / ShadowAtlasSettings::MIN_TILE_SIZE;
        m_gridRows = m_settings.atlasHeight / ShadowAtlasSettings::MIN_TILE_SIZE;
        m_gridOccupancy.assign(m_gridCols * m_gridRows, false);

        m_tiles.reserve(m_settings.maxTiles);

        return CreateGPUResources();
    }

    /**
     * @brief Release all GPU resources and reset state
     */
    void Shutdown()
    {
        m_tiles.clear();
        m_gridOccupancy.clear();
        m_atlasDSV.Reset();
        m_atlasSRV.Reset();
        m_atlasTexture.Reset();
        m_device  = nullptr;
        m_context = nullptr;
        m_metrics = {};
    }

    // ========================================================================
    // Tile allocation
    // ========================================================================

    /**
     * @brief Allocate a tile of the requested resolution for a light
     * @param light       Non-owning pointer to the requesting light
     * @param tileSize    Desired resolution (clamped to valid tile sizes)
     * @param filterMode  Shadow filter technique for this tile
     * @param cacheMode   Static or dynamic caching
     * @param cascadeIndex -1 for non-cascaded; >= 0 for CSM cascade
     * @return Pointer to the new tile, or nullptr if atlas is full
     */
    ShadowTile* AllocateTile(Light* light,
                             uint32_t tileSize,
                             ShadowFilterMode filterMode = ShadowFilterMode::PCF,
                             ShadowCacheMode cacheMode   = ShadowCacheMode::Dynamic,
                             int32_t cascadeIndex         = -1)
    {
        tileSize = SnapToValidTileSize(tileSize);

        if (m_tiles.size() >= m_settings.maxTiles)
        {
            return nullptr;
        }

        const uint32_t span = tileSize / ShadowAtlasSettings::MIN_TILE_SIZE;

        uint32_t cellX = 0;
        uint32_t cellY = 0;
        if (!FindFreeRegion(span, cellX, cellY))
        {
            return nullptr; // Atlas full
        }

        MarkRegion(cellX, cellY, span, true);

        ShadowTile tile;
        tile.x            = cellX * ShadowAtlasSettings::MIN_TILE_SIZE;
        tile.y            = cellY * ShadowAtlasSettings::MIN_TILE_SIZE;
        tile.size         = tileSize;
        tile.light        = light;
        tile.cascadeIndex = cascadeIndex;
        tile.filterMode   = filterMode;
        tile.cacheMode    = cacheMode;
        tile.dirty        = true;
        tile.allocated    = true;
        tile.gridX        = cellX;
        tile.gridY        = cellY;
        tile.gridSpan     = span;

        // Precompute UV scale/bias for shader sampling
        const float invW = 1.0f / static_cast<float>(m_settings.atlasWidth);
        const float invH = 1.0f / static_cast<float>(m_settings.atlasHeight);
        tile.uvScaleBias =
        {
            static_cast<float>(tileSize) * invW,
            static_cast<float>(tileSize) * invH,
            static_cast<float>(tile.x)   * invW,
            static_cast<float>(tile.y)   * invH
        };

        m_tiles.push_back(tile);
        return &m_tiles.back();
    }

    /**
     * @brief Allocate a set of tiles for cascaded shadow maps
     * @param light       Directional light
     * @param cascadeCount Number of cascades (1-4)
     * @param tileSizePerCascade Resolution per cascade tile
     * @param filterMode  Shadow filter
     * @return Vector of pointers to allocated tiles (empty on failure)
     */
    std::vector<ShadowTile*> AllocateCascadeTiles(Light* light,
                                                  uint32_t cascadeCount,
                                                  uint32_t tileSizePerCascade,
                                                  ShadowFilterMode filterMode = ShadowFilterMode::PCF)
    {
        std::vector<ShadowTile*> result;
        result.reserve(cascadeCount);

        for (uint32_t i = 0; i < cascadeCount; ++i)
        {
            ShadowTile* tile = AllocateTile(light, tileSizePerCascade, filterMode,
                                            ShadowCacheMode::Dynamic,
                                            static_cast<int32_t>(i));
            if (!tile)
            {
                // Roll back all allocated cascade tiles
                for (auto* t : result)
                {
                    FreeTile(t);
                }
                return {};
            }
            result.push_back(tile);
        }
        return result;
    }

    /**
     * @brief Free a previously allocated tile
     * @param tile Pointer obtained from AllocateTile; nullptr is safe
     */
    void FreeTile(ShadowTile* tile)
    {
        if (!tile)
        {
            return;
        }

        MarkRegion(tile->gridX, tile->gridY, tile->gridSpan, false);

        auto it = std::find_if(m_tiles.begin(), m_tiles.end(),
            [tile](const ShadowTile& t) { return &t == tile; });

        if (it != m_tiles.end())
        {
            // Swap-and-pop for O(1) removal
            if (it != m_tiles.end() - 1)
            {
                *it = std::move(m_tiles.back());
            }
            m_tiles.pop_back();
        }
    }

    /**
     * @brief Free all tiles belonging to a specific light
     */
    void FreeTilesForLight(const Light* light)
    {
        for (auto it = m_tiles.begin(); it != m_tiles.end(); )
        {
            if (it->light == light)
            {
                MarkRegion(it->gridX, it->gridY, it->gridSpan, false);
                if (it != m_tiles.end() - 1)
                {
                    *it = std::move(m_tiles.back());
                }
                m_tiles.pop_back();
            }
            else
            {
                ++it;
            }
        }
    }

    // ========================================================================
    // Per-frame update
    // ========================================================================

    /**
     * @brief Select which tiles to re-render this frame based on priority budget
     * @param currentFrame Current engine frame number
     * @return Tiles selected for rendering this frame (up to budget)
     */
    std::vector<ShadowTile*> BeginFrame(uint64_t currentFrame)
    {
        m_currentFrame = currentFrame;
        m_metrics.updatesThisFrame = 0;
        m_metrics.cacheHits        = 0;
        m_metrics.activeTiles      = static_cast<uint32_t>(m_tiles.size());

        // Collect dirty / dynamic candidates
        std::vector<ShadowTile*> candidates;
        candidates.reserve(m_tiles.size());

        for (auto& tile : m_tiles)
        {
            if (tile.cacheMode == ShadowCacheMode::Static && !tile.dirty)
            {
                ++m_metrics.cacheHits;
                continue;
            }
            if (tile.dirty || tile.cacheMode == ShadowCacheMode::Dynamic)
            {
                candidates.push_back(&tile);
            }
        }

        // Sort by descending priority
        std::sort(candidates.begin(), candidates.end(),
            [](const ShadowTile* a, const ShadowTile* b)
            {
                return a->priority > b->priority;
            });

        // Clamp to budget
        const uint32_t budget = m_settings.updateBudgetPerFrame;
        if (candidates.size() > budget)
        {
            candidates.resize(budget);
        }

        m_metrics.updatesThisFrame = static_cast<uint32_t>(candidates.size());
        return candidates;
    }

    /**
     * @brief Set the viewport/scissor on the device context for rendering into a tile
     * @param tile The tile to prepare for rendering
     *
     * The caller should bind the atlas DSV before calling this, then render
     * shadow geometry. The viewport is restricted to the tile region.
     */
    void SetTileViewport(const ShadowTile& tile) const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context)
        {
            return;
        }

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = static_cast<float>(tile.x);
        vp.TopLeftY = static_cast<float>(tile.y);
        vp.Width    = static_cast<float>(tile.size);
        vp.Height   = static_cast<float>(tile.size);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);

        D3D11_RECT scissor = {};
        scissor.left   = static_cast<LONG>(tile.x);
        scissor.top    = static_cast<LONG>(tile.y);
        scissor.right  = static_cast<LONG>(tile.x + tile.size);
        scissor.bottom = static_cast<LONG>(tile.y + tile.size);
        m_context->RSSetScissorRects(1, &scissor);
#else
        (void)tile;
#endif
    }

    /**
     * @brief Mark a tile as successfully rendered this frame
     */
    void MarkTileUpdated(ShadowTile& tile)
    {
        tile.lastUpdateFrame = m_currentFrame;
        tile.dirty = false;
    }

    /**
     * @brief Invalidate a tile so it will be re-rendered (even if static)
     */
    void InvalidateTile(ShadowTile& tile)
    {
        tile.dirty = true;
    }

    /**
     * @brief Invalidate all tiles (e.g. after a scene change)
     */
    void InvalidateAll()
    {
        for (auto& tile : m_tiles)
        {
            tile.dirty = true;
        }
    }

    // ========================================================================
    // Shadow matrix / UV helpers
    // ========================================================================

    /**
     * @brief Get the world-to-shadow-clip matrix for a tile
     */
    XMMATRIX GetShadowMatrix(const ShadowTile& tile) const
    {
        return tile.shadowMatrix;
    }

    /**
     * @brief Set the shadow matrix on a tile (called after computing light VP)
     */
    void SetShadowMatrix(ShadowTile& tile, const XMMATRIX& lightViewProj)
    {
        tile.shadowMatrix = lightViewProj;
    }

    /**
     * @brief Get the UV transform to convert shadow-clip coords to atlas UVs
     * @return XMFLOAT4(scaleU, scaleV, biasU, biasV)
     *
     * In the shader:
     *   atlasUV = shadowClipXY * 0.5 + 0.5;            // NDC -> [0,1]
     *   atlasUV = atlasUV * float2(scale) + float2(bias);
     */
    XMFLOAT4 GetAtlasUVTransform(const ShadowTile& tile) const
    {
        return tile.uvScaleBias;
    }

    // ========================================================================
    // Defragmentation
    // ========================================================================

    /**
     * @brief Compact the atlas by repacking tiles to eliminate internal gaps
     *
     * This is an offline operation that invalidates all tiles (forces full
     * re-render). Call sparingly, e.g. on level load or when fragmentation
     * is high.
     */
    void Defragment()
    {
        // Sort tiles by size descending for better packing
        std::sort(m_tiles.begin(), m_tiles.end(),
            [](const ShadowTile& a, const ShadowTile& b)
            {
                return a.size > b.size;
            });

        // Clear grid
        std::fill(m_gridOccupancy.begin(), m_gridOccupancy.end(), false);

        // Re-place each tile
        for (auto& tile : m_tiles)
        {
            const uint32_t span = tile.size / ShadowAtlasSettings::MIN_TILE_SIZE;
            uint32_t cellX = 0;
            uint32_t cellY = 0;

            if (FindFreeRegion(span, cellX, cellY))
            {
                MarkRegion(cellX, cellY, span, true);
                tile.gridX    = cellX;
                tile.gridY    = cellY;
                tile.gridSpan = span;
                tile.x        = cellX * ShadowAtlasSettings::MIN_TILE_SIZE;
                tile.y        = cellY * ShadowAtlasSettings::MIN_TILE_SIZE;

                const float invW = 1.0f / static_cast<float>(m_settings.atlasWidth);
                const float invH = 1.0f / static_cast<float>(m_settings.atlasHeight);
                tile.uvScaleBias =
                {
                    static_cast<float>(tile.size) * invW,
                    static_cast<float>(tile.size) * invH,
                    static_cast<float>(tile.x)    * invW,
                    static_cast<float>(tile.y)    * invH
                };
            }

            tile.dirty = true; // Must re-render after move
        }
    }

    // ========================================================================
    // GPU resource accessors
    // ========================================================================

    /** @brief Full atlas depth texture */
    ID3D11Texture2D* GetAtlasTexture() const { return m_atlasTexture.Get(); }

    /** @brief Atlas-wide shader resource view for shadow sampling */
    ID3D11ShaderResourceView* GetAtlasSRV() const { return m_atlasSRV.Get(); }

    /** @brief Atlas-wide depth stencil view for rendering */
    ID3D11DepthStencilView* GetAtlasDSV() const { return m_atlasDSV.Get(); }

    // ========================================================================
    // Queries
    // ========================================================================

    const ShadowAtlasSettings& GetSettings() const { return m_settings; }

    const std::vector<ShadowTile>& GetTiles() const { return m_tiles; }

    uint32_t GetActiveTileCount() const { return static_cast<uint32_t>(m_tiles.size()); }

    /**
     * @brief Find tiles belonging to a specific light
     */
    std::vector<ShadowTile*> GetTilesForLight(const Light* light)
    {
        std::vector<ShadowTile*> result;
        for (auto& tile : m_tiles)
        {
            if (tile.light == light)
            {
                result.push_back(&tile);
            }
        }
        return result;
    }

    // ========================================================================
    // Metrics
    // ========================================================================

    /**
     * @brief Recompute and return current metrics snapshot
     */
    ShadowAtlasMetrics GetMetrics() const
    {
        ShadowAtlasMetrics m = m_metrics;
        m.activeTiles = static_cast<uint32_t>(m_tiles.size());

        // Utilization: sum of tile areas / atlas area
        uint64_t usedArea = 0;
        for (const auto& tile : m_tiles)
        {
            usedArea += static_cast<uint64_t>(tile.size) * tile.size;
        }
        const uint64_t totalArea = static_cast<uint64_t>(m_settings.atlasWidth)
                                 * m_settings.atlasHeight;
        m.atlasUtilization = totalArea > 0
            ? static_cast<float>(usedArea) / static_cast<float>(totalArea)
            : 0.0f;

        // Fragmented cells: free cells that have at least one occupied neighbor
        m.fragmentedCells = CountFragmentedCells();

        // Memory: depth32 = 4 bytes/pixel
        m.memoryUsageMB = static_cast<float>(totalArea * 4) / (1024.0f * 1024.0f);

        return m;
    }

    // ========================================================================
    // Console integration
    // ========================================================================

    /**
     * @brief Return a summary string suitable for the debug console
     */
    std::string Console_GetStatus() const
    {
        const auto m = GetMetrics();
        return std::format(
            "ShadowAtlas {}x{} | Tiles: {}/{} | Util: {:.1f}% | "
            "Updates/frame: {} | Cache hits: {} | Frag cells: {} | Mem: {:.2f} MB",
            m_settings.atlasWidth, m_settings.atlasHeight,
            m.activeTiles, m_settings.maxTiles,
            m.atlasUtilization * 100.0f,
            m.updatesThisFrame, m.cacheHits,
            m.fragmentedCells, m.memoryUsageMB);
    }

    /**
     * @brief List all active tiles (for console)
     */
    std::string Console_ListTiles() const
    {
        std::string result;
        for (uint32_t i = 0; i < m_tiles.size(); ++i)
        {
            const auto& t = m_tiles[i];
            result += std::format(
                "[{}] {}x{} at ({},{}) cascade={} filter={} cache={} dirty={} prio={:.2f}\n",
                i, t.size, t.size, t.x, t.y,
                t.cascadeIndex,
                FilterModeToString(t.filterMode),
                CacheModeToString(t.cacheMode),
                t.dirty ? "yes" : "no",
                t.priority);
        }
        if (result.empty())
        {
            result = "No active shadow tiles.\n";
        }
        return result;
    }

    /**
     * @brief Set the per-frame update budget via console
     */
    void Console_SetUpdateBudget(uint32_t budget)
    {
        m_settings.updateBudgetPerFrame = budget;
    }

    /**
     * @brief Force defragmentation via console
     */
    void Console_Defragment()
    {
        Defragment();
    }

    /**
     * @brief Invalidate all tiles via console
     */
    void Console_InvalidateAll()
    {
        InvalidateAll();
    }

private:
    // ========================================================================
    // GPU resource creation
    // ========================================================================

    HRESULT CreateGPUResources()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // --- Depth texture ---
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width              = m_settings.atlasWidth;
        texDesc.Height             = m_settings.atlasHeight;
        texDesc.MipLevels          = 1;
        texDesc.ArraySize          = 1;
        texDesc.Format             = DXGI_FORMAT_R32_TYPELESS;
        texDesc.SampleDesc.Count   = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage              = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags          = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags     = 0;
        texDesc.MiscFlags          = 0;

        HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr,
                                                m_atlasTexture.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }

        // --- Depth stencil view (full atlas) ---
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format             = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension      = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = 0;

        hr = m_device->CreateDepthStencilView(m_atlasTexture.Get(), &dsvDesc,
                                               m_atlasDSV.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }

        // --- Shader resource view ---
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels       = 1;

        hr = m_device->CreateShaderResourceView(m_atlasTexture.Get(), &srvDesc,
                                                 m_atlasSRV.ReleaseAndGetAddressOf());
        return hr;
#else
        return S_OK;
#endif
    }

    // ========================================================================
    // Grid allocator helpers
    // ========================================================================

    /**
     * @brief Find a free rectangular region of span x span cells
     * @return true if found, with cellX/cellY set
     */
    bool FindFreeRegion(uint32_t span, uint32_t& outX, uint32_t& outY) const
    {
        for (uint32_t y = 0; y + span <= m_gridRows; ++y)
        {
            for (uint32_t x = 0; x + span <= m_gridCols; ++x)
            {
                if (IsRegionFree(x, y, span))
                {
                    outX = x;
                    outY = y;
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * @brief Check whether a span x span block starting at (cx, cy) is entirely free
     */
    bool IsRegionFree(uint32_t cx, uint32_t cy, uint32_t span) const
    {
        for (uint32_t dy = 0; dy < span; ++dy)
        {
            for (uint32_t dx = 0; dx < span; ++dx)
            {
                if (m_gridOccupancy[(cy + dy) * m_gridCols + (cx + dx)])
                {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * @brief Mark/unmark a region in the occupancy grid
     */
    void MarkRegion(uint32_t cx, uint32_t cy, uint32_t span, bool occupied)
    {
        for (uint32_t dy = 0; dy < span; ++dy)
        {
            for (uint32_t dx = 0; dx < span; ++dx)
            {
                m_gridOccupancy[(cy + dy) * m_gridCols + (cx + dx)] = occupied;
            }
        }
    }

    /**
     * @brief Snap a requested size to the nearest valid tile size
     */
    static uint32_t SnapToValidTileSize(uint32_t requested)
    {
        for (int i = static_cast<int>(ShadowAtlasSettings::TILE_SIZE_COUNT) - 1; i >= 0; --i)
        {
            if (requested >= ShadowAtlasSettings::TILE_SIZES[i])
            {
                return ShadowAtlasSettings::TILE_SIZES[i];
            }
        }
        return ShadowAtlasSettings::MIN_TILE_SIZE;
    }

    /**
     * @brief Count free cells that have at least one occupied neighbour
     */
    uint32_t CountFragmentedCells() const
    {
        uint32_t count = 0;
        for (uint32_t y = 0; y < m_gridRows; ++y)
        {
            for (uint32_t x = 0; x < m_gridCols; ++x)
            {
                const uint32_t idx = y * m_gridCols + x;
                if (m_gridOccupancy[idx])
                {
                    continue; // occupied
                }
                // Check 4-connected neighbours
                bool hasOccupiedNeighbour = false;
                if (x > 0            && m_gridOccupancy[idx - 1])           hasOccupiedNeighbour = true;
                if (x + 1 < m_gridCols && m_gridOccupancy[idx + 1])         hasOccupiedNeighbour = true;
                if (y > 0            && m_gridOccupancy[idx - m_gridCols])   hasOccupiedNeighbour = true;
                if (y + 1 < m_gridRows && m_gridOccupancy[idx + m_gridCols]) hasOccupiedNeighbour = true;

                if (hasOccupiedNeighbour)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    // ========================================================================
    // String helpers
    // ========================================================================

    static const char* FilterModeToString(ShadowFilterMode mode)
    {
        switch (mode)
        {
            case ShadowFilterMode::PCF:  return "PCF";
            case ShadowFilterMode::VSM:  return "VSM";
            case ShadowFilterMode::PCSS: return "PCSS";
        }
        return "Unknown";
    }

    static const char* CacheModeToString(ShadowCacheMode mode)
    {
        switch (mode)
        {
            case ShadowCacheMode::Dynamic: return "Dynamic";
            case ShadowCacheMode::Static:  return "Static";
        }
        return "Unknown";
    }

    // ========================================================================
    // Member data
    // ========================================================================

    // Non-owning device pointers
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Settings
    ShadowAtlasSettings m_settings;

    // GPU resources
    ComPtr<ID3D11Texture2D>          m_atlasTexture;
    ComPtr<ID3D11DepthStencilView>   m_atlasDSV;
    ComPtr<ID3D11ShaderResourceView> m_atlasSRV;

    // Tile storage
    std::vector<ShadowTile> m_tiles;

    // Grid allocator
    uint32_t m_gridCols = 0;
    uint32_t m_gridRows = 0;
    std::vector<bool> m_gridOccupancy;

    // Frame tracking
    uint64_t m_currentFrame = 0;

    // Metrics (mutable because updated in const GetMetrics via cache counts)
    mutable ShadowAtlasMetrics m_metrics;
};
