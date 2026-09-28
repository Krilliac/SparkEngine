/**
 * @file RTSFogOfWarSystem.cpp
 * @brief Grid-based fog of war: vision updates, exploration, and reveal/hide
 */

#include "RTSFogOfWarSystem.h"
#include "Spark/ModuleLog.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RTS
{

    // === FogGrid ===

    void FogGrid::Resize(int w, int h)
    {
        width = w;
        height = h;
        cells.assign(static_cast<size_t>(w * h), RTSVisibility::Unexplored);
    }

    RTSVisibility FogGrid::GetCell(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return RTSVisibility::Unexplored;

        return cells[static_cast<size_t>(y * width + x)];
    }

    void FogGrid::SetCell(int x, int y, RTSVisibility vis)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;

        auto& cell = cells[static_cast<size_t>(y * width + x)];

        // Visibility can only increase during a frame (Unexplored -> Fog -> Visible)
        if (static_cast<uint8_t>(vis) > static_cast<uint8_t>(cell))
            cell = vis;
    }

    // === RTSFogOfWarSystem ===

    bool RTSFogOfWarSystem::Initialize(Spark::IEngineContext* context, int mapWidth, int mapHeight)
    {
        // Same bound RestoreState enforces; WorldToGrid's saturation relies on it.
        if (mapWidth <= 0 || mapHeight <= 0 || mapWidth > MAX_MAP_DIMENSION || mapHeight > MAX_MAP_DIMENSION)
        {
            return false;
        }

        m_context = context;
        m_mapWidth = mapWidth;
        m_mapHeight = mapHeight;

        // Initialize grids for all factions
        for (int i = 0; i < static_cast<int>(RTSFaction::Count); ++i)
        {
            auto faction = static_cast<RTSFaction>(i);
            m_grids[faction].Resize(mapWidth, mapHeight);
        }

        Spark::ModuleLog::Info(m_context, "[RTS] Fog of war initialized ({}x{} grid)", mapWidth, mapHeight);
        return true;
    }

    void RTSFogOfWarSystem::Update(float deltaTime)
    {
        (void)deltaTime;
        // Vision is cleared and rebuilt each frame by the module's OnUpdate,
        // which calls ClearCurrentVision then UpdateVision for each unit.
    }

    void RTSFogOfWarSystem::Shutdown()
    {
        m_grids.clear();
    }

    const FogGrid* RTSFogOfWarSystem::GetGrid(RTSFaction faction) const
    {
        const auto it = m_grids.find(faction);
        return it != m_grids.end() ? &it->second : nullptr;
    }

    bool RTSFogOfWarSystem::RestoreState(const std::vector<FogGrid>& grids)
    {
        if (grids.size() != static_cast<size_t>(RTSFaction::Count))
            return false;
        const int width = grids.front().width;
        const int height = grids.front().height;
        if (width <= 0 || height <= 0 || width > MAX_MAP_DIMENSION || height > MAX_MAP_DIMENSION)
            return false;
        for (const FogGrid& grid : grids)
        {
            if (grid.width != width || grid.height != height ||
                grid.cells.size() != static_cast<size_t>(width) * static_cast<size_t>(height) ||
                !std::ranges::all_of(grid.cells, [](RTSVisibility cell) { return cell < RTSVisibility::Count; }))
            {
                return false;
            }
        }

        std::unordered_map<RTSFaction, FogGrid> restored;
        for (size_t index = 0; index < grids.size(); ++index)
            restored.emplace(static_cast<RTSFaction>(index), grids[index]);
        m_grids = std::move(restored);
        m_mapWidth = width;
        m_mapHeight = height;
        return true;
    }

    // === Vision updates ===

    RTSFogOfWarSystem::CellRect RTSFogOfWarSystem::ClipDisc(const FogGrid& grid, int centerX, int centerY, float radius)
    {
        if (!std::isfinite(radius) || radius < 0.0f || grid.width <= 0 || grid.height <= 0)
        {
            return {};
        }

        // Any radius past the grid's width + height already covers every cell. Clamping in float first keeps
        // the int conversion defined and the loop bounded regardless of what a save file supplied.
        const float reach = std::min(radius / CELL_SIZE, static_cast<float>(grid.width + grid.height));
        const int cells = static_cast<int>(std::ceil(reach));

        // centerX/centerY come from WorldToGrid, which saturates to +/-2 * MAX_MAP_DIMENSION, so these sums
        // cannot overflow.
        CellRect rect;
        rect.minX = std::max(0, centerX - cells);
        rect.maxX = std::min(grid.width - 1, centerX + cells);
        rect.minY = std::max(0, centerY - cells);
        rect.maxY = std::min(grid.height - 1, centerY + cells);
        return rect;
    }

    void RTSFogOfWarSystem::UpdateVision(RTSFaction faction, float unitX, float unitY, float visionRange)
    {
        auto it = m_grids.find(faction);
        if (it == m_grids.end())
            return;

        auto& grid = it->second;
        const int centerX = WorldToGrid(unitX);
        const int centerY = WorldToGrid(unitY);
        const CellRect rect = ClipDisc(grid, centerX, centerY, visionRange);

        // Reveal cells within vision radius (only those on the grid are visited)
        for (int y = rect.minY; y <= rect.maxY; ++y)
        {
            const int64_t dy = static_cast<int64_t>(y) - centerY;
            for (int x = rect.minX; x <= rect.maxX; ++x)
            {
                const int64_t dx = static_cast<int64_t>(x) - centerX;
                const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * CELL_SIZE;
                if (dist <= visionRange)
                {
                    grid.SetCell(x, y, RTSVisibility::Visible);
                }
            }
        }
    }

    size_t RTSFogOfWarSystem::VisionCellCost(RTSFaction faction, float unitX, float unitY, float visionRange) const
    {
        const auto it = m_grids.find(faction);
        if (it == m_grids.end())
        {
            return 0;
        }
        const CellRect rect = ClipDisc(it->second, WorldToGrid(unitX), WorldToGrid(unitY), visionRange);
        if (rect.minX > rect.maxX || rect.minY > rect.maxY)
        {
            return 0;
        }
        return static_cast<size_t>(rect.maxX - rect.minX + 1) * static_cast<size_t>(rect.maxY - rect.minY + 1);
    }

    void RTSFogOfWarSystem::ClearCurrentVision(RTSFaction faction)
    {
        auto it = m_grids.find(faction);
        if (it == m_grids.end())
            return;

        auto& grid = it->second;
        for (auto& cell : grid.cells)
        {
            // Visible -> Fog (explored but not currently seen)
            // Fog stays Fog, Unexplored stays Unexplored
            if (cell == RTSVisibility::Visible)
                cell = RTSVisibility::Fog;
        }
    }

    // === Queries ===

    bool RTSFogOfWarSystem::IsVisible(RTSFaction faction, float worldX, float worldY) const
    {
        return GetVisibilityAtPosition(faction, worldX, worldY) == RTSVisibility::Visible;
    }

    bool RTSFogOfWarSystem::IsExplored(RTSFaction faction, float worldX, float worldY) const
    {
        auto vis = GetVisibilityAtPosition(faction, worldX, worldY);
        return vis == RTSVisibility::Visible || vis == RTSVisibility::Fog;
    }

    RTSVisibility RTSFogOfWarSystem::GetVisibilityAtPosition(RTSFaction faction, float worldX, float worldY) const
    {
        auto it = m_grids.find(faction);
        if (it == m_grids.end())
            return RTSVisibility::Unexplored;

        int gx = WorldToGrid(worldX);
        int gy = WorldToGrid(worldY);
        return it->second.GetCell(gx, gy);
    }

    // === Abilities ===

    void RTSFogOfWarSystem::RevealArea(RTSFaction faction, float centerX, float centerY, float radius)
    {
        UpdateVision(faction, centerX, centerY, radius);
    }

    void RTSFogOfWarSystem::HideArea(RTSFaction faction, float centerX, float centerY, float radius)
    {
        auto it = m_grids.find(faction);
        if (it == m_grids.end())
            return;

        auto& grid = it->second;
        const int cx = WorldToGrid(centerX);
        const int cy = WorldToGrid(centerY);
        const CellRect rect = ClipDisc(grid, cx, cy, radius);

        for (int y = rect.minY; y <= rect.maxY; ++y)
        {
            const int64_t dy = static_cast<int64_t>(y) - cy;
            for (int x = rect.minX; x <= rect.maxX; ++x)
            {
                const int64_t dx = static_cast<int64_t>(x) - cx;
                const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * CELL_SIZE;
                if (dist <= radius)
                {
                    auto& cell =
                        grid.cells[static_cast<size_t>(y) * static_cast<size_t>(grid.width) + static_cast<size_t>(x)];
                    if (cell == RTSVisibility::Visible)
                    {
                        cell = RTSVisibility::Fog;
                    }
                }
            }
        }
    }

    int RTSFogOfWarSystem::GetMapWidth() const
    {
        return m_mapWidth;
    }

    int RTSFogOfWarSystem::GetMapHeight() const
    {
        return m_mapHeight;
    }

    float RTSFogOfWarSystem::GetExploredPercent(RTSFaction faction) const
    {
        auto it = m_grids.find(faction);
        if (it == m_grids.end())
            return 0.0f;

        const auto& grid = it->second;
        if (grid.cells.empty())
            return 0.0f;

        size_t explored = 0;
        for (const auto& cell : grid.cells)
        {
            if (cell != RTSVisibility::Unexplored)
                explored++;
        }

        return static_cast<float>(explored) / static_cast<float>(grid.cells.size()) * 100.0f;
    }

    std::string RTSFogOfWarSystem::GetFogStatusString() const
    {
        std::string result = "=== RTS Fog of War ===\n";
        result += "Map: " + std::to_string(m_mapWidth) + "x" + std::to_string(m_mapHeight) + "\n";

        const char* factionNames[] = {"Human", "Sentinel", "Swarm"};
        for (int i = 0; i < static_cast<int>(RTSFaction::Count); ++i)
        {
            auto faction = static_cast<RTSFaction>(i);
            float explored = GetExploredPercent(faction);
            result += std::string(factionNames[i]) + " explored: ";
            result += std::to_string(static_cast<int>(explored)) + "%\n";
        }
        return result;
    }

    // === Internal ===

    int RTSFogOfWarSystem::WorldToGrid(float worldPos) const
    {
        // Saturate before the cast: converting an out-of-range float to int is undefined behaviour, and every
        // position beyond +/-2 * MAX_MAP_DIMENSION is off the grid anyway. The negated comparison sends NaN to
        // the low bound.
        constexpr auto limit = static_cast<float>(2 * MAX_MAP_DIMENSION);
        const float cell = std::floor(worldPos / CELL_SIZE);
        if (!(cell > -limit))
        {
            return -2 * MAX_MAP_DIMENSION;
        }
        if (cell > limit)
        {
            return 2 * MAX_MAP_DIMENSION;
        }
        return static_cast<int>(cell);
    }

    void RTSFogOfWarSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RTS Fog of War"))
        {
            ImGui::Text("Map size: %dx%d", m_mapWidth, m_mapHeight);

            const char* names[] = {"Human", "Sentinel", "Swarm"};
            for (int i = 0; i < static_cast<int>(RTSFaction::Count); ++i)
            {
                auto faction = static_cast<RTSFaction>(i);
                ImGui::Text("%s explored: %.1f%%", names[i], GetExploredPercent(faction));
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RTS
