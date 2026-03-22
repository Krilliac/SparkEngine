/**
 * @file VolumeComponents.h
 * @brief ECS components for spatial volumes, probes, and placeable markers
 * @author Spark Engine Team
 * @date 2026
 *
 * These components allow volumes, probes, and markers to be placed as
 * entities in the editor and processed by their corresponding engine systems
 * (ProximityTriggerSystem, VolumeManager, AudioMixer, etc.).
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

// =============================================================================
// TriggerVolumeComponent — Proximity trigger (sphere/AABB)
// =============================================================================

/**
 * @brief Spatial trigger that fires events when entities enter or exit.
 *
 * Bridges to ProximityTriggerSystem at runtime. The system creates the
 * underlying trigger from this component's shape/size data on entity creation.
 */
struct TriggerVolumeComponent
{
    enum class Shape : uint8_t
    {
        Sphere,
        AABB
    };

    Shape shape = Shape::Sphere;
    float radius = 5.0f;                             ///< Sphere radius (Sphere mode).
    DirectX::XMFLOAT3 halfExtents{5.0f, 5.0f, 5.0f}; ///< Box half-extents (AABB mode).
    std::string onEnterEvent;                        ///< Script event name on enter.
    std::string onExitEvent;                         ///< Script event name on exit.
    bool enabled = true;                             ///< Active state.
    bool oneShot = false;                            ///< Fire once then disable.
    uint32_t runtimeTriggerID = 0;                   ///< ID assigned by ProximityTriggerSystem.

    bool Validate() const
    {
        ASSERT_MSG(radius > 0.0f, "TriggerVolume radius must be positive");
        return radius > 0.0f;
    }
};

// =============================================================================
// PostProcessVolumeComponent — Spatial post-processing overrides
// =============================================================================

/**
 * @brief Defines a region where post-processing parameters are overridden.
 *
 * Bridges to VolumeManager. Global volumes always apply; local volumes
 * blend based on camera distance from the AABB bounds.
 */
struct PostProcessVolumeComponent
{
    bool isGlobal = false;      ///< Global (always-active) or local (bounded).
    int priority = 0;           ///< Higher priority wins on overlap.
    float weight = 1.0f;        ///< Blend weight [0,1].
    float blendDistance = 2.0f; ///< Fade-in distance for local volumes.

    // Exposure
    bool overrideExposure = false;
    float exposure = 0.0f;
    float minEV = -2.0f;
    float maxEV = 14.0f;

    // Bloom
    bool overrideBloom = false;
    float bloomIntensity = 1.0f;
    float bloomThreshold = 0.9f;

    // Color Grading
    bool overrideColorGrading = false;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float temperature = 0.0f;

    // Fog
    bool overrideFog = false;
    float fogDensity = 0.01f;
    float fogHeight = 0.2f;
};

// =============================================================================
// ReflectionProbeComponent — Cubemap capture point
// =============================================================================

/**
 * @brief Defines a reflection probe capture point.
 *
 * Bridges to ReflectionProbeCache. The system captures a cubemap at the
 * entity's position and uses it for environment reflections within the
 * influence radius.
 */
struct ReflectionProbeComponent
{
    int resolution = 256;                              ///< Cubemap resolution.
    float influenceRadius = 10.0f;                     ///< Influence sphere radius.
    DirectX::XMFLOAT3 boxExtents{5.0f, 5.0f, 5.0f};    ///< Box projection extents.
    bool useBoxProjection = false;                     ///< Enable parallax correction.
    bool isDynamic = false;                            ///< Re-render each frame vs baked.
    float refreshInterval = 0.0f;                      ///< Dynamic refresh interval (0=every frame).
    int importance = 1;                                ///< Priority for probe blending.
    DirectX::XMFLOAT3 captureOffset{0.0f, 0.0f, 0.0f}; ///< Offset from entity position.

    bool Validate() const
    {
        ASSERT_MSG(resolution > 0 && (resolution & (resolution - 1)) == 0,
                   "ReflectionProbe resolution must be a power of 2");
        ASSERT_MSG(influenceRadius > 0.0f, "ReflectionProbe influenceRadius must be positive");
        return resolution > 0 && influenceRadius > 0.0f;
    }
};

// =============================================================================
// LightProbeComponent — SH-encoded indirect illumination sample point
// =============================================================================

