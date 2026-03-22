/**
 * @file EntityArchetypeLoader.cpp
 * @brief Data-driven archetype loading and entity spawning
 */

#include "EntityArchetypeLoader.h"
#include "Components/LightComponents.h"
#include "../../Utils/SparkConsole.h"
#include "../../Utils/StringUtils.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::ECS
{

    // =========================================================================
    // Parsing Helpers
    // =========================================================================

    /// Trim leading and trailing whitespace.
    static std::string Trim(const std::string& str)
    {
        auto start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        auto end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    /// Split a string by a delimiter character.
    static std::vector<std::string> Split(const std::string& str, char delimiter)
    {
        std::vector<std::string> tokens;
        std::istringstream stream(str);
        std::string token;
        while (std::getline(stream, token, delimiter))
        {
            std::string trimmed = Trim(token);
            if (!trimmed.empty())
            {
                tokens.push_back(trimmed);
            }
        }
        return tokens;
    }

    /// Parse a "x,y,z" string into three floats. Returns false on parse failure.
    static bool ParseFloat3(const std::string& str, float& x, float& y, float& z)
    {
        auto parts = Split(str, ',');
        if (parts.size() < 3)
            return false;

        auto px = Spark::StringUtils::ParseFloat(parts[0]);
        auto py = Spark::StringUtils::ParseFloat(parts[1]);
        auto pz = Spark::StringUtils::ParseFloat(parts[2]);
        if (!px || !py || !pz)
            return false;

        x = *px;
        y = *py;
        z = *pz;
        return true;
    }

    /**
     * @brief Parse a component line into a ComponentEntry.
     *
     * Format: "TypeName: param1 / param2 / param3"
     * The part before ':' is the type name. The part after is split by '/'
     * into positional parameters stored as properties "p0", "p1", etc.
     */
    static ComponentEntry ParseComponentLine(const std::string& value)
    {
        ComponentEntry entry;

        auto colonPos = value.find(':');
        if (colonPos == std::string::npos)
        {
            entry.typeName = Trim(value);
            return entry;
        }

        entry.typeName = Trim(value.substr(0, colonPos));
        std::string params = Trim(value.substr(colonPos + 1));

        auto parts = Split(params, '/');
        for (size_t i = 0; i < parts.size(); ++i)
        {
            entry.properties["p" + std::to_string(i)] = parts[i];
        }

        return entry;
    }

    // =========================================================================
    // File Loading
    // =========================================================================

    bool LoadArchetypeFromFile(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("ArchetypeLoader: failed to open file: " + path);
            return false;
        }

        Archetype archetype;
        std::string line;

        while (std::getline(file, line))
        {
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed.starts_with("//"))
                continue;

            auto eqPos = trimmed.find('=');
            if (eqPos == std::string::npos)
                continue;

            std::string key = Trim(trimmed.substr(0, eqPos));
            std::string value = Trim(trimmed.substr(eqPos + 1));

            if (key == "name")
            {
                archetype.name = value;
            }
            else if (key == "category")
            {
                archetype.category = value;
            }
            else if (key == "component")
            {
                archetype.components.push_back(ParseComponentLine(value));
            }
        }

        if (archetype.name.empty())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("ArchetypeLoader: file has no 'name' field: " + path);
            return false;
        }

        EntityArchetypeSystem::GetInstance().RegisterArchetype(archetype);
        Spark::SimpleConsole::GetInstance().LogSuccess("ArchetypeLoader: loaded archetype '" + archetype.name +
                                                       "' with " + std::to_string(archetype.components.size()) +
                                                       " components");
        return true;
    }

    // =========================================================================
    // Entity Spawning
    // =========================================================================

    /// Apply a Transform component from archetype properties.
    static void ApplyTransform(World& world, EntityID entity, const ComponentEntry& entry)
    {
        Transform transform;

        // p0 = position, p1 = rotation, p2 = scale
        if (auto it = entry.properties.find("p0"); it != entry.properties.end())
        {
            ParseFloat3(it->second, transform.position.x, transform.position.y, transform.position.z);
        }
        if (auto it = entry.properties.find("p1"); it != entry.properties.end())
        {
            ParseFloat3(it->second, transform.rotation.x, transform.rotation.y, transform.rotation.z);
        }
        if (auto it = entry.properties.find("p2"); it != entry.properties.end())
        {
            ParseFloat3(it->second, transform.scale.x, transform.scale.y, transform.scale.z);
        }

        world.AddComponent<Transform>(entity, transform);
    }

    /// Apply a LightComponent from archetype properties.
    static void ApplyLightComponent(World& world, EntityID entity, const ComponentEntry& entry)
    {
        LightComponent light;

        // p0 = type (Point/Directional/Spot)
        if (auto it = entry.properties.find("p0"); it != entry.properties.end())
        {
            if (it->second == "Directional")
                light.type = LightComponent::Type::Directional;
            else if (it->second == "Spot")
                light.type = LightComponent::Type::Spot;
            else
                light.type = LightComponent::Type::Point;
        }

        // p1 = color (r,g,b)
        if (auto it = entry.properties.find("p1"); it != entry.properties.end())
        {
            ParseFloat3(it->second, light.color.x, light.color.y, light.color.z);
        }

        // p2 = intensity
        if (auto it = entry.properties.find("p2"); it != entry.properties.end())
        {
            light.intensity = Spark::StringUtils::ParseFloat(it->second).value_or(light.intensity);
        }

        // p3 = range
        if (auto it = entry.properties.find("p3"); it != entry.properties.end())
        {
            light.range = Spark::StringUtils::ParseFloat(it->second).value_or(light.range);
        }

        world.AddComponent<LightComponent>(entity, light);
    }

    /// Apply a NameComponent from archetype properties.
    static void ApplyNameComponent(World& world, EntityID entity, const ComponentEntry& entry)
    {
        std::string name;
        if (auto it = entry.properties.find("p0"); it != entry.properties.end())
        {
            name = it->second;
        }
        // Only add if entity doesn't already have a NameComponent
        if (!world.HasComponent<NameComponent>(entity))
        {
            world.AddComponent<NameComponent>(entity, NameComponent{name});
        }
    }

    EntityID SpawnFromArchetype(const std::string& name, World& world)
    {
        const auto* archetype = EntityArchetypeSystem::GetInstance().GetArchetype(name);
        if (!archetype)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("SpawnFromArchetype: archetype not found: " + name);
            return entt::null;
        }

        EntityID entity = world.CreateEntity(archetype->name);

        for (const auto& comp : archetype->components)
        {
            if (comp.typeName == "Transform")
            {
                ApplyTransform(world, entity, comp);
            }
            else if (comp.typeName == "LightComponent")
            {
                ApplyLightComponent(world, entity, comp);
            }
            else if (comp.typeName == "NameComponent")
            {
                ApplyNameComponent(world, entity, comp);
            }
            // Unrecognized component types are stored in the archetype
            // for game-specific code to interpret via GetArchetype()
        }

        return entity;
    }

    EntityID SpawnFromArchetype(const std::string& name, World& world,
                                const std::unordered_map<std::string, std::string>& overrides)
    {
        const auto* archetype = EntityArchetypeSystem::GetInstance().GetArchetype(name);
        if (!archetype)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("SpawnFromArchetype: archetype not found: " + name);
            return entt::null;
        }

        // Build a mutable copy of the archetype's components with overrides applied.
        // Override keys use the format "ComponentType.propertyName".
        auto components = archetype->components;
        for (const auto& [key, value] : overrides)
        {
            auto dotPos = key.find('.');
            if (dotPos == std::string::npos)
                continue;

            std::string compType = key.substr(0, dotPos);
            std::string propName = key.substr(dotPos + 1);

            for (auto& comp : components)
            {
                if (comp.typeName == compType)
                {
                    comp.properties[propName] = value;
                }
            }
        }

        EntityID entity = world.CreateEntity(archetype->name);

        for (const auto& comp : components)
        {
            if (comp.typeName == "Transform")
            {
                ApplyTransform(world, entity, comp);
            }
            else if (comp.typeName == "LightComponent")
            {
                ApplyLightComponent(world, entity, comp);
            }
            else if (comp.typeName == "NameComponent")
            {
                ApplyNameComponent(world, entity, comp);
            }
        }

        return entity;
    }

} // namespace Spark::ECS
