/**
 * @file RTSFogOfWarSystem.cpp
 * @brief Grid-based fog of war: vision updates, exploration, and reveal/hide
 */

#include "RTSFogOfWarSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

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
        m_context = context;
        m_mapWidth = mapWidth;
        m_mapHeight = mapHeight;

        // Initialize grids for all factions
        for (int i = 0; i < static_cast<int>(RTSFaction::Count); ++i)
        {
            auto faction = static_cast<RTSFaction>(i);
            m_grids[faction].Resize(mapWidth, mapHeight);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS fog of war initialized (%dx%d grid)", mapWidth, mapHeight);
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Fog of war initialized (" + std::to_string(mapWidth) + "x" +
                                                    std::to_string(mapHeight) + " grid)");
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

    // === Vision updates ===

    void RTSFogOfWarSystem::UpdateVision(RTSFaction faction, float unitX, float unitY, float visionRange)
    {
        auto it = m_grids.find(faction);
        if (it == m_grids.end())
            return;

        auto& grid = it->second;
        int centerX = WorldToGrid(unitX);
        int centerY = WorldToGrid(unitY);
        int radius = static_cast<int>(std::ceil(visionRange / CELL_SIZE));

        // Reveal cells within vision radius
        for (int dy = -radius; dy <= radius; ++dy)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * CELL_SIZE;
                if (dist <= visionRange)
                {
                    grid.SetCell(centerX + dx, centerY + dy, RTSVisibility::Visible);
                }
            }
        }
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
        int cx = WorldToGrid(centerX);
        int cy = WorldToGrid(centerY);
        int r = static_cast<int>(std::ceil(radius / CELL_SIZE));

        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * CELL_SIZE;
                if (dist <= radius)
                {
                    int gx = cx + dx;
                    int gy = cy + dy;
                    if (gx >= 0 && gx < grid.width && gy >= 0 && gy < grid.height)
                    {
                        auto& cell = grid.cells[static_cast<size_t>(gy * grid.width + gx)];
                        if (cell == RTSVisibility::Visible)
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
        return static_cast<int>(std::floor(worldPos / CELL_SIZE));
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