/**
 * @brief Defines a light probe for indirect illumination sampling.
 *
 * Bridges to LightProbeSystem. Probes can be auto-placed in grids or
 * manually positioned. SH coefficients are either baked or computed at runtime.
 */
struct LightProbeComponent
{
    float influenceRadius = 10.0f;                   ///< Probe influence radius.
    DirectX::XMFLOAT3 gridSpacing{4.0f, 4.0f, 4.0f}; ///< Auto-placement grid spacing.
    bool baked = false;                              ///< Whether SH coefficients are baked.
    int shOrder = 2;                                 ///< Spherical harmonics order (1 or 2).

    bool Validate() const
    {
        ASSERT_MSG(influenceRadius > 0.0f, "LightProbe influenceRadius must be positive");
        ASSERT_MSG(shOrder >= 1 && shOrder <= 2, "LightProbe shOrder must be 1 or 2");
        return influenceRadius > 0.0f && shOrder >= 1 && shOrder <= 2;
    }
};

// =============================================================================
// NavObstacleComponent — Dynamic NavMesh obstacle
// =============================================================================

/**
 * @brief Marks an entity as a dynamic NavMesh obstacle.
 *
 * Bridges to NavMeshObstacles. The system carves the obstacle shape out of
 * the walkable NavMesh at runtime, preventing AI agents from pathfinding
 * through the obstacle.
 */
struct NavObstacleComponent
{
    enum class Shape : uint8_t
    {
        Box,
        Cylinder
    };

    Shape shape = Shape::Box;
    DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f}; ///< Box half-extents.
    float radius = 1.0f;                             ///< Cylinder radius.
    float height = 2.0f;                             ///< Cylinder height.
    bool carveOnMove = true;                         ///< Re-carve when entity moves.
    uint32_t runtimeObstacleID = 0;                  ///< ID assigned by NavMeshObstacles.

    bool Validate() const
    {
        ASSERT_MSG(radius > 0.0f, "NavObstacle radius must be positive");
        ASSERT_MSG(height > 0.0f, "NavObstacle height must be positive");
        return radius > 0.0f && height > 0.0f;
    }
};

// =============================================================================
// WaterPlaneComponent — Water surface with waves
// =============================================================================

/**
 * @brief Defines a water surface plane with Gerstner wave simulation.
 *
 * Bridges to WaterRenderer. The system renders the water surface with
 * reflection, refraction, and wave animation.
 */
struct WaterPlaneComponent
{
    DirectX::XMFLOAT2 size{100.0f, 100.0f};                 ///< Water surface size.
    DirectX::XMFLOAT4 shallowColor{0.2f, 0.5f, 0.6f, 0.8f}; ///< Shallow water color.
    DirectX::XMFLOAT4 deepColor{0.05f, 0.1f, 0.2f, 0.95f};  ///< Deep water color.
    float waveHeight = 0.3f;                                ///< Gerstner wave height.
    float waveSpeed = 1.0f;                                 ///< Wave animation speed.
    float waveFrequency = 0.5f;                             ///< Wave frequency.
    float reflectionStrength = 0.5f;                        ///< Reflection intensity [0,1].
    float refractionStrength = 0.3f;                        ///< Refraction intensity [0,1].
    bool receiveShadows = true;                             ///< Receive shadows from above.
};

// =============================================================================
// FogVolumeComponent — Local volumetric fog region
// =============================================================================

/**
 * @brief Defines a local fog volume for atmospheric effects.
 *
 * Bridges to FroxelVolumetricFog. The system applies local density overrides
 * within the AABB bounds, creating localized fog regions.
 */
struct FogVolumeComponent
{
    DirectX::XMFLOAT3 halfExtents{10.0f, 5.0f, 10.0f}; ///< Volume half-extents.
    float density = 0.05f;                             ///< Local fog density.
    DirectX::XMFLOAT4 color{0.7f, 0.7f, 0.7f, 1.0f};   ///< Fog color + opacity.
    float falloff = 1.0f;                              ///< Edge falloff exponent.
    float heightFalloff = 0.0f;                        ///< Height-based density falloff.
};

// =============================================================================
// LODGroupComponent — Distance-based level-of-detail switching
// =============================================================================

