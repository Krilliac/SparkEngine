/**
 * @file EntityArchetypeParse.cpp
 * @brief .archetype text reader, apart from the loader's registration and spawning
 *
 * LoadArchetypeFromFile opens the file and hands the stream here. Keeping the reader in its
 * own translation unit lets the SEC-120 fuzz target (FuzzerTests/FuzzEntityArchetype.cpp)
 * link the shipped parser without the World, the console or the archetype registry.
 */

#include "EntityArchetype.h"

#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace Spark::ECS
{
    namespace
    {
        /// Trim leading and trailing whitespace.
        std::string Trim(const std::string& str)
        {
            auto start = str.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
            {
                return "";
            }
            auto end = str.find_last_not_of(" \t\r\n");
            return str.substr(start, end - start + 1);
        }

        /// Split a string by a delimiter character, dropping empty (all-whitespace) tokens.
        std::vector<std::string> Split(const std::string& str, char delimiter)
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

        /**
         * @brief Parse a component line into a ComponentEntry.
         *
         * Format: "TypeName: param1 / param2 / param3"
         * The part before ':' is the type name. The part after is split by '/'
         * into positional parameters stored as properties "p0", "p1", etc.
         */
        ComponentEntry ParseComponentLine(const std::string& value)
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
    } // namespace

    bool ParseArchetypeDefinition(std::istream& input, Archetype& archetype)
    {
        archetype = {};
        std::string line;

        while (std::getline(input, line))
        {
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed.starts_with("//"))
            {
                continue;
            }

            auto eqPos = trimmed.find('=');
            if (eqPos == std::string::npos)
            {
                continue;
            }

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

        return !archetype.name.empty();
    }

} // namespace Spark::ECS
