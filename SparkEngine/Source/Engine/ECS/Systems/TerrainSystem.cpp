/**
 * @file TerrainSystem.cpp
 * @brief Implementation of the TerrainSystem ECS system
 *
 * Wires TerrainComponent entities to the ClipmapTerrain LOD system.
 * Each frame: selects LOD per terrain, feeds heightmap data to the
 * clipmap system, and triggers mesh rebuilds when terrain is dirty.
 */

#include "../../../Core/Platform.h"
#include "TerrainSystem.h"
#include "../../../Graphics/ClipmapTerrain.h"
#include "../../../Utils/LogMacros.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace Spark::ECS
{

    void TerrainSystem::Update(World& world, float /*deltaTime*/)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        m_activeTerrainCount = 0;

        auto& clipmap = Spark::Graphics::ClipmapTerrain::GetInstance();

        // Feed camera position to the clipmap system so it can update LOD centers
        clipmap.Update(m_cameraPosition);

        auto view = world.GetEntitiesWith<TerrainComponent, Transform>();
        SPARK_LOG_TRACE(Spark::LogCategory::ECS, "TerrainSystem updating %zu terrain entities",
                        static_cast<size_t>(view.size_hint()));

        for (auto entity : view)
        {
            auto& terrain = view.get<TerrainComponent>(entity);
            auto& transform = view.get<Transform>(entity);

            // Skip invisible terrains
            if (!terrain.visible)
                continue;

            auto* active = world.GetComponent<ActiveComponent>(entity);
            if (active && !active->active)
                continue;

            ++m_activeTerrainCount;

            // LOD selection based on camera distance to terrain center
            float dx = m_cameraPosition.x - transform.position.x;
            float dy = m_cameraPosition.y - transform.position.y;
            float dz = m_cameraPosition.z - transform.position.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            float baseDist = terrain.terrainSize * terrain.lodBias;
            int selectedLOD = 0;
            float lodDist = baseDist;
            for (int i = 0; i < terrain.lodLevels; ++i)
            {
                if (distSq > lodDist * lodDist)
                    selectedLOD = i + 1;
                lodDist *= 2.0f;
            }
            selectedLOD = std::min(selectedLOD, terrain.lodLevels - 1);
            terrain.selectedLOD = static_cast<uint32_t>(selectedLOD);

            // If the terrain has heightmap data and is dirty, feed it to the clipmap system
            if (terrain.dirty && !terrain.heightmap.empty())
            {
                int res = terrain.heightmapResolution;
                clipmap.SetHeightmapData(terrain.heightmap, res, res);
                terrain.dirty = false;

                SPARK_LOG_DEBUG(Spark::LogCategory::ECS, "TerrainSystem: heightmap uploaded to clipmap (%dx%d)", res,
                                res);
            }

            // Rebuild clipmap meshes for levels that moved
            for (int level = 0; level < clipmap.GetLevelCount(); ++level)
            {
                const auto& lvl = clipmap.GetLevel(level);
                if (lvl.needsUpdate)
                {
                    std::vector<float> vertices;
                    std::vector<uint32_t> indices;
                    clipmap.GenerateMesh(level, vertices, indices);
                }
            }
        }

        SPARK_LOG_TRACE(Spark::LogCategory::ECS, "TerrainSystem active terrains: %d", m_activeTerrainCount);
    }

} // namespace Spark::ECS