/**
 * @brief Groups LOD meshes and controls distance-based switching.
 *
 * Bridges to MeshLOD. The system selects the appropriate LOD level based
 * on camera distance and crossfades between levels.
 */
struct LODGroupComponent
{
    float lodDistances[4] = {10.0f, 25.0f, 50.0f, 100.0f}; ///< Distance thresholds per LOD level.
    int lodCount = 3;                                      ///< Number of LOD levels (1-4).
    float crossFadeDuration = 0.5f;                        ///< Crossfade transition time (seconds).
    bool autoGenerate = true;                              ///< Auto-generate LODs from highest detail mesh.
    int currentLOD = 0;                                    ///< Runtime: current active LOD level.

    bool Validate() const
    {
        ASSERT_MSG(lodCount >= 1 && lodCount <= 4, "LODGroup lodCount must be in [1, 4]");
        return lodCount >= 1 && lodCount <= 4;
    }
};

// =============================================================================
// SpawnPointComponent — Generic spawn point marker
// =============================================================================

/**
 * @brief Marks an entity position as a spawn point for gameplay entities.
 *
 * Used by game code to find spawn locations by tag. Supports team assignment,
 * random offset radius, respawn cooldowns, and concurrent spawn limits.
 */
struct SpawnPointComponent
{
    std::string spawnTag = "default"; ///< Tag for spawn group selection (e.g. "team_a", "boss").
    int teamID = 0;                   ///< Team assignment (0=neutral).
    float spawnRadius = 1.0f;         ///< Random offset radius around point.
    float respawnDelay = 5.0f;        ///< Delay before reuse (seconds).
    int maxConcurrent = -1;           ///< Max simultaneous spawns (-1=unlimited).
    bool enabled = true;              ///< Active state.
    int priority = 0;                 ///< Selection priority (higher = preferred).
    float lastUsedTime = -999.0f;     ///< Runtime: last time this point was used.
    int currentSpawnCount = 0;        ///< Runtime: current active spawns from this point.

    bool IsAvailable(float currentTime) const
    {
        if (!enabled)
            return false;
        if (currentTime - lastUsedTime < respawnDelay)
            return false;
        if (maxConcurrent >= 0 && currentSpawnCount >= maxConcurrent)
            return false;
        return true;
    }

    bool Validate() const
    {
        ASSERT_MSG(spawnRadius >= 0.0f, "SpawnPoint radius must be non-negative");
        ASSERT_MSG(respawnDelay >= 0.0f, "SpawnPoint respawnDelay must be non-negative");
        return spawnRadius >= 0.0f && respawnDelay >= 0.0f;
    }
};

// =============================================================================
// AudioReverbZoneComponent — Spatial audio reverb zone
// =============================================================================

/**
 * @brief Defines a spatial zone where audio reverb is applied.
 *
 * Bridges to AudioMixer::ReverbZone. Sounds playing within the zone have
 * reverb applied based on the preset and custom parameters. The effect
 * blends between inner (full) and outer (fade) radii.
 */
struct AudioReverbZoneComponent
{
    enum class Preset : uint8_t
    {
        Generic,
        Room,
        Hall,
        Cave,
        Arena,
        Forest,
        Underwater
    };

    float innerRadius = 5.0f;      ///< Full-effect radius.
    float outerRadius = 15.0f;     ///< Fade-out radius.
    Preset preset = Preset::Room;  ///< Reverb preset.
    float decayTime = 1.5f;        ///< Reverb decay time (seconds).
    float earlyReflections = 0.5f; ///< Early reflection level [0,1].
    float lateReverbLevel = 0.5f;  ///< Late reverb level [0,1].
    float diffusion = 1.0f;        ///< Reverb diffusion [0,1].
    float roomSize = 0.5f;         ///< Room size factor [0,1].
    bool enabled = true;           ///< Active state.

    bool Validate() const
    {
        ASSERT_MSG(innerRadius > 0.0f, "ReverbZone innerRadius must be positive");
        ASSERT_MSG(outerRadius >= innerRadius, "ReverbZone outerRadius must be >= innerRadius");
        ASSERT_MSG(decayTime > 0.0f, "ReverbZone decayTime must be positive");
        return innerRadius > 0.0f && outerRadius >= innerRadius && decayTime > 0.0f;
    }
};
