/**
 * @file FoliageSystem.h
 * @brief Instanced vegetation placement and rendering system
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages procedural and painted vegetation within scatter volumes.
 * Each foliage volume defines bounds, density, species, and terrain
 * constraints. The system generates instance transforms and renders
 * them via GPU instancing for optimal performance.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief A single vegetation species/type within a foliage volume.
     */
    struct FoliageSpecies
    {
        std::string meshPath;         ///< Mesh asset for this species.
        std::string materialPath;     ///< Material asset.
        float density = 1.0f;         ///< Instances per square meter.
        float minScale = 0.8f;        ///< Minimum random scale.
        float maxScale = 1.2f;        ///< Maximum random scale.
        float minSlopeAngle = 0.0f;   ///< Min terrain slope for placement (degrees).
        float maxSlopeAngle = 45.0f;  ///< Max terrain slope for placement (degrees).
        float minAltitude = -1000.0f; ///< Min placement altitude.
        float maxAltitude = 1000.0f;  ///< Max placement altitude.
        bool alignToSurface = true;   ///< Align to terrain normal.
        bool randomRotation = true;   ///< Random Y-axis rotation.
        bool castShadows = true;      ///< Instances cast shadows.
        float windInfluence = 1.0f;   ///< Wind sway multiplier [0,2].
        float cullDistance = 100.0f;  ///< Distance to cull instances.
    };

    /**
     * @brief Defines a region where vegetation is procedurally placed.
     */
    struct FoliageVolumeData
    {
        XMFLOAT3 halfExtents{50.0f, 50.0f, 50.0f}; ///< Volume half-extents.
        int seed = 0;                              ///< Random seed for reproducibility.
        float globalDensityScale = 1.0f;           ///< Global density multiplier.
        bool enabled = true;                       ///< Active state.

        // Species are stored separately in the component's species list
    };

    /**
     * @brief Manages foliage volumes and instance generation.
     */
    class FoliageManager
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<FoliageManager>() instead")]]
        static FoliageManager& GetInstance()
        {
            static FoliageManager s;
            return s;
        }

        void Initialize()
        {
            m_volumes.clear();
            m_initialized = true;
        }

        void Shutdown()
        {
            m_volumes.clear();
            m_initialized = false;
        }

        /**
         * @brief Register a foliage volume for instance generation.
         * @return Volume ID.
         */
        uint32_t AddVolume(const XMFLOAT3& center, const XMFLOAT3& halfExtents)
        {
            uint32_t id = m_nextId++;
            VolumeEntry entry;
            entry.id = id;
            entry.center = center;
            entry.halfExtents = halfExtents;
            m_volumes.push_back(entry);
            return id;
        }

        void RemoveVolume(uint32_t volumeId)
        {
            m_volumes.erase(std::remove_if(m_volumes.begin(), m_volumes.end(),
                                           [volumeId](const VolumeEntry& v) { return v.id == volumeId; }),
                            m_volumes.end());
        }

        uint32_t GetVolumeCount() const { return static_cast<uint32_t>(m_volumes.size()); }
        bool IsInitialized() const { return m_initialized; }

      private:
        FoliageManager() = default;

        struct VolumeEntry
        {
            uint32_t id = 0;
            XMFLOAT3 center{0, 0, 0};
            XMFLOAT3 halfExtents{50, 50, 50};
        };

        std::vector<VolumeEntry> m_volumes;
        uint32_t m_nextId = 1;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
