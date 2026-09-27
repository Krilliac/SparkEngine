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
        /// Reveal the disc of @p visionRange around the unit. Iteration is clipped to the grid, and a
        /// non-finite or negative range is ignored, so a restored save cannot turn this into an unbounded loop.
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
        /** @return The faction's visibility grid, or nullptr before Initialize. */
        const FogGrid* GetGrid(RTSFaction faction) const;
        std::string GetFogStatusString() const;

        /**
         * @brief Replace every faction's grid (explored history included) from a persistence snapshot.
         * @param grids  One grid per faction, indexed by RTSFaction, all sharing dimensions in
         *               [1, MAX_MAP_DIMENSION] with width * height cells of valid RTSVisibility values.
         * @return false (leaving state untouched) if any grid is malformed.
         */
        bool RestoreState(const std::vector<FogGrid>& grids);

        static constexpr int MAX_MAP_DIMENSION = 1024;

      private:
        /// Inclusive grid-cell rectangle; empty when minX > maxX or minY > maxY.
        struct CellRect
        {
            int minX = 0;
            int minY = 0;
            int maxX = -1;
            int maxY = -1;
        };

        /**
         * Bounding box of the disc of @p radius world units around a grid cell, clipped to @p grid. A
         * non-finite or negative radius yields an empty rectangle, and a radius larger than the grid is
         * clamped before any int conversion, so the reveal/hide loops cost at most width * height cells.
         */
        static CellRect ClipDisc(const FogGrid& grid, int centerX, int centerY, float radius);

        /// Saturating world-to-grid conversion; NaN and far-off-grid positions map outside the grid.
        int WorldToGrid(float worldPos) const;

        Spark::IEngineContext* m_context{nullptr};

        std::unordered_map<RTSFaction, FogGrid> m_grids;
        int m_mapWidth = 128;
        int m_mapHeight = 128;
        static constexpr float CELL_SIZE = 1.0f; ///< World units per grid cell
    };

} // namespace RTS
