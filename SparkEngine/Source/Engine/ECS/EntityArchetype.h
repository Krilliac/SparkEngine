/**
 * @file EntityArchetype.h
 * @brief Entity archetype/template system
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Provides a data-driven entity template system where archetypes define
 * which components an entity should have and their default property values.
 * Archetypes are registered at startup (from code or loaded from data files)
 * and queried at spawn time to configure new entities consistently.
 *
 * ## Architecture
 * ```
 * EntityArchetypeSystem (singleton)
 *   ├── m_archetypes      (registered archetypes, keyed by name)
 *   └── Query methods     (by name, by category, list all)
 * ```
 *
 * ## CryEngine Comparison
 * CryEngine's Entity Archetype system uses XML-defined entity templates
 * in the Sandbox editor. This system provides the runtime equivalent:
 * archetypes are C++ structs with component entries and string properties
 * that the spawning code interprets to add and configure components.
 *
 * ## Usage
 * @code
 *   auto& eas = Spark::ECS::EntityArchetypeSystem::GetInstance();
 *   eas.Initialize();
 *
 *   Archetype guardArchetype;
 *   guardArchetype.name = "GuardNPC";
 *   guardArchetype.category = "Characters";
 *   guardArchetype.components = {
 *       {"Transform", {{"posX", "0"}, {"posY", "0"}, {"posZ", "0"}}},
 *       {"MeshRenderer", {{"mesh", "guard_model.fbx"}}},
 *       {"AIComponent", {{"behaviorTree", "PatrolGuard"}, {"detectionRange", "25"}}}
 *   };
 *   eas.RegisterArchetype(guardArchetype);
 *
 *   // At spawn time:
 *   if (const auto* arch = eas.GetArchetype("GuardNPC"))
 *   {
 *       for (const auto& comp : arch->components)
 *       {
 *           // Add component by typeName, configure from properties
 *       }
 *   }
 * @endcode
 *
 * @see Components.h (ECS component types referenced by archetypes)
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::ECS
{

    // =========================================================================
    // Archetype Data Structures
    // =========================================================================

    /**
     * @brief A component entry within an archetype.
     *
     * Describes a component by type name and provides default property
     * values as string key-value pairs. The spawning code is responsible
     * for interpreting these values and applying them to actual components.
     */
    struct ComponentEntry
    {
        std::string typeName; ///< Component type name (e.g., "Transform", "MeshRenderer")

        /** @brief Default property values. Key = property name, Value = string representation. */
        std::unordered_map<std::string, std::string> properties;
    };

    /**
     * @brief An entity archetype — a template for spawning entities.
     *
     * Defines a named, categorized collection of components with default
     * property values. Multiple entities can be spawned from the same
     * archetype with consistent configuration.
     */
    struct Archetype
    {
        std::string name;     ///< Unique archetype name (e.g., "GuardNPC")
        std::string category; ///< Category for organization (e.g., "Characters", "Props")

        /** @brief Components that entities of this archetype should have. */
        std::vector<ComponentEntry> components;
    };

    // =========================================================================
    // EntityArchetypeSystem
    // =========================================================================

    /**
     * @class EntityArchetypeSystem
     * @brief Singleton registry of entity archetypes for data-driven spawning.
     *
     * Stores archetype definitions and provides lookup by name or category.
     * Does not perform actual entity spawning — the caller retrieves an
     * archetype and iterates its components to add them to a new entity.
     */
    class EntityArchetypeSystem
    {
      public:
        /** @brief Get the singleton instance. */
        static EntityArchetypeSystem& GetInstance()
        {
            static EntityArchetypeSystem s;
            return s;
        }

        /** @brief Initialize the system. */
        void Initialize();

        /** @brief Shut down and remove all archetypes. */
        void Shutdown();

        /**
         * @brief Register an archetype.
         * @param archetype The archetype to register. Replaces any existing with the same name.
         */
        void RegisterArchetype(const Archetype& archetype);

        /**
         * @brief Get an archetype by name.
         * @param name Archetype name.
         * @return Const pointer to the archetype, or nullptr if not found.
         */
        const Archetype* GetArchetype(const std::string& name) const;

        /**
         * @brief Get all archetypes in a category.
         * @param category Category name to filter by.
         * @return Vector of const pointers to matching archetypes.
         */
        std::vector<const Archetype*> GetArchetypesByCategory(const std::string& category) const;

        /**
         * @brief Get all registered archetype names.
         * @return Vector of archetype names.
         */
        std::vector<std::string> GetAllArchetypeNames() const;

        /**
         * @brief Remove an archetype by name.
         * @param name Archetype to remove.
         */
        void RemoveArchetype(const std::string& name);

        /**
         * @brief Check if an archetype exists.
         * @param name Archetype name.
         * @return true if registered.
         */
        bool HasArchetype(const std::string& name) const;

        /**
         * @brief Get the number of registered archetypes.
         * @return Archetype count.
         */
        size_t GetArchetypeCount() const { return m_archetypes.size(); }

      private:
        EntityArchetypeSystem() = default;

        std::unordered_map<std::string, Archetype> m_archetypes;
        bool m_initialized = false;
    };

} // namespace Spark::ECS
