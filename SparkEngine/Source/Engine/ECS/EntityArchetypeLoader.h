/**
 * @file EntityArchetypeLoader.h
 * @brief Data-driven archetype loading from .archetype files and spawn API
 * @author Spark Engine Team
 * @date 2026
 *
 * Loads entity archetypes from a simple text format and provides a spawn API
 * that creates fully configured entities from registered archetypes.
 *
 * ## .archetype file format
 * @code
 * // Comment lines start with //
 * name = GuardNPC
 * category = Characters
 * component = Transform: 0,0,0 / 0,0,0 / 1,1,1
 * component = LightComponent: Point / 1,1,1 / 1.0 / 10.0
 * @endcode
 *
 * @see EntityArchetype.h, Components.h
 */

#pragma once

#include "EntityArchetype.h"
#include "Components.h"

#include <string>
#include <unordered_map>

namespace Spark::ECS
{

    /**
     * @brief Load an archetype definition from a .archetype text file.
     *
     * Parses the file and registers the archetype with EntityArchetypeSystem.
     *
     * @param path Path to the .archetype file.
     * @return true if the file was parsed and the archetype registered.
     */
    bool LoadArchetypeFromFile(const std::string& path);

    /**
     * @brief Spawn a new entity from a registered archetype.
     *
     * Looks up the archetype by name, creates an entity in the given World,
     * and adds all components defined in the archetype with their default
     * property values.
     *
     * Supported component types:
     * - Transform: posX,posY,posZ / rotX,rotY,rotZ / scaleX,scaleY,scaleZ
     * - LightComponent: Type / r,g,b / intensity / range
     * - NameComponent: name
     *
     * Unrecognized component types are silently skipped (the archetype
     * still stores them for game-specific spawning code to interpret).
     *
     * @param name  Name of the registered archetype.
     * @param world World to spawn the entity into.
     * @return The created EntityID, or entt::null if the archetype is not found.
     */
    EntityID SpawnFromArchetype(const std::string& name, World& world);

    /**
     * @brief Spawn a new entity from a registered archetype with property overrides.
     *
     * Works like SpawnFromArchetype(name, world), but after applying the archetype's
     * default component properties, applies the given overrides on top. Override keys
     * use the format "ComponentType.propertyName" (e.g. "Transform.p0" to override
     * the position parameter).
     *
     * @param name      Name of the registered archetype.
     * @param world     World to spawn the entity into.
     * @param overrides Map of "ComponentType.propertyName" -> value strings.
     * @return The created EntityID, or entt::null if the archetype is not found.
     */
    EntityID SpawnFromArchetype(const std::string& name, World& world,
                                const std::unordered_map<std::string, std::string>& overrides);

} // namespace Spark::ECS
