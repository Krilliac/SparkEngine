/**
 * @file RTSFogOfWarSystem.h
 * @brief Grid-based fog of war with vision ranges and exploration tracking
 * @author Spark Engine Team
 * @date 2026
 *
 * Maintains a per-player visibility grid that tracks unexplored, fogged,
 * and visible cells based on unit positions and vision ranges.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RTSEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTS
{

    /**
     * @brief Grid-based fog of war for a single player/faction
     */
    struct FogGrid
    {
        int width = 0;
        int height = 0;
        std::vector<RTSVisibility> cells; ///< Row-major grid of visibility states

        void Resize(int w, int h);
        RTSVisibility GetCell(int x, int y) const;
        void SetCell(int x, int y, RTSVisibility vis);
    };

    /**
     * @brief Manages fog of war grids for all factions
     */
    class RTSFogOfWarSystem
    {
      public:
        RTSFogOfWarSystem() = default;
        ~RTSFogOfWarSystem() = default;

        bool Initialize(Spark::IEngineContext* context, int mapWidth = 128, int mapHeight = 128);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Vision updates ===
        void UpdateVision(RTSFaction faction, float unitX, float unitY, float visionRange);
        void ClearCurrentVision(RTSFaction faction);

        // === Queries ===
        bool IsVisible(RTSFaction faction, float worldX, float worldY) const;
        bool IsExplored(RTSFaction faction, float worldX, float worldY) const;
        RTSVisibility GetVisibilityAtPosition(RTSFaction faction, float worldX, float worldY) const;

        // === Abilities ===
        void RevealArea(RTSFaction faction, float centerX, float centerY, float radius);
        void HideArea(RTSFaction faction, float centerX, float centerY, float radius);

        // === Queries ===
        int GetMapWidth() const;
        int GetMapHeight() const;
        float GetExploredPercent(RTSFaction faction) const;
        std::string GetFogStatusString() const;

      private:
        int WorldToGrid(float worldPos) const;

        Spark::IEngineContext* m_context{nullptr};

        std::unordered_map<RTSFaction, FogGrid> m_grids;
        int m_mapWidth = 128;
        int m_mapHeight = 128;
        static constexpr float CELL_SIZE = 1.0f; ///< World units per grid cell
    };

} // namespace RTS
