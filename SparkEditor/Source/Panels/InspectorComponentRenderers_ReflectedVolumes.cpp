/**
 * @file InspectorComponentRenderers_ReflectedVolumes.cpp
 * @brief Reflection-driven inspector renderers for volume, probe, placement, and audio components
 *
 * Split from InspectorComponentRenderers_Reflected.cpp. Contains:
 *   Volumes: TriggerVolume, PostProcessVolume, FogVolume
 *   Probes:  ReflectionProbe, LightProbe
 *   Placement: NavObstacle, WaterPlane, LODGroup, SpawnPoint, WindZone, Billboard
 *   Audio:   AudioReverbZone, AudioListener
 *   Misc:    CharacterController, Skybox
 */

#include "InspectorComponentRenderers_ReflectedInternal.h"

namespace SparkEditor
{

    // ============================================================================
    // Trigger Volume
    // ============================================================================

    void InspectorPanel::RenderTriggerVolumeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::TRIGGER_VOLUME, ICON_FA_VECTOR_SQUARE, "Trigger Volume",
                                   TriggerVolumeData, FIELD_INT(TriggerVolumeData, shape, "Shape (0=Sphere,1=AABB)"),
                                   FIELD_FLOAT(TriggerVolumeData, radius, "Radius"),
                                   FIELD_VEC3(TriggerVolumeData, halfExtents, "Half Extents"),
                                   FIELD_STRING(TriggerVolumeData, onEnterEvent, "On Enter Event"),
                                   FIELD_STRING(TriggerVolumeData, onExitEvent, "On Exit Event"),
                                   FIELD_BOOL(TriggerVolumeData, enabled, "Enabled"),
                                   FIELD_BOOL(TriggerVolumeData, oneShot, "One Shot"));
    }

    // ============================================================================
    // Post-Process Volume
    // ============================================================================

    void InspectorPanel::RenderPostProcessVolumeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::POST_PROCESS_VOLUME, ICON_FA_ADJUST, "Post-Process Volume",
                                   PostProcessVolumeData, FIELD_BOOL(PostProcessVolumeData, isGlobal, "Is Global"),
                                   FIELD_INT(PostProcessVolumeData, priority, "Priority"),
                                   FIELD_FLOAT_RANGE(PostProcessVolumeData, weight, "Weight", 0.0f, 1.0f),
                                   FIELD_FLOAT(PostProcessVolumeData, blendDistance, "Blend Distance"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideExposure, "Override Exposure"),
                                   FIELD_FLOAT(PostProcessVolumeData, exposure, "Exposure"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideBloom, "Override Bloom"),
                                   FIELD_FLOAT(PostProcessVolumeData, bloomIntensity, "Bloom Intensity"),
                                   FIELD_FLOAT(PostProcessVolumeData, bloomThreshold, "Bloom Threshold"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideColorGrading, "Override Color Grading"),
                                   FIELD_FLOAT_RANGE(PostProcessVolumeData, saturation, "Saturation", 0.0f, 2.0f),
                                   FIELD_FLOAT_RANGE(PostProcessVolumeData, contrast, "Contrast", 0.0f, 2.0f),
                                   FIELD_FLOAT(PostProcessVolumeData, temperature, "Temperature"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideFog, "Override Fog"),
                                   FIELD_FLOAT(PostProcessVolumeData, fogDensity, "Fog Density"));
    }

    // ============================================================================
    // Reflection Probe
    // ============================================================================

    void InspectorPanel::RenderReflectionProbeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::REFLECTION_PROBE, ICON_FA_GLOBE, "Reflection Probe",
                                   ReflectionProbeData, FIELD_INT(ReflectionProbeData, resolution, "Resolution"),
                                   FIELD_FLOAT(ReflectionProbeData, influenceRadius, "Influence Radius"),
                                   FIELD_VEC3(ReflectionProbeData, boxExtents, "Box Extents"),
                                   FIELD_BOOL(ReflectionProbeData, useBoxProjection, "Use Box Projection"),
                                   FIELD_BOOL(ReflectionProbeData, isDynamic, "Is Dynamic"),
                                   FIELD_FLOAT(ReflectionProbeData, refreshInterval, "Refresh Interval"),
                                   FIELD_INT(ReflectionProbeData, importance, "Importance"),
                                   FIELD_VEC3(ReflectionProbeData, captureOffset, "Capture Offset"));
    }

    // ============================================================================
    // Light Probe
    // ============================================================================

    void InspectorPanel::RenderLightProbeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::LIGHT_PROBE, ICON_FA_SUN, "Light Probe", LightProbeData,
                                   FIELD_FLOAT(LightProbeData, influenceRadius, "Influence Radius"),
                                   FIELD_VEC3(LightProbeData, gridSpacing, "Grid Spacing"),
                                   FIELD_BOOL(LightProbeData, baked, "Baked"),
                                   FIELD_INT(LightProbeData, shOrder, "SH Order"));
    }

    // ============================================================================
    // Nav Obstacle
    // ============================================================================

    void InspectorPanel::RenderNavObstacleComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::NAV_OBSTACLE, ICON_FA_ROUTE, "Nav Obstacle", NavObstacleData,
            FIELD_INT(NavObstacleData, shape, "Shape (0=Box,1=Cylinder)"),
            FIELD_VEC3(NavObstacleData, halfExtents, "Half Extents"), FIELD_FLOAT(NavObstacleData, radius, "Radius"),
            FIELD_FLOAT(NavObstacleData, height, "Height"), FIELD_BOOL(NavObstacleData, carveOnMove, "Carve On Move"));
    }

    // ============================================================================
    // Water Plane
    // ============================================================================

    void InspectorPanel::RenderWaterPlaneComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::WATER_PLANE, ICON_FA_WATER, "Water Plane", WaterPlaneData,
            FIELD_FLOAT(WaterPlaneData, waveHeight, "Wave Height"),
            FIELD_FLOAT(WaterPlaneData, waveSpeed, "Wave Speed"),
            FIELD_FLOAT(WaterPlaneData, waveFrequency, "Wave Frequency"),
            FIELD_FLOAT_RANGE(WaterPlaneData, reflectionStrength, "Reflection Strength", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(WaterPlaneData, refractionStrength, "Refraction Strength", 0.0f, 1.0f),
            FIELD_BOOL(WaterPlaneData, receiveShadows, "Receive Shadows"));
    }

    // ============================================================================
    // Fog Volume
    // ============================================================================

    void InspectorPanel::RenderFogVolumeComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::FOG_VOLUME, ICON_FA_SMOG, "Fog Volume", FogVolumeData,
            FIELD_VEC3(FogVolumeData, halfExtents, "Half Extents"), FIELD_FLOAT(FogVolumeData, density, "Density"),
            FIELD_VEC4(FogVolumeData, color, "Color"), FIELD_FLOAT(FogVolumeData, falloff, "Falloff"),
            FIELD_FLOAT(FogVolumeData, heightFalloff, "Height Falloff"));
    }

    // ============================================================================
    // LOD Group
    // ============================================================================

    void InspectorPanel::RenderLODGroupComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::LOD_GROUP, ICON_FA_LAYER_GROUP, "LOD Group", LODGroupData,
                                   FIELD_FLOAT(LODGroupData, lodDistance0, "LOD 0 Distance"),
                                   FIELD_FLOAT(LODGroupData, lodDistance1, "LOD 1 Distance"),
                                   FIELD_FLOAT(LODGroupData, lodDistance2, "LOD 2 Distance"),
                                   FIELD_FLOAT(LODGroupData, lodDistance3, "LOD 3 Distance"),
                                   FIELD_INT(LODGroupData, lodCount, "LOD Count"),
                                   FIELD_FLOAT(LODGroupData, crossFadeDuration, "Cross Fade Duration"),
                                   FIELD_BOOL(LODGroupData, autoGenerate, "Auto Generate"));
    }

    // ============================================================================
    // Spawn Point
    // ============================================================================

    void InspectorPanel::RenderSpawnPointComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::SPAWN_POINT, ICON_FA_LOCATION_ARROW, "Spawn Point", SpawnPointData,
            FIELD_STRING(SpawnPointData, spawnTag, "Spawn Tag"), FIELD_INT(SpawnPointData, teamID, "Team ID"),
            FIELD_FLOAT(SpawnPointData, spawnRadius, "Spawn Radius"),
            FIELD_FLOAT(SpawnPointData, respawnDelay, "Respawn Delay"),
            FIELD_INT(SpawnPointData, maxConcurrent, "Max Concurrent"), FIELD_BOOL(SpawnPointData, enabled, "Enabled"),
            FIELD_INT(SpawnPointData, priority, "Priority"));
    }

    // ============================================================================
    // Audio Reverb Zone
    // ============================================================================

    void InspectorPanel::RenderAudioReverbZoneComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::AUDIO_REVERB_ZONE, ICON_FA_VOLUME_UP, "Audio Reverb Zone", AudioReverbZoneData,
            FIELD_FLOAT(AudioReverbZoneData, innerRadius, "Inner Radius"),
            FIELD_FLOAT(AudioReverbZoneData, outerRadius, "Outer Radius"),
            FIELD_INT(AudioReverbZoneData, reverbPreset, "Preset (0-6)"),
            FIELD_FLOAT(AudioReverbZoneData, decayTime, "Decay Time"),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, earlyReflections, "Early Reflections", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, lateReverbLevel, "Late Reverb Level", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, diffusion, "Diffusion", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, roomSize, "Room Size", 0.0f, 1.0f));
    }

    // ============================================================================
    // Wind Zone
    // ============================================================================

    void InspectorPanel::RenderWindZoneComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::WIND_ZONE, ICON_FA_WIND, "Wind Zone", WindZoneData,
            FIELD_INT(WindZoneData, mode, "Mode (0=Dir,1=Sphere)"), FIELD_VEC3(WindZoneData, direction, "Direction"),
            FIELD_FLOAT(WindZoneData, mainStrength, "Main Strength"),
            FIELD_FLOAT(WindZoneData, turbulenceStrength, "Turbulence"),
            FIELD_FLOAT(WindZoneData, pulseFrequency, "Pulse Frequency"), FIELD_FLOAT(WindZoneData, radius, "Radius"),
            FIELD_BOOL(WindZoneData, affectsParticles, "Affects Particles"),
            FIELD_BOOL(WindZoneData, affectsVegetation, "Affects Vegetation"));
    }

    // ============================================================================
    // Billboard
    // ============================================================================

    void InspectorPanel::RenderBillboardComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::BILLBOARD, ICON_FA_IMAGE, "Billboard", BillboardData,
                                   FIELD_STRING(BillboardData, texturePath, "Texture Path"),
                                   FIELD_VEC4(BillboardData, color, "Color"),
                                   FIELD_INT(BillboardData, lockAxis, "Lock Axis (0=Full,1=Y,2=None)"),
                                   FIELD_FLOAT(BillboardData, fadeStartDistance, "Fade Start Distance"),
                                   FIELD_FLOAT(BillboardData, fadeEndDistance, "Fade End Distance"),
                                   FIELD_INT(BillboardData, sortingLayer, "Sorting Layer"));
    }

    // ============================================================================
    // Audio Listener
    // ============================================================================

    void InspectorPanel::RenderAudioListenerComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::AUDIO_LISTENER, ICON_FA_HEADPHONES, "Audio Listener",
                                   AudioListenerData, FIELD_BOOL(AudioListenerData, isActive, "Is Active"),
                                   FIELD_FLOAT(AudioListenerData, volumeScale, "Volume Scale"));
    }

    // ============================================================================
    // Character Controller
    // ============================================================================

    void InspectorPanel::RenderCharacterControllerComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::CHARACTER_CONTROLLER, ICON_FA_RUNNING, "Character Controller",
                                   CharacterControllerData, FIELD_FLOAT(CharacterControllerData, height, "Height"),
                                   FIELD_FLOAT(CharacterControllerData, radius, "Radius"),
                                   FIELD_FLOAT(CharacterControllerData, slopeLimit, "Slope Limit"),
                                   FIELD_FLOAT(CharacterControllerData, stepHeight, "Step Height"),
                                   FIELD_FLOAT(CharacterControllerData, skinWidth, "Skin Width"),
                                   FIELD_FLOAT(CharacterControllerData, moveSpeed, "Move Speed"),
                                   FIELD_FLOAT(CharacterControllerData, jumpForce, "Jump Force"),
                                   FIELD_FLOAT(CharacterControllerData, gravity, "Gravity"));
    }

    // ============================================================================
    // Skybox
    // ============================================================================

    void InspectorPanel::RenderSkyboxComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::SKYBOX, ICON_FA_CLOUD_SUN, "Skybox", SkyboxData,
            FIELD_ENUM(SkyboxData, mode, "Mode", "Procedural", "Cubemap", "Gradient", "Solid Color"),
            FIELD_STRING(SkyboxData, cubemapPath, "Cubemap Path"), FIELD_VEC4(SkyboxData, topColor, "Top Color"),
            FIELD_VEC4(SkyboxData, bottomColor, "Bottom Color"), FIELD_FLOAT(SkyboxData, exposure, "Exposure"),
            FIELD_FLOAT(SkyboxData, rotation, "Rotation"));
    }

} // namespace SparkEditor
