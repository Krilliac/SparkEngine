/**
 * @file PrefabAsset.h
 * @brief Prefab asset data structure for reusable entity templates
 * @author Spark Engine Team
 * @date 2025
 *
 * Defines the PrefabAsset class that stores a template entity's component
 * data for instantiation and reuse across scenes.
 */

#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <variant>

#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;

namespace SparkEditor
{

    /**
     * @brief Property value type for serialized component properties
     */
    using PrefabPropertyValue = std::variant<bool, int, float, double, std::string, XMFLOAT3, XMFLOAT4>;

    /**
     * @brief Serialized component data within a prefab
     */
    struct SerializedComponent
    {
        std::string typeName;                                            ///< Component type name (e.g. "Transform")
        std::unordered_map<std::string, PrefabPropertyValue> properties; ///< Property name to value mapping
    };

    /**
     * @brief Override tracking for a prefab instance property
     */
    struct PrefabOverride
    {
        std::string componentType; ///< Component that has the override
        std::string propertyName;  ///< Property name that is overridden
        PrefabPropertyValue value; ///< Overridden value
    };

    /**
     * @brief Prefab asset storing a reusable entity template
     *
     * A prefab captures an entity's component configuration as a template
     * that can be instantiated multiple times. Instances track overrides
     * to identify which properties differ from the template.
     */
    class PrefabAsset
    {
      public:
        PrefabAsset() = default;

        /**
         * @brief Construct a named prefab
         * @param name Display name of the prefab
         */
        explicit PrefabAsset(const std::string& name);

        /**
         * @brief Get the prefab name
         * @return Prefab display name
         */
        const std::string& GetName() const { return m_name; }

        /**
         * @brief Set the prefab name
         * @param name New display name
         */
        void SetName(const std::string& name) { m_name = name; }

        /**
         * @brief Get the file path of this prefab
         * @return File path (.sparkprefab)
         */
        const std::string& GetFilePath() const { return m_filePath; }

        /**
         * @brief Set the file path
         * @param path File path
         */
        void SetFilePath(const std::string& path) { m_filePath = path; }

        /**
         * @brief Get all serialized components
         * @return Const reference to the component list
         */
        const std::vector<SerializedComponent>& GetComponents() const { return m_components; }

        /**
         * @brief Get mutable reference to components for editing
         * @return Reference to the component list
         */
        std::vector<SerializedComponent>& GetComponents() { return m_components; }

        /**
         * @brief Add a component to the prefab template
         * @param component Serialized component data
         */
        void AddComponent(const SerializedComponent& component);

        /**
         * @brief Remove a component by type name
         * @param typeName Component type to remove
         * @return true if component was found and removed
         */
        bool RemoveComponent(const std::string& typeName);

        /**
         * @brief Check if prefab has a component of the given type
         * @param typeName Component type name
         * @return true if the component exists
         */
        bool HasComponent(const std::string& typeName) const;

        /**
         * @brief Get a component by type name
         * @param typeName Component type name
         * @return Pointer to the component, or nullptr if not found
         */
        const SerializedComponent* GetComponent(const std::string& typeName) const;

        /**
         * @brief Save the prefab to a file
         * @param path File path to save to (.sparkprefab)
         * @return true if save succeeded
         */
        bool Save(const std::string& path);

        /**
         * @brief Load a prefab from a file
         * @param path File path to load from
         * @return Loaded prefab asset, or empty prefab on failure
         */
        static PrefabAsset Load(const std::string& path);

        /**
         * @brief Check if this prefab has been modified since last save
         * @return true if modified
         */
        bool IsModified() const { return m_isModified; }

        /**
         * @brief Mark the prefab as modified or saved
         * @param modified Modified state
         */
        void SetModified(bool modified) { m_isModified = modified; }

        /**
         * @brief Get the unique ID of this prefab
         * @return Prefab ID
         */
        uint64_t GetId() const { return m_id; }

      private:
        std::string m_name;
        std::string m_filePath;
        std::vector<SerializedComponent> m_components;
        uint64_t m_id = 0;
        bool m_isModified = false;

        static uint64_t s_nextId;
    };

} // namespace SparkEditor
