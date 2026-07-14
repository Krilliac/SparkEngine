/**
 * @file ArenaBuilder.h
 * @brief Procedural arena construction with multi-room layouts
 *
 * Builds a complete game arena with cover walls, corridors, sniper
 * nests, platforms, ramps, and environmental variety. Replaces the
 * basic test scene with a fully playable combat map.
 */

#pragma once
#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#include <d3d11.h>
#endif

#include <vector>
#include <string>
#include <memory>

class Game;
class GraphicsEngine;

namespace Spark
{

    /**
     * @brief Builds the combat arena geometry
     *
     * Creates a multi-room arena suitable for wave-based combat:
     * - Central arena with open sightlines
     * - Cover walls and pillars for tactical play
     * - Elevated sniper platforms
     * - Side corridors for flanking
     * - Ramps connecting elevation levels
     * - Cover crates and barricades
     */
    class ArenaBuilder
    {
      public:
        /**
         * @brief Build the complete arena into the game's object list
         * @param game Game instance to spawn objects into
         * @param graphics Graphics engine for D3D device access
         */
        static void BuildArena(Game* game, GraphicsEngine* graphics);

      private:
        /// @brief Create the arena floor sections
        static void BuildFloor(Game* game, GraphicsEngine* graphics);

        /// @brief Create perimeter walls
        static void BuildWalls(Game* game, GraphicsEngine* graphics);

        /// @brief Create cover structures (pillars, half-walls, crates)
        static void BuildCover(Game* game, GraphicsEngine* graphics);

        /// @brief Create elevated platforms and sniper nests
        static void BuildPlatforms(Game* game, GraphicsEngine* graphics);

        /// @brief Create connecting ramps between elevation levels
        static void BuildRamps(Game* game, GraphicsEngine* graphics);

        /// @brief Create side corridors and rooms
        static void BuildCorridors(Game* game, GraphicsEngine* graphics);
    };

} // namespace Spark
