/**
 * @file EntityPresetManager.cpp
 * @brief Implementation of entity preset registry with built-in game object templates
 */

#include "EntityPresetManager.h"
#include "Utils/SparkConsole.h"

namespace Spark::ECS
{

    EntityPresetManager& EntityPresetManager::GetInstance()
    {
        static EntityPresetManager instance;
        return instance;
    }

    void EntityPresetManager::Initialize()
    {
        RegisterBuiltinPresets();

        auto& console = SimpleConsole::GetInstance();
        console.LogInfo("[EntityPresetManager] Initialized with " + std::to_string(m_presets.size()) + " presets");
    }

    void EntityPresetManager::RegisterPreset(EntityPreset preset)
    {
        m_presets.push_back(std::move(preset));
    }

    const EntityPreset* EntityPresetManager::FindPreset(const std::string& name) const
    {
        for (const auto& preset : m_presets)
        {
            if (preset.name == name)
            {
                return &preset;
            }
        }
        return nullptr;
    }

    std::unordered_map<std::string, std::vector<std::string>> EntityPresetManager::GetPresetsByCategory() const
    {
        std::unordered_map<std::string, std::vector<std::string>> result;
        for (const auto& preset : m_presets)
        {
            result[preset.category].push_back(preset.name);
        }
        return result;
    }

    int EntityPresetManager::LoadPresetsFromDirectory(const std::string& /*directory*/)
    {
        // JSON loading — requires JSON parser. For now, presets are registered programmatically.
        return 0;
    }

    void EntityPresetManager::RegisterBuiltinPresets()
    {
        // ================================================================
        // Characters
        // ================================================================

        RegisterPreset({
            "Player",
            "Character",
            "Player character with controller, camera, and health",
            {
                {"Transform", {{"position", "0,0,0"}, {"rotation", "0,0,0"}, {"scale", "1,1,1"}}},
                {"HealthComponent", {{"maxHealth", "100"}, {"health", "100"}}},
                {"CharacterControllerComponent", {{"speed", "5.0"}, {"jumpForce", "8.0"}}},
                {"Camera", {{"fov", "75"}, {"near", "0.1"}, {"far", "1000"}}},
                {"ColliderComponent", {{"type", "capsule"}, {"radius", "0.5"}, {"height", "1.8"}}},
            },
        });

        RegisterPreset({
            "Enemy",
            "Character",
            "AI-controlled enemy with health, pathfinding, and death loot drop",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "Enemy"}}},
                {"HealthComponent", {{"maxHealth", "50"}, {"health", "50"}}},
                {"AIComponent", {{"behaviorTree", "patrol_and_chase"}, {"detectionRadius", "15.0"}}},
                {"MeshRenderer", {{"mesh", "models/enemy_default.mesh"}}},
                {"ColliderComponent", {{"type", "capsule"}, {"radius", "0.5"}, {"height", "1.8"}}},
                {"RigidBodyComponent", {{"mass", "70"}, {"kinematic", "false"}}},
            },
        });

        RegisterPreset({
            "NPC",
            "Character",
            "Non-player character with dialogue trigger",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "NPC"}}},
                {"MeshRenderer", {{"mesh", "models/npc_default.mesh"}}},
                {"AIComponent", {{"behaviorTree", "idle"}}},
                {"TriggerVolumeComponent", {{"shape", "sphere"}, {"radius", "3.0"}}},
            },
        });

        // ================================================================
        // Props
        // ================================================================

        RegisterPreset({
            "Door",
            "Prop",
            "Interactive door that opens on trigger enter (with optional key requirement)",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "Door"}}},
                {"MeshRenderer", {{"mesh", "models/door.mesh"}}},
                {"AnimationController", {{"defaultAnim", "door_closed"}}},
                {"TriggerVolumeComponent", {{"shape", "box"}, {"halfExtents", "2,2,1"}}},
                {"AudioSourceComponent", {{"clip", "audio/door_open.wav"}}},
            },
        });

        RegisterPreset({
            "Chest",
            "Prop",
            "Treasure chest that opens on interaction and spawns loot",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "Chest"}}},
                {"MeshRenderer", {{"mesh", "models/chest.mesh"}}},
                {"AnimationController", {{"defaultAnim", "chest_closed"}}},
                {"TriggerVolumeComponent", {{"shape", "sphere"}, {"radius", "2.0"}}},
            },
        });

        RegisterPreset({
            "Destructible",
            "Prop",
            "Object that breaks when health reaches zero",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "Destructible"}}},
                {"MeshRenderer", {{"mesh", "models/crate.mesh"}}},
                {"HealthComponent", {{"maxHealth", "25"}, {"health", "25"}}},
                {"ColliderComponent", {{"type", "box"}, {"halfExtents", "0.5,0.5,0.5"}}},
                {"RigidBodyComponent", {{"mass", "10"}}},
            },
        });

        // ================================================================
        // Pickups
        // ================================================================

        RegisterPreset({
            "Health Pickup",
            "Pickup",
            "Restores health on contact, then destroys itself",
            {
                {"Transform", {{"position", "0,0.5,0"}}},
                {"NameComponent", {{"name", "HealthPickup"}}},
                {"MeshRenderer", {{"mesh", "models/pickup_health.mesh"}}},
                {"TriggerVolumeComponent", {{"shape", "sphere"}, {"radius", "1.0"}}},
                {"ParticleEmitterComponent", {{"preset", "glow_green"}}},
            },
        });

        RegisterPreset({
            "Ammo Pickup",
            "Pickup",
            "Grants ammunition on contact",
            {
                {"Transform", {{"position", "0,0.5,0"}}},
                {"NameComponent", {{"name", "AmmoPickup"}}},
                {"MeshRenderer", {{"mesh", "models/pickup_ammo.mesh"}}},
                {"TriggerVolumeComponent", {{"shape", "sphere"}, {"radius", "1.0"}}},
                {"ParticleEmitterComponent", {{"preset", "glow_yellow"}}},
            },
        });

        RegisterPreset({
            "Key Item",
            "Pickup",
            "Collectible key that unlocks doors or chests",
            {
                {"Transform", {{"position", "0,1,0"}}},
                {"NameComponent", {{"name", "Key"}}},
                {"MeshRenderer", {{"mesh", "models/key.mesh"}}},
                {"TriggerVolumeComponent", {{"shape", "sphere"}, {"radius", "1.5"}}},
                {"ParticleEmitterComponent", {{"preset", "glow_blue"}}},
            },
        });

        // ================================================================
        // Effects
        // ================================================================

        RegisterPreset({
            "Sound Source",
            "Effect",
            "3D positioned audio emitter",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "SoundSource"}}},
                {"AudioSourceComponent", {{"clip", ""}, {"volume", "1.0"}, {"loop", "true"}, {"range", "20.0"}}},
            },
        });

        RegisterPreset({
            "Particle Effect",
            "Effect",
            "Particle system emitter",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "ParticleEffect"}}},
                {"ParticleEmitterComponent", {{"preset", "default"}}},
            },
        });

        RegisterPreset({
            "Spawn Point",
            "Effect",
            "Marks a respawn location for players or enemies",
            {
                {"Transform", {{"position", "0,0,0"}}},
                {"NameComponent", {{"name", "SpawnPoint"}}},
                {"TagComponent", {{"tag", "SpawnPoint"}}},
            },
        });
    }

} // namespace Spark::ECS
