/**
 * @file FuzzEntityArchetypeProduction.cpp
 * @brief libc++-compiled production adapter for the .archetype libFuzzer harness.
 *
 * LoadArchetypeFromFile opens every .archetype file and hands the stream to
 * Spark::ECS::ParseArchetypeDefinition; SpawnFromArchetype later looks components up by
 * type name and positional parameters by "p<N>". The adapter wraps the fuzz bytes in the
 * same kind of stream and calls the shipped reader. A violated contract aborts so libFuzzer
 * records a crash:
 *  - the result is true exactly when the archetype is named,
 *  - no accepted name, category, type name or parameter holds a line break or keeps
 *    leading or trailing whitespace; no type name holds ':' and no parameter holds '/'
 *    or is empty (each would re-split differently),
 *  - each component's properties are exactly "p0".."p<N-1>",
 *  - there are no more components than input lines,
 *  - an accepted archetype written back as key = value lines parses to the identical
 *    archetype, and parsing the same bytes twice gives the same archetype.
 */

#include "FuzzEntityArchetypeProduction.h"

#include "Engine/ECS/EntityArchetype.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEntityArchetype: ParseArchetypeDefinition violated: %s\n", what);
        std::abort();
    }

    using Spark::ECS::Archetype;
    using Spark::ECS::ComponentEntry;

    bool Parse(const std::string& text, Archetype& archetype)
    {
        std::istringstream input(text);
        return Spark::ECS::ParseArchetypeDefinition(input, archetype);
    }

    bool SameArchetype(const Archetype& a, const Archetype& b)
    {
        if (a.name != b.name || a.category != b.category || a.components.size() != b.components.size())
            return false;
        for (std::size_t i = 0; i < a.components.size(); ++i)
        {
            if (a.components[i].typeName != b.components[i].typeName ||
                a.components[i].properties != b.components[i].properties)
                return false;
        }
        return true;
    }

    void CheckText(const std::string& value)
    {
        if (value.find('\n') != std::string::npos)
            InvariantFailure("a field holds a line feed");
        constexpr std::string_view kWhitespace = " \t\r\n";
        if (!value.empty() && (kWhitespace.find(value.front()) != std::string_view::npos ||
                               kWhitespace.find(value.back()) != std::string_view::npos))
            InvariantFailure("a field keeps leading or trailing whitespace");
    }

    void CheckComponent(const ComponentEntry& component)
    {
        CheckText(component.typeName);
        if (component.typeName.find(':') != std::string::npos)
            InvariantFailure("a component type name holds ':'");
        for (std::size_t i = 0; i < component.properties.size(); ++i)
        {
            const auto it = component.properties.find("p" + std::to_string(i));
            if (it == component.properties.end())
                InvariantFailure("component properties are not exactly p0..p<N-1>");
            const std::string& value = it->second;
            if (value.empty())
                InvariantFailure("an empty positional parameter survived");
            if (value.find('/') != std::string::npos)
                InvariantFailure("a positional parameter holds '/'");
            CheckText(value);
        }
    }

    std::string Write(const Archetype& archetype)
    {
        std::string text = "name = " + archetype.name + "\ncategory = " + archetype.category + "\n";
        for (const ComponentEntry& component : archetype.components)
        {
            text += "component = " + component.typeName;
            for (std::size_t i = 0; i < component.properties.size(); ++i)
            {
                text += i == 0 ? ": " : " / ";
                text += component.properties.at("p" + std::to_string(i));
            }
            text += "\n";
        }
        return text;
    }
} // namespace

extern "C" int SparkFuzzParseArchetype(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const std::string text = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    Archetype archetype;
    archetype.name = "sentinel";
    archetype.components.push_back(ComponentEntry{"Sentinel", {{"p0", "x"}}});
    const bool accepted = Parse(text, archetype);
    if (accepted != !archetype.name.empty())
        InvariantFailure("the result disagrees with whether the archetype is named");

    Archetype again;
    if (Parse(text, again) != accepted || !SameArchetype(archetype, again))
        InvariantFailure("parsing the same bytes twice gave different archetypes");
    if (!accepted)
        return 0;

    const std::size_t lineCount = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    if (archetype.components.size() > lineCount)
        InvariantFailure("more components than input lines");
    CheckText(archetype.name);
    CheckText(archetype.category);
    for (const ComponentEntry& component : archetype.components)
        CheckComponent(component);

    Archetype reloaded;
    if (!Parse(Write(archetype), reloaded))
        InvariantFailure("a written archetype is no longer named");
    if (!SameArchetype(archetype, reloaded))
        InvariantFailure("write -> parse changed the archetype");
    return 0;
}
