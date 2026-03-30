/**
 * @file ComponentReflection.cpp
 * @brief Runtime reflection registration for all ECS components
 *
 * Registers every ECS component type with the Spark::TypeRegistry (field metadata)
 * and Spark::ComponentFactory (dynamic add/has/remove/getRaw operations).
 *
 * All registrations happen at static initialization time via the SPARK_REFLECT_*
 * macros and a static ComponentFactoryRegistrar. After startup both registries
 * are read-only and safe to query from any thread.
 *
 * @see Core/Reflection.h, Engine/ECS/Components.h
 */

#include "Reflection.h"
#include "../Engine/ECS/Components.h"
#include "../Engine/ECS/Components/CollisionMaskComponents.h"
#include "../Engine/ECS/Components/VisibilityComponents.h"
#include "../Engine/ECS/Components/TerrainComponents.h"

#include "Utils/LogMacros.h"

#include <cstring>
#include <sstream>
#include <string>

// ============================================================================
// SetFieldFromString / GetFieldAsString implementations
// ============================================================================

namespace Spark
{

    bool SetFieldFromString(void* component, const FieldInfo& field, const std::string& value)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "SetFieldFromString: field='%s' value='%s'", field.name,
                        value.c_str());
        auto* dst = static_cast<char*>(component) + field.offset;

        switch (field.type)
        {
        case FieldType::Bool:
        {
            bool v = (value == "true" || value == "1" || value == "yes");
            std::memcpy(dst, &v, sizeof(bool));
            return true;
        }
        case FieldType::Int:
        {
            try
            {
                int v = std::stoi(value);
                std::memcpy(dst, &v, sizeof(int));
                return true;
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse int from '%s'",
                                value.c_str());
                return false;
            }
        }
        case FieldType::Float:
        {
            try
            {
                float v = std::stof(value);
                std::memcpy(dst, &v, sizeof(float));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        case FieldType::Double:
        {
            try
            {
                double v = std::stod(value);
                std::memcpy(dst, &v, sizeof(double));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        case FieldType::String:
        {
            auto* str = reinterpret_cast<std::string*>(dst);
            *str = value;
            return true;
        }
        case FieldType::Vector3:
        {
            // Parse "x,y,z" format
            float x = 0, y = 0, z = 0;
            std::istringstream ss(value);
            char delim;
            if (ss >> x >> delim >> y >> delim >> z)
            {
                std::memcpy(dst, &x, sizeof(float));
                std::memcpy(dst + sizeof(float), &y, sizeof(float));
                std::memcpy(dst + 2 * sizeof(float), &z, sizeof(float));
                return true;
            }
            return false;
        }
        case FieldType::Vector4:
        {
            // Parse "x,y,z,w" format
            float x = 0, y = 0, z = 0, w = 0;
            std::istringstream ss(value);
            char delim;
            if (ss >> x >> delim >> y >> delim >> z >> delim >> w)
            {
                std::memcpy(dst, &x, sizeof(float));
                std::memcpy(dst + sizeof(float), &y, sizeof(float));
                std::memcpy(dst + 2 * sizeof(float), &z, sizeof(float));
                std::memcpy(dst + 3 * sizeof(float), &w, sizeof(float));
                return true;
            }
            return false;
        }
        default:
            SPARK_LOG_WARN(Spark::LogCategory::Core, "SetFieldFromString: unsupported field type %d for '%s'",
                           static_cast<int>(field.type), field.name);
            return false;
        }
    }

    std::string GetFieldAsString(const void* component, const FieldInfo& field)
    {
        const auto* src = static_cast<const char*>(component) + field.offset;

        switch (field.type)
        {
        case FieldType::Bool:
        {
            bool v;
            std::memcpy(&v, src, sizeof(bool));
            return v ? "true" : "false";
        }
        case FieldType::Int:
        {
            int v;
            std::memcpy(&v, src, sizeof(int));
            return std::to_string(v);
        }
        case FieldType::Float:
        {
            float v;
            std::memcpy(&v, src, sizeof(float));
            return std::to_string(v);
        }
        case FieldType::Double:
        {
            double v;
            std::memcpy(&v, src, sizeof(double));
            return std::to_string(v);
        }
        case FieldType::String:
        {
            const auto* str = reinterpret_cast<const std::string*>(src);
            return *str;
        }
        case FieldType::Vector3:
        {
            float x, y, z;
            std::memcpy(&x, src, sizeof(float));
            std::memcpy(&y, src + sizeof(float), sizeof(float));
            std::memcpy(&z, src + 2 * sizeof(float), sizeof(float));
            return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
        }
        case FieldType::Vector4:
        {
            float x, y, z, w;
            std::memcpy(&x, src, sizeof(float));
            std::memcpy(&y, src + sizeof(float), sizeof(float));
            std::memcpy(&z, src + 2 * sizeof(float), sizeof(float));
            std::memcpy(&w, src + 3 * sizeof(float), sizeof(float));
            return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + "," + std::to_string(w);
        }
        default:
            return "";
        }
    }

} // namespace Spark

// ============================================================================
// Helper macro: register a component type with the ComponentFactory.
//
// Uses World's template API behind type-erased void* callbacks. The entity
// ID is passed as uint32_t and reinterpret-cast to EntityID (entt::entity).
// ============================================================================

#define SPARK_REGISTER_COMPONENT(Type)                                                                                 \
    {                                                                                                                  \
        Spark::ComponentOps ops;                                                                                       \
        ops.add = [](void* w, uint32_t e) { static_cast<World*>(w)->AddComponent<Type>(static_cast<EntityID>(e)); };   \
        ops.has = [](void* w, uint32_t e) -> bool                                                                      \
        { return static_cast<World*>(w)->HasComponent<Type>(static_cast<EntityID>(e)); };                              \
        ops.remove = [](void* w, uint32_t e)                                                                           \
        { static_cast<World*>(w)->RemoveComponent<Type>(static_cast<EntityID>(e)); };                                  \
        ops.getRaw = [](void* w, uint32_t e) -> void*                                                                  \
        { return static_cast<World*>(w)->GetComponent<Type>(static_cast<EntityID>(e)); };                              \
        Spark::ComponentFactory::Get().Register(#Type, ops);                                                           \
    }

// ============================================================================
// XMFLOAT3/XMFLOAT4 → FieldType mappings
//
// DeduceFieldType doesn't know about DirectX types, so we specify Vector3/
// Vector4 explicitly via SPARK_REFLECT_FIELD_AS.
// ============================================================================

#define SPARK_REFLECT_FIELD_AS(Type, member, displayName, fieldType)                                                   \
    {                                                                                                                  \
        Spark::FieldInfo field;                                                                                        \
        field.name = displayName;                                                                                      \
        field.fieldName = #member;                                                                                     \
        field.type = fieldType;                                                                                        \
        field.offset = offsetof(Type, member);                                                                         \
        field.size = sizeof(Type::member);                                                                             \
        field.ownerType = GetTypeId<Type>();                                                                           \
        info.fields.push_back(field);                                                                                  \
    }

// ============================================================================
// Type reflection registrations — SPARK_REFLECT_TYPE + SPARK_REFLECT_FIELD
// ============================================================================

// --- Core ---

SPARK_REFLECT_TYPE(NameComponent)
SPARK_REFLECT_FIELD(NameComponent, name, "Name")
SPARK_REFLECT_END(NameComponent)

SPARK_REFLECT_TYPE(Transform)
SPARK_REFLECT_FIELD_AS(Transform, position, "Position", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD_AS(Transform, rotation, "Rotation", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD_AS(Transform, scale, "Scale", Spark::FieldType::Vector3)
SPARK_REFLECT_END(Transform)

SPARK_REFLECT_TYPE(MeshRenderer)
SPARK_REFLECT_FIELD(MeshRenderer, meshPath, "Mesh Path")
SPARK_REFLECT_FIELD(MeshRenderer, materialPath, "Material Path")
SPARK_REFLECT_FIELD(MeshRenderer, castShadows, "Cast Shadows")
SPARK_REFLECT_FIELD(MeshRenderer, receiveShadows, "Receive Shadows")
SPARK_REFLECT_FIELD(MeshRenderer, visible, "Visible")
SPARK_REFLECT_END(MeshRenderer)

SPARK_REFLECT_TYPE(Camera)
SPARK_REFLECT_FIELD_RANGE(Camera, fov, "FOV", 1.0f, 179.0f)
SPARK_REFLECT_FIELD(Camera, nearPlane, "Near Plane")
SPARK_REFLECT_FIELD(Camera, farPlane, "Far Plane")
SPARK_REFLECT_FIELD(Camera, isMainCamera, "Is Main Camera")
SPARK_REFLECT_END(Camera)

SPARK_REFLECT_TYPE(Script)
SPARK_REFLECT_FIELD(Script, scriptPath, "Script Path")
SPARK_REFLECT_FIELD(Script, className, "Class Name")
SPARK_REFLECT_FIELD(Script, moduleName, "Module Name")
SPARK_REFLECT_FIELD(Script, enabled, "Enabled")
SPARK_REFLECT_END(Script)

// --- Physics ---

SPARK_REFLECT_TYPE(RigidBodyComponent)
SPARK_REFLECT_FIELD(RigidBodyComponent, mass, "Mass")
SPARK_REFLECT_FIELD(RigidBodyComponent, friction, "Friction")
SPARK_REFLECT_FIELD_RANGE(RigidBodyComponent, restitution, "Restitution", 0.0f, 1.0f)
SPARK_REFLECT_FIELD(RigidBodyComponent, linearDamping, "Linear Damping")
SPARK_REFLECT_FIELD(RigidBodyComponent, angularDamping, "Angular Damping")
SPARK_REFLECT_FIELD(RigidBodyComponent, gravityFactor, "Gravity Factor")
SPARK_REFLECT_FIELD(RigidBodyComponent, isTrigger, "Is Trigger")
SPARK_REFLECT_END(RigidBodyComponent)

SPARK_REFLECT_TYPE(ColliderComponent)
SPARK_REFLECT_FIELD_AS(ColliderComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(ColliderComponent, radius, "Radius")
SPARK_REFLECT_FIELD(ColliderComponent, height, "Height")
SPARK_REFLECT_FIELD_AS(ColliderComponent, offset, "Offset", Spark::FieldType::Vector3)
SPARK_REFLECT_END(ColliderComponent)

// --- Audio ---

SPARK_REFLECT_TYPE(AudioSourceComponent)
SPARK_REFLECT_FIELD(AudioSourceComponent, soundName, "Sound Name")
SPARK_REFLECT_FIELD_RANGE(AudioSourceComponent, volume, "Volume", 0.0f, 2.0f)
SPARK_REFLECT_FIELD_RANGE(AudioSourceComponent, pitch, "Pitch", 0.01f, 4.0f)
SPARK_REFLECT_FIELD(AudioSourceComponent, minDistance, "Min Distance")
SPARK_REFLECT_FIELD(AudioSourceComponent, maxDistance, "Max Distance")
SPARK_REFLECT_FIELD(AudioSourceComponent, is3D, "Is 3D")
SPARK_REFLECT_FIELD(AudioSourceComponent, loop, "Loop")
SPARK_REFLECT_FIELD(AudioSourceComponent, playOnAwake, "Play On Awake")
SPARK_REFLECT_END(AudioSourceComponent)

// --- Light ---

SPARK_REFLECT_TYPE(LightComponent)
SPARK_REFLECT_FIELD_AS(LightComponent, color, "Color", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(LightComponent, intensity, "Intensity")
SPARK_REFLECT_FIELD(LightComponent, range, "Range")
SPARK_REFLECT_FIELD(LightComponent, spotAngle, "Spot Angle")
SPARK_REFLECT_FIELD(LightComponent, spotInnerAngle, "Spot Inner Angle")
SPARK_REFLECT_FIELD(LightComponent, castShadows, "Cast Shadows")
SPARK_REFLECT_FIELD(LightComponent, shadowMapResolution, "Shadow Map Resolution")
SPARK_REFLECT_END(LightComponent)

// --- Animation ---

SPARK_REFLECT_TYPE(ParticleEmitterComponent)
SPARK_REFLECT_FIELD(ParticleEmitterComponent, effectName, "Effect Name")
SPARK_REFLECT_FIELD(ParticleEmitterComponent, autoPlay, "Auto Play")
SPARK_REFLECT_FIELD(ParticleEmitterComponent, emissionRate, "Emission Rate")
SPARK_REFLECT_FIELD(ParticleEmitterComponent, lifetime, "Lifetime")
SPARK_REFLECT_FIELD_AS(ParticleEmitterComponent, startColor, "Start Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(ParticleEmitterComponent, startSize, "Start Size")
SPARK_REFLECT_FIELD(ParticleEmitterComponent, startSpeed, "Start Speed")
SPARK_REFLECT_END(ParticleEmitterComponent)

SPARK_REFLECT_TYPE(AnimationController)
SPARK_REFLECT_FIELD(AnimationController, currentAnimation, "Current Animation")
SPARK_REFLECT_FIELD(AnimationController, defaultAnimation, "Default Animation")
SPARK_REFLECT_FIELD(AnimationController, playbackSpeed, "Playback Speed")
SPARK_REFLECT_FIELD(AnimationController, playing, "Playing")
SPARK_REFLECT_FIELD(AnimationController, loop, "Loop")
SPARK_REFLECT_END(AnimationController)

// --- AI ---

SPARK_REFLECT_TYPE(AIComponent)
SPARK_REFLECT_FIELD(AIComponent, behaviorTreeName, "Behavior Tree")
SPARK_REFLECT_END(AIComponent)

// --- Networking ---

SPARK_REFLECT_TYPE(NetworkIdentity)
SPARK_REFLECT_FIELD(NetworkIdentity, replicateTransform, "Replicate Transform")
SPARK_REFLECT_FIELD(NetworkIdentity, replicateHealth, "Replicate Health")
SPARK_REFLECT_END(NetworkIdentity)

// --- Gameplay ---

SPARK_REFLECT_TYPE(ActiveComponent)
SPARK_REFLECT_FIELD(ActiveComponent, active, "Active")
SPARK_REFLECT_END(ActiveComponent)

SPARK_REFLECT_TYPE(HealthComponent)
SPARK_REFLECT_FIELD(HealthComponent, health, "Health")
SPARK_REFLECT_FIELD_RANGE(HealthComponent, maxHealth, "Max Health", 1.0f, 100000.0f)
SPARK_REFLECT_END(HealthComponent)

SPARK_REFLECT_TYPE(WeatherComponent)
SPARK_REFLECT_FIELD(WeatherComponent, weatherType, "Weather Type")
SPARK_REFLECT_FIELD_RANGE(WeatherComponent, intensity, "Intensity", 0.0f, 1.0f)
SPARK_REFLECT_FIELD(WeatherComponent, windSpeed, "Wind Speed")
SPARK_REFLECT_FIELD(WeatherComponent, transitionTime, "Transition Time")
SPARK_REFLECT_FIELD(WeatherComponent, enabled, "Enabled")
SPARK_REFLECT_END(WeatherComponent)

SPARK_REFLECT_TYPE(InventoryTag)
SPARK_REFLECT_FIELD(InventoryTag, maxSlots, "Max Slots")
SPARK_REFLECT_FIELD(InventoryTag, maxWeight, "Max Weight")
SPARK_REFLECT_FIELD(InventoryTag, currency, "Currency")
SPARK_REFLECT_END(InventoryTag)

SPARK_REFLECT_TYPE(QuestTrackerTag)
SPARK_REFLECT_FIELD(QuestTrackerTag, activeQuestCount, "Active Quest Count")
SPARK_REFLECT_FIELD(QuestTrackerTag, completedQuestCount, "Completed Quest Count")
SPARK_REFLECT_END(QuestTrackerTag)

// --- FPS ---

SPARK_REFLECT_TYPE(DecalComponent)
SPARK_REFLECT_FIELD(DecalComponent, texturePath, "Texture Path")
SPARK_REFLECT_FIELD(DecalComponent, category, "Category")
SPARK_REFLECT_FIELD_AS(DecalComponent, size, "Size", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(DecalComponent, lifetime, "Lifetime")
SPARK_REFLECT_FIELD(DecalComponent, fadeOutDuration, "Fade Out Duration")
SPARK_REFLECT_FIELD(DecalComponent, receiveLighting, "Receive Lighting")
SPARK_REFLECT_FIELD(DecalComponent, sortOrder, "Sort Order")
SPARK_REFLECT_END(DecalComponent)

SPARK_REFLECT_TYPE(ProjectileComponent)
SPARK_REFLECT_FIELD_AS(ProjectileComponent, direction, "Direction", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(ProjectileComponent, speed, "Speed")
SPARK_REFLECT_FIELD(ProjectileComponent, gravityScale, "Gravity Scale")
SPARK_REFLECT_FIELD(ProjectileComponent, damage, "Damage")
SPARK_REFLECT_FIELD(ProjectileComponent, explosionRadius, "Explosion Radius")
SPARK_REFLECT_FIELD(ProjectileComponent, maxRange, "Max Range")
SPARK_REFLECT_FIELD(ProjectileComponent, maxLifetime, "Max Lifetime")
SPARK_REFLECT_FIELD(ProjectileComponent, impactEffectName, "Impact Effect")
SPARK_REFLECT_FIELD(ProjectileComponent, impactDecalPath, "Impact Decal")
SPARK_REFLECT_FIELD(ProjectileComponent, trailEffectName, "Trail Effect")
SPARK_REFLECT_END(ProjectileComponent)

SPARK_REFLECT_TYPE(InteractionComponent)
SPARK_REFLECT_FIELD(InteractionComponent, displayName, "Display Name")
SPARK_REFLECT_FIELD(InteractionComponent, actionVerb, "Action Verb")
SPARK_REFLECT_FIELD(InteractionComponent, interactionRadius, "Interaction Radius")
SPARK_REFLECT_FIELD(InteractionComponent, holdDuration, "Hold Duration")
SPARK_REFLECT_FIELD(InteractionComponent, cooldownDuration, "Cooldown Duration")
SPARK_REFLECT_FIELD(InteractionComponent, usesRemaining, "Uses Remaining")
SPARK_REFLECT_FIELD(InteractionComponent, showHighlight, "Show Highlight")
SPARK_REFLECT_FIELD(InteractionComponent, requiredItemId, "Required Item")
SPARK_REFLECT_FIELD(InteractionComponent, onInteractEvent, "On Interact Event")
SPARK_REFLECT_FIELD(InteractionComponent, interactSoundName, "Interact Sound")
SPARK_REFLECT_END(InteractionComponent)

// --- Spline ---

SPARK_REFLECT_TYPE(SplineFollowerComponent)
SPARK_REFLECT_FIELD(SplineFollowerComponent, speed, "Speed")
SPARK_REFLECT_FIELD(SplineFollowerComponent, playing, "Playing")
SPARK_REFLECT_FIELD(SplineFollowerComponent, orientToPath, "Orient To Path")
SPARK_REFLECT_END(SplineFollowerComponent)

// --- Volumes ---

SPARK_REFLECT_TYPE(TriggerVolumeComponent)
SPARK_REFLECT_FIELD(TriggerVolumeComponent, radius, "Radius")
SPARK_REFLECT_FIELD_AS(TriggerVolumeComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(TriggerVolumeComponent, onEnterEvent, "On Enter Event")
SPARK_REFLECT_FIELD(TriggerVolumeComponent, onExitEvent, "On Exit Event")
SPARK_REFLECT_FIELD(TriggerVolumeComponent, enabled, "Enabled")
SPARK_REFLECT_FIELD(TriggerVolumeComponent, oneShot, "One Shot")
SPARK_REFLECT_END(TriggerVolumeComponent)

SPARK_REFLECT_TYPE(PostProcessVolumeComponent)
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, isGlobal, "Is Global")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, priority, "Priority")
SPARK_REFLECT_FIELD_RANGE(PostProcessVolumeComponent, weight, "Weight", 0.0f, 1.0f)
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, blendDistance, "Blend Distance")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, bloomIntensity, "Bloom Intensity")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, bloomThreshold, "Bloom Threshold")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, saturation, "Saturation")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, contrast, "Contrast")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, temperature, "Temperature")
SPARK_REFLECT_FIELD(PostProcessVolumeComponent, fogDensity, "Fog Density")
SPARK_REFLECT_END(PostProcessVolumeComponent)

SPARK_REFLECT_TYPE(ReflectionProbeComponent)
SPARK_REFLECT_FIELD(ReflectionProbeComponent, resolution, "Resolution")
SPARK_REFLECT_FIELD(ReflectionProbeComponent, influenceRadius, "Influence Radius")
SPARK_REFLECT_FIELD_AS(ReflectionProbeComponent, boxExtents, "Box Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(ReflectionProbeComponent, useBoxProjection, "Use Box Projection")
SPARK_REFLECT_FIELD(ReflectionProbeComponent, isDynamic, "Is Dynamic")
SPARK_REFLECT_FIELD(ReflectionProbeComponent, importance, "Importance")
SPARK_REFLECT_END(ReflectionProbeComponent)

SPARK_REFLECT_TYPE(LightProbeComponent)
SPARK_REFLECT_FIELD(LightProbeComponent, influenceRadius, "Influence Radius")
SPARK_REFLECT_FIELD(LightProbeComponent, baked, "Baked")
SPARK_REFLECT_FIELD(LightProbeComponent, shOrder, "SH Order")
SPARK_REFLECT_END(LightProbeComponent)

SPARK_REFLECT_TYPE(NavObstacleComponent)
SPARK_REFLECT_FIELD_AS(NavObstacleComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(NavObstacleComponent, radius, "Radius")
SPARK_REFLECT_FIELD(NavObstacleComponent, height, "Height")
SPARK_REFLECT_FIELD(NavObstacleComponent, carveOnMove, "Carve On Move")
SPARK_REFLECT_END(NavObstacleComponent)

SPARK_REFLECT_TYPE(WaterPlaneComponent)
SPARK_REFLECT_FIELD(WaterPlaneComponent, waveHeight, "Wave Height")
SPARK_REFLECT_FIELD(WaterPlaneComponent, waveSpeed, "Wave Speed")
SPARK_REFLECT_FIELD(WaterPlaneComponent, waveFrequency, "Wave Frequency")
SPARK_REFLECT_FIELD_RANGE(WaterPlaneComponent, reflectionStrength, "Reflection Strength", 0.0f, 1.0f)
SPARK_REFLECT_FIELD_RANGE(WaterPlaneComponent, refractionStrength, "Refraction Strength", 0.0f, 1.0f)
SPARK_REFLECT_FIELD(WaterPlaneComponent, receiveShadows, "Receive Shadows")
SPARK_REFLECT_END(WaterPlaneComponent)

SPARK_REFLECT_TYPE(FogVolumeComponent)
SPARK_REFLECT_FIELD_AS(FogVolumeComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(FogVolumeComponent, density, "Density")
SPARK_REFLECT_FIELD_AS(FogVolumeComponent, color, "Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(FogVolumeComponent, falloff, "Falloff")
SPARK_REFLECT_FIELD(FogVolumeComponent, heightFalloff, "Height Falloff")
SPARK_REFLECT_END(FogVolumeComponent)

SPARK_REFLECT_TYPE(SpawnPointComponent)
SPARK_REFLECT_FIELD(SpawnPointComponent, spawnTag, "Spawn Tag")
SPARK_REFLECT_FIELD(SpawnPointComponent, teamID, "Team ID")
SPARK_REFLECT_FIELD(SpawnPointComponent, spawnRadius, "Spawn Radius")
SPARK_REFLECT_FIELD(SpawnPointComponent, respawnDelay, "Respawn Delay")
SPARK_REFLECT_FIELD(SpawnPointComponent, maxConcurrent, "Max Concurrent")
SPARK_REFLECT_FIELD(SpawnPointComponent, enabled, "Enabled")
SPARK_REFLECT_FIELD(SpawnPointComponent, priority, "Priority")
SPARK_REFLECT_END(SpawnPointComponent)

SPARK_REFLECT_TYPE(AudioReverbZoneComponent)
SPARK_REFLECT_FIELD(AudioReverbZoneComponent, innerRadius, "Inner Radius")
SPARK_REFLECT_FIELD(AudioReverbZoneComponent, outerRadius, "Outer Radius")
SPARK_REFLECT_FIELD(AudioReverbZoneComponent, decayTime, "Decay Time")
SPARK_REFLECT_FIELD_RANGE(AudioReverbZoneComponent, earlyReflections, "Early Reflections", 0.0f, 1.0f)
SPARK_REFLECT_FIELD_RANGE(AudioReverbZoneComponent, lateReverbLevel, "Late Reverb Level", 0.0f, 1.0f)
SPARK_REFLECT_FIELD_RANGE(AudioReverbZoneComponent, diffusion, "Diffusion", 0.0f, 1.0f)
SPARK_REFLECT_FIELD_RANGE(AudioReverbZoneComponent, roomSize, "Room Size", 0.0f, 1.0f)
SPARK_REFLECT_FIELD(AudioReverbZoneComponent, enabled, "Enabled")
SPARK_REFLECT_END(AudioReverbZoneComponent)

// --- Terrain ---

SPARK_REFLECT_TYPE(TerrainComponent)
SPARK_REFLECT_FIELD(TerrainComponent, terrainAssetPath, "Terrain Asset Path")
SPARK_REFLECT_FIELD(TerrainComponent, heightmapResolution, "Heightmap Resolution")
SPARK_REFLECT_FIELD(TerrainComponent, terrainSize, "Terrain Size")
SPARK_REFLECT_FIELD(TerrainComponent, heightScale, "Height Scale")
SPARK_REFLECT_FIELD(TerrainComponent, minHeight, "Min Height")
SPARK_REFLECT_FIELD(TerrainComponent, maxHeight, "Max Height")
SPARK_REFLECT_FIELD(TerrainComponent, lodLevels, "LOD Levels")
SPARK_REFLECT_FIELD(TerrainComponent, lodBias, "LOD Bias")
SPARK_REFLECT_FIELD(TerrainComponent, generateCollider, "Generate Collider")
SPARK_REFLECT_FIELD(TerrainComponent, castShadows, "Cast Shadows")
SPARK_REFLECT_FIELD(TerrainComponent, receiveShadows, "Receive Shadows")
SPARK_REFLECT_FIELD(TerrainComponent, visible, "Visible")
SPARK_REFLECT_END(TerrainComponent)

// ============================================================================
// ComponentFactory registrations — static initializer
// ============================================================================

namespace
{

    struct ComponentFactoryRegistrar
    {
        ComponentFactoryRegistrar()
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Registering ECS component types with ComponentFactory");
            // Core
            SPARK_REGISTER_COMPONENT(NameComponent)
            SPARK_REGISTER_COMPONENT(Transform)
            SPARK_REGISTER_COMPONENT(MeshRenderer)
            SPARK_REGISTER_COMPONENT(Camera)
            SPARK_REGISTER_COMPONENT(Script)

            // Physics
            SPARK_REGISTER_COMPONENT(RigidBodyComponent)
            SPARK_REGISTER_COMPONENT(ColliderComponent)

            // Audio
            SPARK_REGISTER_COMPONENT(AudioSourceComponent)

            // Light
            SPARK_REGISTER_COMPONENT(LightComponent)

            // Animation
            SPARK_REGISTER_COMPONENT(ParticleEmitterComponent)
            SPARK_REGISTER_COMPONENT(AnimationController)

            // AI
            SPARK_REGISTER_COMPONENT(AIComponent)

            // Networking
            SPARK_REGISTER_COMPONENT(NetworkIdentity)

            // Gameplay
            SPARK_REGISTER_COMPONENT(ActiveComponent)
            SPARK_REGISTER_COMPONENT(HealthComponent)
            SPARK_REGISTER_COMPONENT(WeatherComponent)
            SPARK_REGISTER_COMPONENT(InventoryTag)
            SPARK_REGISTER_COMPONENT(QuestTrackerTag)

            // FPS
            SPARK_REGISTER_COMPONENT(DecalComponent)
            SPARK_REGISTER_COMPONENT(ProjectileComponent)
            SPARK_REGISTER_COMPONENT(InteractionComponent)

            // Spline
            SPARK_REGISTER_COMPONENT(SplineComponent)
            SPARK_REGISTER_COMPONENT(SplineFollowerComponent)

            // Volumes
            SPARK_REGISTER_COMPONENT(TriggerVolumeComponent)
            SPARK_REGISTER_COMPONENT(PostProcessVolumeComponent)
            SPARK_REGISTER_COMPONENT(ReflectionProbeComponent)
            SPARK_REGISTER_COMPONENT(LightProbeComponent)
            SPARK_REGISTER_COMPONENT(NavObstacleComponent)
            SPARK_REGISTER_COMPONENT(WaterPlaneComponent)
            SPARK_REGISTER_COMPONENT(FogVolumeComponent)
            SPARK_REGISTER_COMPONENT(LODGroupComponent)
            SPARK_REGISTER_COMPONENT(SpawnPointComponent)
            SPARK_REGISTER_COMPONENT(AudioReverbZoneComponent)

            // Terrain
            SPARK_REGISTER_COMPONENT(TerrainComponent)

            SPARK_LOG_INFO(Spark::LogCategory::Core, "ComponentFactory registration complete");
        }
    };

    static ComponentFactoryRegistrar s_componentFactoryRegistrar;

} // anonymous namespace
