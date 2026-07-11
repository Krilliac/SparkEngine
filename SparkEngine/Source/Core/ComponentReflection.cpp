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

// Bring Spark-namespaced component types into the global namespace so
// the SPARK_REFLECT_TYPE token-pasting macros work.
using Spark::CameraDrawMaskComponent;
using Spark::CollisionMaskComponent;
using Spark::VisibilityMaskComponent;

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
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "SetFieldFromString: field='%s' value='%s'", field.name.c_str(),
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
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse float from '%s'",
                                value.c_str());
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
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse double from '%s'",
                                value.c_str());
                return false;
            }
        }
        case FieldType::String:
        {
            auto* str = reinterpret_cast<std::string*>(dst);
            *str = value;
            return true;
        }
        case FieldType::Vector2:
        {
            // Parse "x,y" format
            float x = 0, y = 0;
            std::istringstream ss(value);
            char delim;
            if (ss >> x >> delim >> y)
            {
                std::memcpy(dst, &x, sizeof(float));
                std::memcpy(dst + sizeof(float), &y, sizeof(float));
                return true;
            }
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse Vector2 from '%s'",
                            value.c_str());
            return false;
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
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse Vector3 from '%s'",
                            value.c_str());
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
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse Vector4 from '%s'",
                            value.c_str());
            return false;
        }
        case FieldType::Enum:
        {
            // Plain `enum class` types deduced as Enum have no explicit underlying
            // type, so MSVC stores them as `int`. Round-trip via the underlying int
            // so scene reload preserves the selected value (previously fell through
            // to `default:` and silently dropped the write).
            try
            {
                int v = std::stoi(value);
                std::memcpy(dst, &v, sizeof(int));
                return true;
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SetFieldFromString: failed to parse enum int from '%s'",
                                value.c_str());
                return false;
            }
        }
        default:
            SPARK_LOG_WARN(Spark::LogCategory::Core, "SetFieldFromString: unsupported field type %d for '%s'",
                           static_cast<int>(field.type), field.name.c_str());
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
        case FieldType::Vector2:
        {
            float x, y;
            std::memcpy(&x, src, sizeof(float));
            std::memcpy(&y, src + sizeof(float), sizeof(float));
            return std::to_string(x) + "," + std::to_string(y);
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
        case FieldType::Enum:
        {
            // See SetFieldFromString: MSVC stores a plain `enum class` (no explicit
            // underlying type) as `int`, so read it back the same way.
            int v;
            std::memcpy(&v, src, sizeof(int));
            return std::to_string(v);
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
SPARK_REFLECT_FIELD_ATTR_AS(Transform, position, "Position", Spark::FieldType::Vector3, field.replicated = true;)
SPARK_REFLECT_FIELD_ATTR_AS(Transform, rotation, "Rotation", Spark::FieldType::Vector3, field.replicated = true;)
SPARK_REFLECT_FIELD_ATTR_AS(Transform, scale, "Scale", Spark::FieldType::Vector3, field.replicated = true;)
SPARK_REFLECT_END(Transform)

SPARK_REFLECT_TYPE(MeshRenderer)
SPARK_REFLECT_FIELD(MeshRenderer, meshPath, "Mesh Path")
SPARK_REFLECT_FIELD(MeshRenderer, materialPath, "Material Path")
SPARK_REFLECT_FIELD(MeshRenderer, castShadows, "Cast Shadows")
SPARK_REFLECT_FIELD(MeshRenderer, receiveShadows, "Receive Shadows")
SPARK_REFLECT_FIELD(MeshRenderer, visible, "Visible")
SPARK_REFLECT_FIELD_RANGE(MeshRenderer, emissive, "Emissive", 0.0f, 2.0f)
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
SPARK_REFLECT_FIELD(RigidBodyComponent, type, "Type")
SPARK_REFLECT_FIELD(RigidBodyComponent, mass, "Mass")
SPARK_REFLECT_FIELD(RigidBodyComponent, friction, "Friction")
SPARK_REFLECT_FIELD_RANGE(RigidBodyComponent, restitution, "Restitution", 0.0f, 1.0f)
SPARK_REFLECT_FIELD(RigidBodyComponent, linearDamping, "Linear Damping")
SPARK_REFLECT_FIELD(RigidBodyComponent, angularDamping, "Angular Damping")
SPARK_REFLECT_FIELD(RigidBodyComponent, gravityFactor, "Gravity Factor")
SPARK_REFLECT_FIELD(RigidBodyComponent, isTrigger, "Is Trigger")
SPARK_REFLECT_FIELD(RigidBodyComponent, motionQuality, "Motion Quality")
SPARK_REFLECT_END(RigidBodyComponent)

SPARK_REFLECT_TYPE(ColliderComponent)
SPARK_REFLECT_FIELD(ColliderComponent, shape, "Shape")
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
SPARK_REFLECT_FIELD_ATTR(NetworkIdentity, replicateTransform, "Replicate Transform", field.replicated = true;)
SPARK_REFLECT_FIELD_ATTR(NetworkIdentity, replicateHealth, "Replicate Health", field.replicated = true;)
SPARK_REFLECT_END(NetworkIdentity)

// --- Gameplay ---

SPARK_REFLECT_TYPE(ActiveComponent)
SPARK_REFLECT_FIELD(ActiveComponent, active, "Active")
SPARK_REFLECT_END(ActiveComponent)

SPARK_REFLECT_TYPE(HealthComponent)
SPARK_REFLECT_FIELD_ATTR(HealthComponent, health, "Health", field.replicated = true;)
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

// --- Spline (missing reflection) ---

SPARK_REFLECT_TYPE(SplineComponent)
SPARK_REFLECT_FIELD(SplineComponent, debugVisible, "Debug Visible")
SPARK_REFLECT_END(SplineComponent)

// --- Volumes (missing reflection) ---

SPARK_REFLECT_TYPE(LODGroupComponent)
SPARK_REFLECT_FIELD(LODGroupComponent, lodCount, "LOD Count")
SPARK_REFLECT_FIELD(LODGroupComponent, crossFadeDuration, "Cross Fade Duration")
SPARK_REFLECT_FIELD(LODGroupComponent, autoGenerate, "Auto Generate")
SPARK_REFLECT_FIELD(LODGroupComponent, currentLOD, "Current LOD")
SPARK_REFLECT_END(LODGroupComponent)

// --- 2D / Sprite ---

SPARK_REFLECT_TYPE(SpriteRenderer)
SPARK_REFLECT_FIELD(SpriteRenderer, texturePath, "Texture Path")
SPARK_REFLECT_FIELD_AS(SpriteRenderer, color, "Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD_AS(SpriteRenderer, sourceRect, "Source Rect", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD_AS(SpriteRenderer, pivot, "Pivot", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD(SpriteRenderer, pixelsPerUnit, "Pixels Per Unit")
SPARK_REFLECT_FIELD(SpriteRenderer, sortingLayer, "Sorting Layer")
SPARK_REFLECT_FIELD(SpriteRenderer, orderInLayer, "Order In Layer")
SPARK_REFLECT_FIELD(SpriteRenderer, flipX, "Flip X")
SPARK_REFLECT_FIELD(SpriteRenderer, flipY, "Flip Y")
SPARK_REFLECT_FIELD(SpriteRenderer, visible, "Visible")
SPARK_REFLECT_FIELD(SpriteRenderer, textureWidth, "Texture Width")
SPARK_REFLECT_FIELD(SpriteRenderer, textureHeight, "Texture Height")
SPARK_REFLECT_END(SpriteRenderer)

SPARK_REFLECT_TYPE(SpriteAnimator)
SPARK_REFLECT_FIELD(SpriteAnimator, currentClipIndex, "Current Clip Index")
SPARK_REFLECT_FIELD(SpriteAnimator, currentFrameIndex, "Current Frame Index")
SPARK_REFLECT_FIELD(SpriteAnimator, frameTimer, "Frame Timer")
SPARK_REFLECT_FIELD(SpriteAnimator, speed, "Speed")
SPARK_REFLECT_FIELD(SpriteAnimator, playing, "Playing")
SPARK_REFLECT_FIELD(SpriteAnimator, defaultClip, "Default Clip")
SPARK_REFLECT_END(SpriteAnimator)

SPARK_REFLECT_TYPE(Camera2D)
SPARK_REFLECT_FIELD(Camera2D, orthoSize, "Ortho Size")
SPARK_REFLECT_FIELD(Camera2D, nearPlane, "Near Plane")
SPARK_REFLECT_FIELD(Camera2D, farPlane, "Far Plane")
SPARK_REFLECT_FIELD(Camera2D, zoom, "Zoom")
SPARK_REFLECT_FIELD_AS(Camera2D, shakeOffset, "Shake Offset", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD_AS(Camera2D, deadZone, "Dead Zone", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD(Camera2D, followSmoothing, "Follow Smoothing")
SPARK_REFLECT_FIELD(Camera2D, isMain2DCamera, "Is Main 2D Camera")
SPARK_REFLECT_FIELD_AS(Camera2D, clearColor, "Clear Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(Camera2D, viewportX, "Viewport X")
SPARK_REFLECT_FIELD(Camera2D, viewportY, "Viewport Y")
SPARK_REFLECT_FIELD(Camera2D, viewportWidth, "Viewport Width")
SPARK_REFLECT_FIELD(Camera2D, viewportHeight, "Viewport Height")
SPARK_REFLECT_END(Camera2D)

SPARK_REFLECT_TYPE(ParallaxBackground)
SPARK_REFLECT_FIELD(ParallaxBackground, autoScroll, "Auto Scroll")
SPARK_REFLECT_FIELD_AS(ParallaxBackground, autoScrollSpeed, "Auto Scroll Speed", Spark::FieldType::Vector2)
SPARK_REFLECT_END(ParallaxBackground)

SPARK_REFLECT_TYPE(TilemapComponent)
SPARK_REFLECT_FIELD(TilemapComponent, width, "Width")
SPARK_REFLECT_FIELD(TilemapComponent, height, "Height")
SPARK_REFLECT_FIELD(TilemapComponent, sortingLayer, "Sorting Layer")
SPARK_REFLECT_FIELD(TilemapComponent, pixelsPerUnit, "Pixels Per Unit")
SPARK_REFLECT_FIELD(TilemapComponent, generateCollision, "Generate Collision")
SPARK_REFLECT_FIELD_AS(TilemapComponent, gridColor, "Grid Color", Spark::FieldType::Vector4)
SPARK_REFLECT_END(TilemapComponent)

SPARK_REFLECT_TYPE(RigidBody2D)
SPARK_REFLECT_FIELD(RigidBody2D, mass, "Mass")
SPARK_REFLECT_FIELD(RigidBody2D, gravityScale, "Gravity Scale")
SPARK_REFLECT_FIELD(RigidBody2D, linearDamping, "Linear Damping")
SPARK_REFLECT_FIELD(RigidBody2D, angularDamping, "Angular Damping")
SPARK_REFLECT_FIELD(RigidBody2D, fixedRotation, "Fixed Rotation")
SPARK_REFLECT_FIELD(RigidBody2D, isBullet, "Is Bullet")
SPARK_REFLECT_FIELD_AS(RigidBody2D, linearVelocity, "Linear Velocity", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD(RigidBody2D, angularVelocity, "Angular Velocity")
SPARK_REFLECT_FIELD(RigidBody2D, friction, "Friction")
SPARK_REFLECT_FIELD(RigidBody2D, restitution, "Restitution")
SPARK_REFLECT_END(RigidBody2D)

SPARK_REFLECT_TYPE(Collider2D)
SPARK_REFLECT_FIELD_AS(Collider2D, halfExtents, "Half Extents", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD(Collider2D, radius, "Radius")
SPARK_REFLECT_FIELD(Collider2D, height, "Height")
SPARK_REFLECT_FIELD_AS(Collider2D, offset, "Offset", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD(Collider2D, isTrigger, "Is Trigger")
SPARK_REFLECT_FIELD_AS(Collider2D, edgeStart, "Edge Start", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD_AS(Collider2D, edgeEnd, "Edge End", Spark::FieldType::Vector2)
SPARK_REFLECT_END(Collider2D)

SPARK_REFLECT_TYPE(NineSliceSprite)
SPARK_REFLECT_FIELD(NineSliceSprite, texturePath, "Texture Path")
SPARK_REFLECT_FIELD(NineSliceSprite, borderLeft, "Border Left")
SPARK_REFLECT_FIELD(NineSliceSprite, borderTop, "Border Top")
SPARK_REFLECT_FIELD(NineSliceSprite, borderRight, "Border Right")
SPARK_REFLECT_FIELD(NineSliceSprite, borderBottom, "Border Bottom")
SPARK_REFLECT_FIELD_AS(NineSliceSprite, size, "Size", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD_AS(NineSliceSprite, color, "Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(NineSliceSprite, fillCenter, "Fill Center")
SPARK_REFLECT_FIELD(NineSliceSprite, sortingLayer, "Sorting Layer")
SPARK_REFLECT_FIELD(NineSliceSprite, orderInLayer, "Order In Layer")
SPARK_REFLECT_END(NineSliceSprite)

SPARK_REFLECT_TYPE(PixelPerfectComponent)
SPARK_REFLECT_FIELD(PixelPerfectComponent, referenceWidth, "Reference Width")
SPARK_REFLECT_FIELD(PixelPerfectComponent, referenceHeight, "Reference Height")
SPARK_REFLECT_FIELD(PixelPerfectComponent, upscaleToFill, "Upscale To Fill")
SPARK_REFLECT_FIELD(PixelPerfectComponent, cropToFit, "Crop To Fit")
SPARK_REFLECT_FIELD(PixelPerfectComponent, currentScaleFactor, "Current Scale Factor")
SPARK_REFLECT_END(PixelPerfectComponent)

// --- Placement ---

SPARK_REFLECT_TYPE(WindZoneComponent)
SPARK_REFLECT_FIELD_AS(WindZoneComponent, direction, "Direction", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(WindZoneComponent, mainStrength, "Main Strength")
SPARK_REFLECT_FIELD(WindZoneComponent, turbulenceStrength, "Turbulence Strength")
SPARK_REFLECT_FIELD(WindZoneComponent, pulseFrequency, "Pulse Frequency")
SPARK_REFLECT_FIELD(WindZoneComponent, radius, "Radius")
SPARK_REFLECT_FIELD(WindZoneComponent, affectsParticles, "Affects Particles")
SPARK_REFLECT_FIELD(WindZoneComponent, affectsVegetation, "Affects Vegetation")
SPARK_REFLECT_FIELD(WindZoneComponent, affectsCloth, "Affects Cloth")
SPARK_REFLECT_END(WindZoneComponent)

SPARK_REFLECT_TYPE(PhysicsJointComponent)
SPARK_REFLECT_FIELD_AS(PhysicsJointComponent, anchor, "Anchor", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD_AS(PhysicsJointComponent, axis, "Axis", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(PhysicsJointComponent, lowerLimit, "Lower Limit")
SPARK_REFLECT_FIELD(PhysicsJointComponent, upperLimit, "Upper Limit")
SPARK_REFLECT_FIELD(PhysicsJointComponent, enableLimits, "Enable Limits")
SPARK_REFLECT_FIELD(PhysicsJointComponent, enableMotor, "Enable Motor")
SPARK_REFLECT_FIELD(PhysicsJointComponent, motorSpeed, "Motor Speed")
SPARK_REFLECT_FIELD(PhysicsJointComponent, motorMaxForce, "Motor Max Force")
SPARK_REFLECT_FIELD(PhysicsJointComponent, breakForce, "Break Force")
SPARK_REFLECT_FIELD(PhysicsJointComponent, breakTorque, "Break Torque")
SPARK_REFLECT_END(PhysicsJointComponent)

SPARK_REFLECT_TYPE(OccluderComponent)
SPARK_REFLECT_FIELD_AS(OccluderComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(OccluderComponent, doubleSided, "Double Sided")
SPARK_REFLECT_END(OccluderComponent)

SPARK_REFLECT_TYPE(CoverPointComponent)
SPARK_REFLECT_FIELD(CoverPointComponent, width, "Width")
SPARK_REFLECT_FIELD_AS(CoverPointComponent, coverNormal, "Cover Normal", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(CoverPointComponent, canLeanLeft, "Can Lean Left")
SPARK_REFLECT_FIELD(CoverPointComponent, canLeanRight, "Can Lean Right")
SPARK_REFLECT_FIELD(CoverPointComponent, canFireOver, "Can Fire Over")
SPARK_REFLECT_FIELD(CoverPointComponent, maxOccupants, "Max Occupants")
SPARK_REFLECT_FIELD(CoverPointComponent, currentOccupants, "Current Occupants")
SPARK_REFLECT_END(CoverPointComponent)

SPARK_REFLECT_TYPE(TacticalPointComponent)
SPARK_REFLECT_FIELD(TacticalPointComponent, qualityScore, "Quality Score")
SPARK_REFLECT_FIELD(TacticalPointComponent, radius, "Radius")
SPARK_REFLECT_FIELD(TacticalPointComponent, enabled, "Enabled")
SPARK_REFLECT_END(TacticalPointComponent)

SPARK_REFLECT_TYPE(DestructibleComponent)
SPARK_REFLECT_FIELD(DestructibleComponent, health, "Health")
SPARK_REFLECT_FIELD(DestructibleComponent, maxHealth, "Max Health")
SPARK_REFLECT_FIELD(DestructibleComponent, damageStages, "Damage Stages")
SPARK_REFLECT_FIELD(DestructibleComponent, fracturePattern, "Fracture Pattern")
SPARK_REFLECT_FIELD(DestructibleComponent, debrisLifetime, "Debris Lifetime")
SPARK_REFLECT_FIELD(DestructibleComponent, explosionForce, "Explosion Force")
SPARK_REFLECT_FIELD(DestructibleComponent, minDamageThreshold, "Min Damage Threshold")
SPARK_REFLECT_FIELD(DestructibleComponent, generateColliders, "Generate Colliders")
SPARK_REFLECT_FIELD(DestructibleComponent, chainReaction, "Chain Reaction")
SPARK_REFLECT_FIELD(DestructibleComponent, currentStage, "Current Stage")
SPARK_REFLECT_END(DestructibleComponent)

SPARK_REFLECT_TYPE(CinematicTriggerComponent)
SPARK_REFLECT_FIELD(CinematicTriggerComponent, sequenceName, "Sequence Name")
SPARK_REFLECT_FIELD(CinematicTriggerComponent, radius, "Radius")
SPARK_REFLECT_FIELD(CinematicTriggerComponent, playOnce, "Play Once")
SPARK_REFLECT_FIELD(CinematicTriggerComponent, skipable, "Skipable")
SPARK_REFLECT_FIELD(CinematicTriggerComponent, pauseGameplay, "Pause Gameplay")
SPARK_REFLECT_FIELD(CinematicTriggerComponent, hasTriggered, "Has Triggered")
SPARK_REFLECT_END(CinematicTriggerComponent)

SPARK_REFLECT_TYPE(DialogueTriggerComponent)
SPARK_REFLECT_FIELD(DialogueTriggerComponent, dialogueTreeName, "Dialogue Tree Name")
SPARK_REFLECT_FIELD(DialogueTriggerComponent, speakerName, "Speaker Name")
SPARK_REFLECT_FIELD(DialogueTriggerComponent, interactionRadius, "Interaction Radius")
SPARK_REFLECT_FIELD(DialogueTriggerComponent, requiresInteract, "Requires Interact")
SPARK_REFLECT_FIELD(DialogueTriggerComponent, oneShot, "One Shot")
SPARK_REFLECT_FIELD(DialogueTriggerComponent, facePlayer, "Face Player")
SPARK_REFLECT_END(DialogueTriggerComponent)

SPARK_REFLECT_TYPE(AreaBoundaryComponent)
SPARK_REFLECT_FIELD(AreaBoundaryComponent, areaName, "Area Name")
SPARK_REFLECT_FIELD(AreaBoundaryComponent, scenePath, "Scene Path")
SPARK_REFLECT_FIELD_AS(AreaBoundaryComponent, boundsMin, "Bounds Min", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD_AS(AreaBoundaryComponent, boundsMax, "Bounds Max", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(AreaBoundaryComponent, priority, "Priority")
SPARK_REFLECT_FIELD(AreaBoundaryComponent, loadRadius, "Load Radius")
SPARK_REFLECT_FIELD(AreaBoundaryComponent, unloadRadius, "Unload Radius")
SPARK_REFLECT_FIELD(AreaBoundaryComponent, alwaysLoaded, "Always Loaded")
SPARK_REFLECT_END(AreaBoundaryComponent)

SPARK_REFLECT_TYPE(BillboardComponent)
SPARK_REFLECT_FIELD(BillboardComponent, texturePath, "Texture Path")
SPARK_REFLECT_FIELD_AS(BillboardComponent, color, "Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD_AS(BillboardComponent, size, "Size", Spark::FieldType::Vector2)
SPARK_REFLECT_FIELD(BillboardComponent, fadeStartDistance, "Fade Start Distance")
SPARK_REFLECT_FIELD(BillboardComponent, fadeEndDistance, "Fade End Distance")
SPARK_REFLECT_FIELD(BillboardComponent, sortingLayer, "Sorting Layer")
SPARK_REFLECT_END(BillboardComponent)

// --- Advanced Placement ---

SPARK_REFLECT_TYPE(AudioListenerComponent)
SPARK_REFLECT_FIELD(AudioListenerComponent, isActive, "Is Active")
SPARK_REFLECT_FIELD(AudioListenerComponent, volumeScale, "Volume Scale")
SPARK_REFLECT_END(AudioListenerComponent)

SPARK_REFLECT_TYPE(CharacterControllerComponent)
SPARK_REFLECT_FIELD(CharacterControllerComponent, height, "Height")
SPARK_REFLECT_FIELD(CharacterControllerComponent, radius, "Radius")
SPARK_REFLECT_FIELD(CharacterControllerComponent, stepHeight, "Step Height")
SPARK_REFLECT_FIELD(CharacterControllerComponent, slopeLimit, "Slope Limit")
SPARK_REFLECT_FIELD(CharacterControllerComponent, skinWidth, "Skin Width")
SPARK_REFLECT_FIELD(CharacterControllerComponent, gravity, "Gravity")
SPARK_REFLECT_FIELD(CharacterControllerComponent, moveSpeed, "Move Speed")
SPARK_REFLECT_FIELD(CharacterControllerComponent, jumpForce, "Jump Force")
SPARK_REFLECT_FIELD(CharacterControllerComponent, groundState, "Ground State")
SPARK_REFLECT_END(CharacterControllerComponent)

SPARK_REFLECT_TYPE(NavRegionComponent)
SPARK_REFLECT_FIELD_AS(NavRegionComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(NavRegionComponent, agentRadius, "Agent Radius")
SPARK_REFLECT_FIELD(NavRegionComponent, agentHeight, "Agent Height")
SPARK_REFLECT_FIELD(NavRegionComponent, maxSlope, "Max Slope")
SPARK_REFLECT_FIELD(NavRegionComponent, cellSize, "Cell Size")
SPARK_REFLECT_FIELD(NavRegionComponent, autoRebuild, "Auto Rebuild")
SPARK_REFLECT_END(NavRegionComponent)

SPARK_REFLECT_TYPE(NavLinkComponent)
SPARK_REFLECT_FIELD_AS(NavLinkComponent, endOffset, "End Offset", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(NavLinkComponent, radius, "Radius")
SPARK_REFLECT_FIELD(NavLinkComponent, traversalCost, "Traversal Cost")
SPARK_REFLECT_FIELD(NavLinkComponent, bidirectional, "Bidirectional")
SPARK_REFLECT_FIELD(NavLinkComponent, enabled, "Enabled")
SPARK_REFLECT_FIELD(NavLinkComponent, animationName, "Animation Name")
SPARK_REFLECT_END(NavLinkComponent)

SPARK_REFLECT_TYPE(SkyboxComponent)
SPARK_REFLECT_FIELD(SkyboxComponent, cubemapPath, "Cubemap Path")
SPARK_REFLECT_FIELD_AS(SkyboxComponent, topColor, "Top Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD_AS(SkyboxComponent, bottomColor, "Bottom Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(SkyboxComponent, turbidity, "Turbidity")
SPARK_REFLECT_FIELD(SkyboxComponent, sunSize, "Sun Size")
SPARK_REFLECT_FIELD(SkyboxComponent, exposure, "Exposure")
SPARK_REFLECT_FIELD(SkyboxComponent, rotation, "Rotation")
SPARK_REFLECT_END(SkyboxComponent)

SPARK_REFLECT_TYPE(ConstantForceComponent)
SPARK_REFLECT_FIELD_AS(ConstantForceComponent, force, "Force", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD_AS(ConstantForceComponent, torque, "Torque", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(ConstantForceComponent, relativeForce, "Relative Force")
SPARK_REFLECT_FIELD(ConstantForceComponent, relativeTorque, "Relative Torque")
SPARK_REFLECT_FIELD(ConstantForceComponent, enabled, "Enabled")
SPARK_REFLECT_END(ConstantForceComponent)

SPARK_REFLECT_TYPE(ForceRegionComponent)
SPARK_REFLECT_FIELD_AS(ForceRegionComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD_AS(ForceRegionComponent, forceDirection, "Force Direction", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(ForceRegionComponent, forceMagnitude, "Force Magnitude")
SPARK_REFLECT_FIELD(ForceRegionComponent, damping, "Damping")
SPARK_REFLECT_FIELD(ForceRegionComponent, enabled, "Enabled")
SPARK_REFLECT_END(ForceRegionComponent)

SPARK_REFLECT_TYPE(RagdollComponent)
SPARK_REFLECT_FIELD(RagdollComponent, definitionName, "Definition Name")
SPARK_REFLECT_FIELD(RagdollComponent, blendWeight, "Blend Weight")
SPARK_REFLECT_FIELD(RagdollComponent, jointStiffness, "Joint Stiffness")
SPARK_REFLECT_FIELD(RagdollComponent, linearDamping, "Linear Damping")
SPARK_REFLECT_FIELD(RagdollComponent, angularDamping, "Angular Damping")
SPARK_REFLECT_FIELD(RagdollComponent, selfCollision, "Self Collision")
SPARK_REFLECT_END(RagdollComponent)

SPARK_REFLECT_TYPE(SoftBodyComponent)
SPARK_REFLECT_FIELD(SoftBodyComponent, mass, "Mass")
SPARK_REFLECT_FIELD(SoftBodyComponent, stiffness, "Stiffness")
SPARK_REFLECT_FIELD(SoftBodyComponent, damping, "Damping")
SPARK_REFLECT_FIELD(SoftBodyComponent, windInfluence, "Wind Influence")
SPARK_REFLECT_FIELD(SoftBodyComponent, gravityScale, "Gravity Scale")
SPARK_REFLECT_FIELD(SoftBodyComponent, solverIterations, "Solver Iterations")
SPARK_REFLECT_FIELD(SoftBodyComponent, selfCollision, "Self Collision")
SPARK_REFLECT_FIELD(SoftBodyComponent, twoSided, "Two Sided")
SPARK_REFLECT_END(SoftBodyComponent)

SPARK_REFLECT_TYPE(VehicleComponent)
SPARK_REFLECT_FIELD(VehicleComponent, wheelCount, "Wheel Count")
SPARK_REFLECT_FIELD(VehicleComponent, mass, "Mass")
SPARK_REFLECT_FIELD(VehicleComponent, maxEngineTorque, "Max Engine Torque")
SPARK_REFLECT_FIELD(VehicleComponent, maxSteerAngle, "Max Steer Angle")
SPARK_REFLECT_FIELD(VehicleComponent, maxBrakeForce, "Max Brake Force")
SPARK_REFLECT_FIELD(VehicleComponent, suspensionLength, "Suspension Length")
SPARK_REFLECT_FIELD(VehicleComponent, suspensionStiffness, "Suspension Stiffness")
SPARK_REFLECT_FIELD(VehicleComponent, suspensionDamping, "Suspension Damping")
SPARK_REFLECT_FIELD(VehicleComponent, gearCount, "Gear Count")
SPARK_REFLECT_FIELD(VehicleComponent, antiRollBar, "Anti Roll Bar")
SPARK_REFLECT_END(VehicleComponent)

SPARK_REFLECT_TYPE(BuoyancyVolumeComponent)
SPARK_REFLECT_FIELD_AS(BuoyancyVolumeComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(BuoyancyVolumeComponent, waterDensity, "Water Density")
SPARK_REFLECT_FIELD(BuoyancyVolumeComponent, linearDrag, "Linear Drag")
SPARK_REFLECT_FIELD(BuoyancyVolumeComponent, angularDrag, "Angular Drag")
SPARK_REFLECT_FIELD(BuoyancyVolumeComponent, flowSpeed, "Flow Speed")
SPARK_REFLECT_FIELD_AS(BuoyancyVolumeComponent, flowDirection, "Flow Direction", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(BuoyancyVolumeComponent, enabled, "Enabled")
SPARK_REFLECT_END(BuoyancyVolumeComponent)

SPARK_REFLECT_TYPE(SpringArmComponent)
SPARK_REFLECT_FIELD(SpringArmComponent, targetLength, "Target Length")
SPARK_REFLECT_FIELD(SpringArmComponent, probeRadius, "Probe Radius")
SPARK_REFLECT_FIELD(SpringArmComponent, smoothSpeed, "Smooth Speed")
SPARK_REFLECT_FIELD(SpringArmComponent, minLength, "Min Length")
SPARK_REFLECT_FIELD(SpringArmComponent, doCollisionTest, "Do Collision Test")
SPARK_REFLECT_FIELD(SpringArmComponent, currentLength, "Current Length")
SPARK_REFLECT_FIELD(SpringArmComponent, isColliding, "Is Colliding")
SPARK_REFLECT_END(SpringArmComponent)

SPARK_REFLECT_TYPE(TrailRendererComponent)
SPARK_REFLECT_FIELD(TrailRendererComponent, lifetime, "Lifetime")
SPARK_REFLECT_FIELD(TrailRendererComponent, minVertexDistance, "Min Vertex Distance")
SPARK_REFLECT_FIELD(TrailRendererComponent, startWidth, "Start Width")
SPARK_REFLECT_FIELD(TrailRendererComponent, endWidth, "End Width")
SPARK_REFLECT_FIELD_AS(TrailRendererComponent, startColor, "Start Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD_AS(TrailRendererComponent, endColor, "End Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(TrailRendererComponent, materialPath, "Material Path")
SPARK_REFLECT_FIELD(TrailRendererComponent, emitting, "Emitting")
SPARK_REFLECT_FIELD(TrailRendererComponent, sortingLayer, "Sorting Layer")
SPARK_REFLECT_END(TrailRendererComponent)

SPARK_REFLECT_TYPE(Text3DComponent)
SPARK_REFLECT_FIELD(Text3DComponent, text, "Text")
SPARK_REFLECT_FIELD(Text3DComponent, fontPath, "Font Path")
SPARK_REFLECT_FIELD(Text3DComponent, fontSize, "Font Size")
SPARK_REFLECT_FIELD_AS(Text3DComponent, color, "Color", Spark::FieldType::Vector4)
SPARK_REFLECT_FIELD(Text3DComponent, faceCamera, "Face Camera")
SPARK_REFLECT_FIELD(Text3DComponent, castShadows, "Cast Shadows")
SPARK_REFLECT_FIELD(Text3DComponent, maxWidth, "Max Width")
SPARK_REFLECT_FIELD(Text3DComponent, sortingLayer, "Sorting Layer")
SPARK_REFLECT_END(Text3DComponent)

SPARK_REFLECT_TYPE(FoliageVolumeComponent)
SPARK_REFLECT_FIELD_AS(FoliageVolumeComponent, halfExtents, "Half Extents", Spark::FieldType::Vector3)
SPARK_REFLECT_FIELD(FoliageVolumeComponent, seed, "Seed")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, densityScale, "Density Scale")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, minSlopeAngle, "Min Slope Angle")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, maxSlopeAngle, "Max Slope Angle")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, minAltitude, "Min Altitude")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, maxAltitude, "Max Altitude")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, alignToSurface, "Align To Surface")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, castShadows, "Cast Shadows")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, cullDistance, "Cull Distance")
SPARK_REFLECT_FIELD(FoliageVolumeComponent, enabled, "Enabled")
SPARK_REFLECT_END(FoliageVolumeComponent)

// --- Collision Mask ---

SPARK_REFLECT_TYPE(CollisionMaskComponent)
SPARK_REFLECT_FIELD(CollisionMaskComponent, fromMask, "From Mask")
SPARK_REFLECT_FIELD(CollisionMaskComponent, intoMask, "Into Mask")
SPARK_REFLECT_END(CollisionMaskComponent)

// --- Visibility ---

SPARK_REFLECT_TYPE(VisibilityMaskComponent)
SPARK_REFLECT_FIELD(VisibilityMaskComponent, mask, "Mask")
SPARK_REFLECT_END(VisibilityMaskComponent)

SPARK_REFLECT_TYPE(CameraDrawMaskComponent)
SPARK_REFLECT_FIELD(CameraDrawMaskComponent, drawMask, "Draw Mask")
SPARK_REFLECT_END(CameraDrawMaskComponent)

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

            // 2D / Sprite
            SPARK_REGISTER_COMPONENT(SpriteRenderer)
            SPARK_REGISTER_COMPONENT(SpriteAnimator)
            SPARK_REGISTER_COMPONENT(Camera2D)
            SPARK_REGISTER_COMPONENT(ParallaxBackground)
            SPARK_REGISTER_COMPONENT(TilemapComponent)
            SPARK_REGISTER_COMPONENT(RigidBody2D)
            SPARK_REGISTER_COMPONENT(Collider2D)
            SPARK_REGISTER_COMPONENT(NineSliceSprite)
            SPARK_REGISTER_COMPONENT(PixelPerfectComponent)

            // Placement
            SPARK_REGISTER_COMPONENT(WindZoneComponent)
            SPARK_REGISTER_COMPONENT(PhysicsJointComponent)
            SPARK_REGISTER_COMPONENT(OccluderComponent)
            SPARK_REGISTER_COMPONENT(CoverPointComponent)
            SPARK_REGISTER_COMPONENT(TacticalPointComponent)
            SPARK_REGISTER_COMPONENT(DestructibleComponent)
            SPARK_REGISTER_COMPONENT(CinematicTriggerComponent)
            SPARK_REGISTER_COMPONENT(DialogueTriggerComponent)
            SPARK_REGISTER_COMPONENT(AreaBoundaryComponent)
            SPARK_REGISTER_COMPONENT(BillboardComponent)

            // Advanced Placement
            SPARK_REGISTER_COMPONENT(AudioListenerComponent)
            SPARK_REGISTER_COMPONENT(CharacterControllerComponent)
            SPARK_REGISTER_COMPONENT(NavRegionComponent)
            SPARK_REGISTER_COMPONENT(NavLinkComponent)
            SPARK_REGISTER_COMPONENT(SkyboxComponent)
            SPARK_REGISTER_COMPONENT(ConstantForceComponent)
            SPARK_REGISTER_COMPONENT(ForceRegionComponent)
            SPARK_REGISTER_COMPONENT(RagdollComponent)
            SPARK_REGISTER_COMPONENT(SoftBodyComponent)
            SPARK_REGISTER_COMPONENT(VehicleComponent)
            SPARK_REGISTER_COMPONENT(BuoyancyVolumeComponent)
            SPARK_REGISTER_COMPONENT(SpringArmComponent)
            SPARK_REGISTER_COMPONENT(TrailRendererComponent)
            SPARK_REGISTER_COMPONENT(Text3DComponent)
            SPARK_REGISTER_COMPONENT(FoliageVolumeComponent)

            // Collision / Visibility
            SPARK_REGISTER_COMPONENT(CollisionMaskComponent)
            SPARK_REGISTER_COMPONENT(VisibilityMaskComponent)
            SPARK_REGISTER_COMPONENT(CameraDrawMaskComponent)

            SPARK_LOG_INFO(Spark::LogCategory::Core, "ComponentFactory registration complete");
        }
    };

    static ComponentFactoryRegistrar s_componentFactoryRegistrar;

} // anonymous namespace
