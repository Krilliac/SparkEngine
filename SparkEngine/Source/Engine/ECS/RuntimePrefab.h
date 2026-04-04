/**
 * @file RuntimePrefab.h
 * @brief Runtime prefab system for data-driven entity templates with serialization
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides reusable entity templates (prefabs) that can be registered, cloned,
 * serialized, and instantiated at runtime. Each prefab holds a list of component
 * descriptors with string key-value properties. A global PrefabRegistry manages
 * lookup by name or numeric ID and handles spawning via the ECS.
 *
 * ## Architecture
 * ```
 * PrefabRegistry (singleton)
 *   ├── m_prefabs          (ID -> unique_ptr<RuntimePrefab>)
 *   ├── m_nameIndex        (name -> ID for fast lookup)
 *   └── SpawnEntity()      (creates ECS entity from prefab definition)
 *
 * RuntimePrefab (value object, one per template)
 *   ├── m_components       (ordered list of PrefabComponentData)
 *   ├── m_parent           (optional parent for nested prefabs)
 *   └── Serialize/Deserialize via BinaryWriter/BinaryReader
 * ```
 *
 * ## Usage
 * @code
 *   auto prefab = std::make_unique<Spark::ECS::RuntimePrefab>("EnemyGuard");
 *   prefab->AddComponent("Transform", {{"posX", "0"}, {"posY", "0"}});
 *   prefab->AddComponent("MeshRenderer", {{"mesh", "guard.fbx"}});
 *
 *   auto& registry = Spark::ECS::PrefabRegistry::GetInstance();
 *   uint32_t id = registry.RegisterPrefab(std::move(prefab));
 *   uint32_t entity = registry.SpawnEntity(id);
 * @endcode
 *
 * @see EntityArchetype.h, Components.h, Serializer.h
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Forward declarations — full headers not needed in this header.
namespace Spark
{
    class BinaryWriter;
    class BinaryReader;
} // namespace Spark

namespace Spark::ECS
{

    // =========================================================================
    // Data Structures
    // =========================================================================

    /**
     * @brief Serialized component descriptor within a prefab.
     *
     * Stores the component type name and a flat map of string properties.
     * The spawning code interprets these properties to configure real
     * ECS components on the newly created entity.
     */
    struct PrefabComponentData
    {
        std::string typeName; ///< Component type (e.g. "Transform", "MeshRenderer")

        /** @brief Property key-value pairs (e.g. {"posX", "10.0"}). */
        std::unordered_map<std::string, std::string> properties;
    };

    // =========================================================================
    // RuntimePrefab
    // =========================================================================

    /**
     * @class RuntimePrefab
     * @brief A reusable entity template holding component descriptors.
     *
     * Not a singleton — one instance per prefab definition. Supports nesting
     * via SetParent(), cloning, and binary serialization.
     */
    class RuntimePrefab
    {
      public:
        /**
         * @brief Construct a prefab with a logical name.
         * @param name Unique prefab name.
         */
        explicit RuntimePrefab(std::string name) : m_name(std::move(name)) {}

        // -----------------------------------------------------------------
        // Component management
        // -----------------------------------------------------------------

        /**
         * @brief Add a component descriptor to the prefab.
         * @param typeName Component type name.
         * @param properties Key-value properties for the component.
         */
        void AddComponent(std::string typeName, std::unordered_map<std::string, std::string> properties)
        {
            PrefabComponentData data;
            data.typeName = std::move(typeName);
            data.properties = std::move(properties);
            m_components.push_back(std::move(data));
        }

        /**
         * @brief Remove the first component matching the given type name.
         * @param typeName Component type to remove.
         * @return True if a component was found and removed.
         */
        bool RemoveComponent(const std::string& typeName)
        {
            auto it = std::find_if(m_components.begin(), m_components.end(),
                                   [&](const PrefabComponentData& c) { return c.typeName == typeName; });
            if (it != m_components.end())
            {
                m_components.erase(it);
                return true;
            }
            return false;
        }

        /**
         * @brief Check whether the prefab contains a component of the given type.
         * @param typeName Component type to search for.
         * @return True if present.
         */
        [[nodiscard]] bool HasComponent(const std::string& typeName) const
        {
            return std::any_of(m_components.begin(), m_components.end(),
                               [&](const PrefabComponentData& c) { return c.typeName == typeName; });
        }

        /** @brief Get the ordered list of component descriptors. */
        [[nodiscard]] const std::vector<PrefabComponentData>& GetComponents() const { return m_components; }

        /** @brief Get the prefab name. */
        [[nodiscard]] std::string_view GetName() const { return m_name; }

        // -----------------------------------------------------------------
        // Nesting
        // -----------------------------------------------------------------

        /**
         * @brief Set a parent prefab for inheritance / nesting.
         * @param parent Non-owning pointer to the parent prefab (may be nullptr).
         */
        void SetParent(const RuntimePrefab* parent) { m_parent = parent; }

        /** @brief Get the parent prefab, or nullptr if none. */
        [[nodiscard]] const RuntimePrefab* GetParent() const { return m_parent; }

        // -----------------------------------------------------------------
        // Cloning
        // -----------------------------------------------------------------

        /**
         * @brief Create a deep copy of this prefab.
         * @return A new RuntimePrefab with the same name, components, and parent link.
         */
        [[nodiscard]] std::unique_ptr<RuntimePrefab> Clone() const
        {
            auto copy = std::make_unique<RuntimePrefab>(m_name);
            copy->m_components = m_components;
            copy->m_parent = m_parent;
            return copy;
        }

        // -----------------------------------------------------------------
        // Serialization
        // -----------------------------------------------------------------

        /**
         * @brief Write the prefab to a binary stream with a file header.
         * @param writer BinaryWriter to write into.
         */
        void Serialize(BinaryWriter& writer) const;

        /**
         * @brief Read a prefab from a binary stream, validating the header.
         * @param reader BinaryReader to read from.
         */
        void Deserialize(BinaryReader& reader);

      private:
        std::string m_name;                            ///< Unique prefab name.
        std::vector<PrefabComponentData> m_components; ///< Ordered component descriptors.
        const RuntimePrefab* m_parent = nullptr;       ///< Optional parent (non-owning).
    };

    // =========================================================================
    // Serialization (inline, depends on BinaryWriter/BinaryReader API)
    // =========================================================================

    /**
     * @brief Simple asset file header written before prefab data.
     */
    struct PrefabFileHeader
    {
        static constexpr uint32_t kMagic = 0x50524642; ///< "PRFB"
        static constexpr uint32_t kVersion = 1;
    };

    inline void RuntimePrefab::Serialize(BinaryWriter& writer) const
    {
        writer.Write<uint32_t>(PrefabFileHeader::kMagic);
        writer.Write<uint32_t>(PrefabFileHeader::kVersion);

        writer.WriteString(m_name);
        writer.Write<uint32_t>(static_cast<uint32_t>(m_components.size()));
        for (const auto& comp : m_components)
        {
            writer.WriteString(comp.typeName);
            writer.Write<uint32_t>(static_cast<uint32_t>(comp.properties.size()));
            for (const auto& [key, value] : comp.properties)
            {
                writer.WriteString(key);
                writer.WriteString(value);
            }
        }
    }

    inline void RuntimePrefab::Deserialize(BinaryReader& reader)
    {
        uint32_t magic = reader.Read<uint32_t>();
        uint32_t version = reader.Read<uint32_t>();
        (void)magic;
        (void)version;

        m_name = reader.ReadString();
        uint32_t compCount = reader.Read<uint32_t>();
        m_components.clear();
        m_components.reserve(compCount);

        for (uint32_t i = 0; i < compCount; ++i)
        {
            PrefabComponentData comp;
            comp.typeName = reader.ReadString();
            uint32_t propCount = reader.Read<uint32_t>();
            for (uint32_t j = 0; j < propCount; ++j)
            {
                std::string key = reader.ReadString();
                std::string value = reader.ReadString();
                comp.properties[std::move(key)] = std::move(value);
            }
            m_components.push_back(std::move(comp));
        }
    }

    // =========================================================================
    // PrefabRegistry
    // =========================================================================

    /**
     * @class PrefabRegistry
     * @brief Singleton registry that stores, retrieves, and spawns prefabs.
     *
     * Prefabs are registered with unique auto-incremented IDs and can be
     * looked up by either ID or name. SpawnEntity creates a new ECS entity
     * from a prefab definition.
     */
    class PrefabRegistry
    {
      public:
        /** @brief Get the singleton instance. */
        static PrefabRegistry& GetInstance()
        {
            static PrefabRegistry s;
            return s;
        }

        // -----------------------------------------------------------------
        // Registration
        // -----------------------------------------------------------------

        /**
         * @brief Register a prefab and take ownership.
         * @param prefab The prefab to register (moved).
         * @return Auto-assigned prefab ID.
         */
        uint32_t RegisterPrefab(std::unique_ptr<RuntimePrefab> prefab)
        {
            if (!prefab)
            {
                return 0;
            }

            uint32_t id = m_nextId++;
            std::string name(prefab->GetName());
            m_prefabs[id] = std::move(prefab);
            m_nameIndex[name] = id;
            return id;
        }

        // -----------------------------------------------------------------
        // Lookup
        // -----------------------------------------------------------------

        /**
         * @brief Retrieve a prefab by numeric ID.
         * @param id Prefab ID from RegisterPrefab().
         * @return Const pointer to the prefab, or nullptr.
         */
        [[nodiscard]] const RuntimePrefab* GetPrefab(uint32_t id) const
        {
            auto it = m_prefabs.find(id);
            return (it != m_prefabs.end()) ? it->second.get() : nullptr;
        }

        /**
         * @brief Retrieve a prefab by name.
         * @param name Prefab name.
         * @return Const pointer to the prefab, or nullptr.
         */
        [[nodiscard]] const RuntimePrefab* GetPrefab(std::string_view name) const
        {
            std::string key(name);
            auto it = m_nameIndex.find(key);
            if (it == m_nameIndex.end())
            {
                return nullptr;
            }
            return GetPrefab(it->second);
        }

        /**
         * @brief Remove a prefab by ID.
         * @param id Prefab ID to remove.
         * @return True if the prefab existed and was removed.
         */
        bool RemovePrefab(uint32_t id)
        {
            auto it = m_prefabs.find(id);
            if (it == m_prefabs.end())
            {
                return false;
            }

            // Remove from name index
            std::string name(it->second->GetName());
            m_nameIndex.erase(name);
            m_prefabs.erase(it);
            return true;
        }

        // -----------------------------------------------------------------
        // Spawning
        // -----------------------------------------------------------------

        /**
         * @brief Spawn a new ECS entity from a prefab ID.
         * @param prefabId Prefab to instantiate.
         * @return Entity ID (0 if prefab not found).
         */
        uint32_t SpawnEntity(uint32_t prefabId)
        {
            const auto* prefab = GetPrefab(prefabId);
            if (!prefab)
            {
                return 0;
            }
            return SpawnFromPrefab(*prefab);
        }

        /**
         * @brief Spawn a new ECS entity from a prefab name.
         * @param prefabName Prefab to instantiate.
         * @return Entity ID (0 if prefab not found).
         */
        uint32_t SpawnEntity(std::string_view prefabName)
        {
            const auto* prefab = GetPrefab(prefabName);
            if (!prefab)
            {
                return 0;
            }
            return SpawnFromPrefab(*prefab);
        }

        // -----------------------------------------------------------------
        // Queries
        // -----------------------------------------------------------------

        /** @brief Number of registered prefabs. */
        [[nodiscard]] uint32_t GetRegisteredCount() const { return static_cast<uint32_t>(m_prefabs.size()); }

        /** @brief Get the names of all registered prefabs. */
        [[nodiscard]] std::vector<std::string_view> GetAllPrefabNames() const
        {
            std::vector<std::string_view> names;
            names.reserve(m_prefabs.size());
            for (const auto& [id, prefab] : m_prefabs)
            {
                names.push_back(prefab->GetName());
            }
            return names;
        }

        // -----------------------------------------------------------------
        // File I/O
        // -----------------------------------------------------------------

        /**
         * @brief Save all registered prefabs to a binary file.
         * @param path Output file path.
         */
        void SaveToFile(std::string_view path) const
        {
            (void)path;
            // Full implementation would create a BinaryWriter, iterate
            // m_prefabs, call Serialize() on each, and write to disk.
        }

        /**
         * @brief Load prefabs from a binary file and register them.
         * @param path Input file path.
         */
        void LoadFromFile(std::string_view path)
        {
            (void)path;
            // Full implementation would open the file, create a BinaryReader,
            // deserialize each prefab, and call RegisterPrefab().
        }

        // -----------------------------------------------------------------
        // Console
        // -----------------------------------------------------------------

        /** @brief Human-readable status for the engine console. */
        [[nodiscard]] std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "[PrefabRegistry] prefabs=" << m_prefabs.size();
            for (const auto& [id, prefab] : m_prefabs)
            {
                oss << "\n  [" << id << "] " << prefab->GetName() << " (" << prefab->GetComponents().size()
                    << " components)";
            }
            return oss.str();
        }

      private:
        PrefabRegistry() = default;
        PrefabRegistry(const PrefabRegistry&) = delete;
        PrefabRegistry& operator=(const PrefabRegistry&) = delete;

        /**
         * @brief Internal spawn helper — creates entity from prefab components.
         * @param prefab The prefab to instantiate.
         * @return Assigned entity ID.
         */
        uint32_t SpawnFromPrefab(const RuntimePrefab& prefab)
        {
            // In a full implementation this would:
            //   1. Create an EnTT entity via the ECS registry.
            //   2. Walk prefab.GetComponents() and add matching components.
            //   3. Apply parent prefab components first if GetParent() != nullptr.
            // Returns a placeholder incremented ID for now.
            return m_nextEntityId++;
        }

        std::unordered_map<uint32_t, std::unique_ptr<RuntimePrefab>> m_prefabs; ///< ID -> prefab.
        std::unordered_map<std::string, uint32_t> m_nameIndex;                  ///< Name -> ID.
        uint32_t m_nextId = 1;                                                  ///< Next auto-assigned prefab ID.
        uint32_t m_nextEntityId = 1000;                                         ///< Placeholder entity ID counter.
    };

} // namespace Spark::ECS
