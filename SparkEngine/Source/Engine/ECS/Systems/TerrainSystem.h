/**
 * @file TerrainSystem.h
 * @brief ECS system that processes TerrainComponent entities each frame
 */

#pragma once

#include "ECSystems.h"
#include "../Components/TerrainComponents.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Spark::ECS
{

    /**
     * @class TerrainSystem
     * @brief Updates terrain LOD and marks dirty terrains for mesh/collision rebuild.
     *
     * Each frame, iterates entities with TerrainComponent + Transform:
     * 1. Selects LOD level based on camera-to-terrain distance.
     * 2. Clamps heightmap data to configured min/max height bounds.
     *
     * ### Execution order
     * Should run AFTER PhysicsUpdateSystem (so terrain collision reads updated positions)
     * and BEFORE RenderSystem (so LOD selection is ready for draw submission).
     */
    class TerrainSystem : public ISystem
    {
      public:
        void Update(World& world, float deltaTime) override;
        const char* GetName() const override { return "TerrainSystem"; }

        int GetActiveTerrainCount() const { return m_activeTerrainCount; }

        /// Set the camera position used for LOD selection (updated by the player/camera system).
        void SetCameraPosition(const XMFLOAT3& pos) { m_cameraPosition = pos; }

      private:
        int m_activeTerrainCount = 0;
        XMFLOAT3 m_cameraPosition = {0, 0, 0};
        /// World the attempt map below belongs to. Entity ids restart from zero in
        /// a freshly created World, so a map carried across a scene reload would
        /// suppress the new scene's terrain loads.
        const World* m_assetLoadWorld = nullptr;
        /// Entity -> the .sparkterrain path already attempted for it. Keyed by the
        /// asset as well as the entity so a recycled id (or a re-authored path) is
        /// retried instead of being mistaken for an attempt that already happened.
        std::unordered_map<uint32_t, std::string> m_assetLoadAttempted;
    };

} // namespace Spark::ECS
