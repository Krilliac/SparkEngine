/**
 * @file TerrainSystem.cpp
 * @brief Implementation of the TerrainSystem ECS system
 *
 * Wires TerrainComponent entities to the ClipmapTerrain LOD system.
 * Each frame: selects LOD per terrain and feeds heightmap data to the
 * clipmap system when terrain is dirty.
 */

#include "TerrainSystem.h"
#include "../../../Core/Platform.h"
#include "../../../Graphics/ClipmapTerrain.h"
#include "../../../Graphics/TerrainRenderer.h"
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

        // A new World restarts entity ids at zero, so attempts recorded against the
        // previous one would silently veto this scene's terrain loads.
        if (m_assetLoadWorld != &world)
        {
            m_assetLoadAttempted.clear();
            m_assetLoadWorld = &world;
        }
        else
        {
            // Bound the map to entities that still exist: a destroyed terrain must
            // not keep an entry alive for the rest of the process.
            const auto& registry = world.GetRegistry();
            std::erase_if(m_assetLoadAttempted, [&registry](const auto& attempt)
                          { return !registry.valid(static_cast<EntityID>(attempt.first)); });
        }

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

            // An authored .sparkterrain asset with no heightmap yet: load it once per
            // entity *and* asset, so a missing or corrupt file cannot spam the log
            // every frame while a reloaded scene (or a re-authored path) still loads.
            if (terrain.heightmap.empty() && !terrain.terrainAssetPath.empty())
            {
                auto [attempt, inserted] =
                    m_assetLoadAttempted.try_emplace(static_cast<uint32_t>(entity), terrain.terrainAssetPath);
                if (inserted || attempt->second != terrain.terrainAssetPath)
                {
                    attempt->second = terrain.terrainAssetPath;
                    Spark::Graphics::TerrainRenderer::LoadSparkTerrain(terrain.terrainAssetPath, terrain);
                }
            }

            // If the terrain has heightmap data and is dirty, feed it to the clipmap system
            if (terrain.dirty && !terrain.heightmap.empty())
            {
                int res = terrain.heightmapResolution;
                clipmap.SetHeightmapData(terrain.heightmap, res, res);
                terrain.dirty = false;

                SPARK_LOG_DEBUG(Spark::LogCategory::ECS, "TerrainSystem: heightmap uploaded to clipmap (%dx%d)", res,
                                res);
            }

            // Note: GPU mesh rebuilds for dirty clipmap levels (needsUpdate) are the
            // renderer's responsibility via ClipmapTerrain::UpdateGPUMesh; generating
            // mesh data here without a consumer would be discarded work.
        }

        SPARK_LOG_TRACE(Spark::LogCategory::ECS, "TerrainSystem active terrains: %d", m_activeTerrainCount);
    }

} // namespace Spark::ECS
