/**
 * @file TerrainSystem.cpp
 * @brief Implementation of the TerrainSystem ECS system
 */

#include "../../../Core/Platform.h"
#include "TerrainSystem.h"
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

        auto view = world.GetEntitiesWith<TerrainComponent, Transform>();
        for (auto entity : view)
        {
            auto& terrain = view.get<TerrainComponent>(entity);
            auto& transform = view.get<Transform>(entity);

            // Skip invisible terrains
            if (!terrain.visible)
                continue;

            // Check if entity is active
            auto* active = world.GetComponent<ActiveComponent>(entity);
            if (active && !active->active)
                continue;

            ++m_activeTerrainCount;

            // LOD selection based on camera distance to terrain center
            float dx = m_cameraPosition.x - transform.position.x;
            float dy = m_cameraPosition.y - transform.position.y;
            float dz = m_cameraPosition.z - transform.position.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            // Select LOD level: each level doubles the transition distance
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

            // Clamp heightmap values to terrain bounds
            for (auto& h : terrain.heightmap)
            {
                h = std::clamp(h, terrain.minHeight, terrain.maxHeight);
            }
        }

        SPARK_LOG_EVERY_N(Spark::LogLevel::Trace, Spark::LogCategory::ECS, 600, "TerrainSystem: %d active terrains",
                          m_activeTerrainCount);
    }

} // namespace Spark::ECS
