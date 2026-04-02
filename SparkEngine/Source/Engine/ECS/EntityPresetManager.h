/**
 * @file EntityPresetManager.h
 * @brief Pre-configured entity templates for rapid no-code game creation
 *
 * Provides a registry of entity presets (e.g., "Enemy", "Door", "HealthPickup")
 * that bundle common component configurations and event response rules.
 * Non-coders can create game objects by selecting from a preset menu instead
 * of manually adding and configuring components.
 *
 * @see EventResponseSystem.h for the rules attached to presets
 * @see Components.h for the component types
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::ECS
{

    /**
     * @brief A single component configuration within a preset
     */
    struct ComponentPreset
    {
        std::string componentType; ///< Component name (e.g., "Transform", "HealthComponent")
        std::unordered_map<std::string, std::string> properties; ///< Key-value property overrides
    };

    /**
     * @brief A pre-configured entity template with components and optional event rules
     */
    struct EntityPreset
    {
        std::string name;                        ///< Display name (e.g., "Enemy", "Door")
        std::string category;                    ///< Category for grouping (e.g., "Character", "Prop", "Trigger")
        std::string description;                 ///< Brief description for tooltips
        std::vector<ComponentPreset> components; ///< Components to add
    };

    /**
     * @brief Registry and factory for entity presets
     *
     * Manages a library of pre-configured entity templates. Built-in presets
     * are registered at initialization; custom presets can be added or loaded
     * from JSON files.
     *
     * @code
     *   auto& mgr = EntityPresetManager::GetInstance();
     *   mgr.Initialize();
     *   // List available presets
     *   for (const auto& preset : mgr.GetPresets()) { ... }
     *   // Get a specific preset by name
     *   if (auto* preset = mgr.FindPreset("Enemy")) { ... }
     * @endcode
     */
    class EntityPresetManager
    {
      public:
        static EntityPresetManager& GetInstance();

        /**
         * @brief Initialize with built-in presets
         */
        void Initialize();

        /**
         * @brief Register a custom preset
         */
        void RegisterPreset(EntityPreset preset);

        /**
         * @brief Get all registered presets
         */
        const std::vector<EntityPreset>& GetPresets() const { return m_presets; }

        /**
         * @brief Find a preset by name
         * @return Pointer to the preset, or nullptr if not found
         */
        const EntityPreset* FindPreset(const std::string& name) const;

        /**
         * @brief Get preset names grouped by category
         * @return Map of category name → list of preset names
         */
        std::unordered_map<std::string, std::vector<std::string>> GetPresetsByCategory() const;

        /**
         * @brief Load custom presets from a JSON directory
         * @param directory Path to directory containing .preset.json files
         * @return Number of presets loaded
         */
        int LoadPresetsFromDirectory(const std::string& directory);

      private:
        EntityPresetManager() = default;

        void RegisterBuiltinPresets();

        std::vector<EntityPreset> m_presets;
    };

} // namespace Spark::ECS
