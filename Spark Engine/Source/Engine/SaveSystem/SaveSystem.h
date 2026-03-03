/**
 * @file SaveSystem.h
 * @brief Game state serialization and save/load system
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides complete game state persistence:
 * - Full ECS world snapshot (all entities and components)
 * - Save file versioning and migration
 * - Autosave and quicksave/quickload
 * - Save slot management
 * - Compressed save files
 */

#pragma once
#include "../../Core/Platform.h"

#include "../ECS/Components.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>
#include <cstdint>

namespace Spark {

// ============================================================================
// Save Data Types
// ============================================================================

struct SaveMetadata {
    std::string saveName;
    std::string sceneName;
    std::string playerClass;
    uint32_t version = 1;
    uint64_t timestamp = 0;  ///< Unix timestamp
    float playTime = 0.0f;   ///< Total play time in seconds
    std::string screenshotPath;

    /// Player summary for UI display
    float playerHealth = 0.0f;
    float playerArmor = 0.0f;
    DirectX::XMFLOAT3 playerPosition{0, 0, 0};
    int playerKills = 0;
    int playerDeaths = 0;
};

struct SerializedComponent {
    std::string typeName;
    std::unordered_map<std::string, std::string> properties;  ///< Key-value pairs as strings
};

struct SerializedEntity {
    uint32_t entityID;
    std::string name;
    std::vector<SerializedComponent> components;
};

struct SaveData {
    SaveMetadata metadata;
    std::vector<SerializedEntity> entities;

    /// Custom game state (key-value pairs for game-specific data)
    std::unordered_map<std::string, std::string> customState;
};

// ============================================================================
// Component Serializer Registry
// ============================================================================

class ComponentSerializerRegistry {
public:
    using SerializeFunc = std::function<SerializedComponent(const void* component)>;
    using DeserializeFunc = std::function<void(World& world, EntityID entity, const SerializedComponent& data)>;

    static ComponentSerializerRegistry& GetInstance();

    void Register(const std::string& typeName, SerializeFunc serialize, DeserializeFunc deserialize);

    bool HasSerializer(const std::string& typeName) const;
    SerializedComponent Serialize(const std::string& typeName, const void* component) const;
    void Deserialize(const std::string& typeName, World& world, EntityID entity,
                     const SerializedComponent& data) const;

    /// Register all built-in component serializers
    void RegisterBuiltins();

private:
    ComponentSerializerRegistry() = default;
    struct Entry {
        SerializeFunc serialize;
        DeserializeFunc deserialize;
    };
    std::unordered_map<std::string, Entry> m_serializers;
};

// ============================================================================
// SaveSystem
// ============================================================================

class SaveSystem {
public:
    static SaveSystem& GetInstance();

    /// Initialize the save system (creates save directory if needed)
    bool Initialize(const std::string& saveDirectory = "Saves");

    /// Save the current game state
    bool Save(const std::string& slotName, World& world, const SaveMetadata& metadata);

    /// Load a saved game state
    bool Load(const std::string& slotName, World& world);

    /// Quick save (uses a dedicated slot)
    bool QuickSave(World& world, const SaveMetadata& metadata);

    /// Quick load
    bool QuickLoad(World& world);

    /// Autosave (rotating slots)
    bool AutoSave(World& world, const SaveMetadata& metadata);

    /// Delete a save slot
    bool DeleteSave(const std::string& slotName);

    /// Get list of all save slots
    std::vector<SaveMetadata> GetSaveSlots() const;

    /// Get metadata for a specific save
    bool GetSaveMetadata(const std::string& slotName, SaveMetadata& outMetadata) const;

    /// Check if a save slot exists
    bool SaveExists(const std::string& slotName) const;

    /// Serialize world to SaveData (without writing to disk)
    SaveData SerializeWorld(World& world, const SaveMetadata& metadata) const;

    /// Deserialize SaveData into a world (without reading from disk)
    bool DeserializeWorld(const SaveData& data, World& world) const;

    // Configuration
    void SetMaxAutoSaves(int count) { m_maxAutoSaves = count; }
    void SetSaveDirectory(const std::string& dir) { m_saveDirectory = dir; }

    /// Console integration
    std::string Console_ListSaves() const;
    std::string Console_GetSaveInfo(const std::string& slotName) const;

private:
    SaveSystem() = default;

    bool WriteToFile(const std::string& filepath, const SaveData& data) const;
    bool ReadFromFile(const std::string& filepath, SaveData& outData) const;
    std::string GetSavePath(const std::string& slotName) const;

    std::string m_saveDirectory = "Saves";
    int m_maxAutoSaves = 3;
    int m_currentAutoSaveIndex = 0;
    std::string m_quickSaveSlot = "__quicksave";
};

} // namespace Spark
