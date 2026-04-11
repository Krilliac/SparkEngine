/**
 * @file FoliageSystem.h
 * @brief Instanced vegetation placement and rendering system
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages procedural vegetation within scatter volumes. A species
 * registry holds mesh/material/density/wind/LOD parameters; volumes
 * reference species by name and scatter instances deterministically
 * from a per-volume seed. Terrain queries come from ClipmapTerrain so
 * that slope and altitude filters can reject unsuitable positions.
 *
 * Instance data is stored CPU-side and exposed via a read-only query
 * API so the render path can upload it to a GPUSceneBuffer each frame.
 * Distance culling against the camera position is applied each Update.
 *
 * @threadsafety Main thread only.
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class World;

namespace Spark::Graphics
{

    // ========================================================================
    // FoliageSpecies
    // ========================================================================

    /**
     * @brief A single vegetation species registered with the FoliageManager.
     *
     * Identified by a string name used by volumes to reference the species.
     * Density controls instances per square meter; slope and altitude
     * constraints filter generated positions against the terrain.
     */
    struct FoliageSpecies
    {
        std::string name;               ///< Unique species name (lookup key).
        std::string meshPath;           ///< Mesh asset path (for the renderer).
        std::string materialPath;       ///< Material asset path (descriptive; not currently resolved).
        std::string albedoTexturePath;  ///< Albedo texture for the mesh sub-pass. Phase F: loaded
                                        ///< lazily via TextureSystem and cached on the renderer.
                                        ///< Empty string falls back to the engine's 1x1 white SRV.
        float density = 1.0f;           ///< Instances per square meter of volume XZ area.
        float minScale = 0.8f;          ///< Minimum uniform scale.
        float maxScale = 1.2f;          ///< Maximum uniform scale.
        float minSlopeAngleDeg = 0.0f;  ///< Minimum terrain slope for placement (degrees).
        float maxSlopeAngleDeg = 45.0f; ///< Maximum terrain slope for placement (degrees).
        float minAltitude = -10000.0f;  ///< Minimum placement altitude (world Y).
        float maxAltitude = 10000.0f;   ///< Maximum placement altitude (world Y).
        bool alignToSurface = true;     ///< Align instance Y-axis to terrain normal.
        bool randomYaw = true;          ///< Random rotation around Y axis.
        bool castShadows = true;        ///< Instances cast shadows.
        float windInfluence = 1.0f;     ///< Wind sway multiplier [0,2].
        float cullDistance = 100.0f;    ///< Distance (meters) beyond which instances are culled.
        float billboardHeight = 2.0f;   ///< Impostor billboard vertical size in metres. Phase F:
                                        ///< packed into the atlas cell meta buffer so the VS can
                                        ///< scale per-species billboards without a shader rebuild.
        float billboardAspect = 0.5f;   ///< Impostor billboard width/height ratio. Phase G:
                                        ///< `halfWidth = height * aspect`. Default 0.5 keeps the
                                        ///< Phase E/F appearance (tree-like, taller than wide).
                                        ///< Increase for bushes / grass clumps.
    };

    // ========================================================================
    // FoliageVolumeDesc
    // ========================================================================

    /**
     * @brief Configuration for creating a foliage volume.
     *
     * The volume is an axis-aligned box in world space. Scatter happens
     * on the XZ plane; Y is resolved per instance via ClipmapTerrain
     * height sampling (with a hard altitude filter from species).
     */
    struct FoliageVolumeDesc
    {
        XMFLOAT3 center{0.0f, 0.0f, 0.0f};         ///< Volume center in world space.
        XMFLOAT3 halfExtents{50.0f, 50.0f, 50.0f}; ///< Half-extents (XZ used for area, Y used for altitude clamp).
        std::vector<std::string> speciesNames;     ///< Species to scatter in this volume.
        uint32_t seed = 0;                         ///< Seed for deterministic scatter.
        float densityScale = 1.0f;                 ///< Multiplier applied on top of each species' density.
        float minSlopeAngleDeg = 0.0f;             ///< Volume-level slope override (merged with species).
        float maxSlopeAngleDeg = 90.0f;            ///< Volume-level slope override (merged with species).
        float minAltitude = -10000.0f;             ///< Volume-level altitude floor.
        float maxAltitude = 10000.0f;              ///< Volume-level altitude ceiling.
        bool alignToTerrain = true;                ///< Sample terrain height for Y.
        bool enabled = true;                       ///< Inactive volumes generate no instances.
    };

    // ========================================================================
    // FoliageInstance
    // ========================================================================

    /**
     * @brief One scattered vegetation instance.
     *
     * Produced by FoliageManager::GenerateInstances and consumed by the
     * render path (or any other consumer) via GetInstances/GetAllInstances.
     */
    struct FoliageInstance
    {
        XMFLOAT3 position{0.0f, 0.0f, 0.0f}; ///< World-space position.
        XMFLOAT3 normal{0.0f, 1.0f, 0.0f};   ///< Surface normal at the position.
        float yawRadians = 0.0f;             ///< Rotation around the Y axis.
        float scale = 1.0f;                  ///< Uniform scale.
        uint32_t speciesIndex = 0;           ///< Index into the volume's speciesNames list.
        float distanceToCamera = 0.0f;       ///< Distance to camera (updated each frame).
        bool visible = true;                 ///< False if culled this frame.
    };

    // ========================================================================
    // FoliageManager
    // ========================================================================

    /**
     * @brief Singleton manager of foliage species and volumes.
     *
     * Lifecycle: Initialize() once at engine start, UpdateFromECS() once per
     * frame (processes FoliageVolumeComponent entities), Update() once per
     * frame (distance culling), Shutdown() at engine end.
     */
    class FoliageManager
    {
      public:
        /// @brief Get the singleton instance.
        static FoliageManager& GetInstance();

        /// @brief Initialize the manager. Clears all state.
        void Initialize();

        /// @brief Release all species, volumes and instances.
        void Shutdown();

        /// @brief True after Initialize() and before Shutdown().
        bool IsInitialized() const { return m_initialized; }

        // ----- Species registry -----

        /// @brief Register (or overwrite) a species by name.
        void RegisterSpecies(const FoliageSpecies& species);

        /// @brief Look up a species by name. Returns nullptr if missing.
        const FoliageSpecies* FindSpecies(const std::string& name) const;

        /// @brief Number of registered species.
        uint32_t GetSpeciesCount() const { return static_cast<uint32_t>(m_species.size()); }

        /// @brief Unregister a species by name. Instances referencing it remain
        /// but their species index becomes stale; call RemoveVolume if needed.
        void UnregisterSpecies(const std::string& name);

        /// @brief Return the global (registry-wide) index of a species by
        /// name, or UINT32_MAX if the species is not registered. Used by
        /// the render consumer to derive a stable per-species material ID
        /// that does not depend on any volume's local species ordering.
        uint32_t GetSpeciesGlobalIndex(const std::string& name) const;

        /// @brief Look up a species by its global index. Returns nullptr if
        /// the index is out of range. The pointer is invalidated by any
        /// subsequent RegisterSpecies/UnregisterSpecies call.
        ///
        /// Used by the foliage impostor baker to walk the registry without
        /// going through the by-name lookup for every entry.
        const FoliageSpecies* GetSpeciesByGlobalIndex(uint32_t index) const;

        // ----- Volume management -----

        /**
         * @brief Create a foliage volume and scatter instances inside it.
         * Queries ClipmapTerrain for height and slope and filters per
         * species and per-volume constraints. Deterministic in the seed.
         * @return A non-zero volume ID on success, 0 on failure (no species).
         */
        uint32_t AddVolume(const FoliageVolumeDesc& desc);

        /// @brief Remove a volume and its instances. No-op if the ID is unknown.
        void RemoveVolume(uint32_t volumeId);

        /// @brief Number of active volumes.
        uint32_t GetVolumeCount() const { return static_cast<uint32_t>(m_volumes.size()); }

        /// @brief True if the volume ID is currently registered.
        bool HasVolume(uint32_t volumeId) const;

        /// @brief List the IDs of all currently-registered volumes, in
        /// creation order. Empty if none. Used by the render consumer to
        /// iterate volumes without knowing the internal ID range.
        std::vector<uint32_t> GetVolumeIds() const;

        /// @brief Lookup a volume's descriptor by ID. Returns nullptr if
        /// the volume does not exist. The returned pointer is invalidated
        /// by any subsequent Add/Remove call.
        const FoliageVolumeDesc* GetVolumeDesc(uint32_t volumeId) const;

        // ----- Instance queries -----

        /// @brief Immutable view of the instances for a specific volume.
        /// Returns an empty vector if the volume does not exist.
        const std::vector<FoliageInstance>& GetInstances(uint32_t volumeId) const;

        /// @brief Total number of scattered instances across all volumes.
        uint32_t GetTotalInstanceCount() const;

        /// @brief Number of instances visible after the last distance cull.
        uint32_t GetVisibleInstanceCount() const { return m_visibleInstances; }

        // ----- Per-frame updates -----

        /**
         * @brief Update distance culling for all instances.
         * Should be called once per frame with the current camera world
         * position. Marks each instance visible/invisible based on its
         * species cullDistance.
         */
        void Update(float deltaTime, const XMFLOAT3& cameraPosition);

        /**
         * @brief Process FoliageVolumeComponent entities in the ECS world.
         * Creates a volume for any component that has not yet been assigned
         * a runtimeVolumeId, using the entity's Transform position as the
         * volume center. Removes volumes for components that no longer exist.
         *
         * Called once per frame from the engine lifecycle. Reads the world
         * registry; requires a ::World reference.
         */
        void UpdateFromECS(::World& world, float deltaTime);

        // ----- Debug / diagnostics -----

        /// @brief Human-readable one-line status for console display.
        std::string Console_GetStatus() const;

      private:
        FoliageManager() = default;
        FoliageManager(const FoliageManager&) = delete;
        FoliageManager& operator=(const FoliageManager&) = delete;

        struct Volume
        {
            uint32_t id = 0;
            FoliageVolumeDesc desc{};
            std::vector<FoliageInstance> instances;
        };

        // Scatter logic (pure CPU, deterministic in seed)
        void GenerateInstancesForVolume(Volume& volume);

        std::vector<FoliageSpecies> m_species;
        std::unordered_map<std::string, uint32_t> m_speciesByName;
        std::vector<Volume> m_volumes;
        uint32_t m_nextVolumeId = 1;
        uint32_t m_visibleInstances = 0;
        bool m_initialized = false;

        static const std::vector<FoliageInstance> s_emptyInstances;
    };

} // namespace Spark::Graphics
