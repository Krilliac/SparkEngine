/**
 * @file SceneImportParser.cpp
 * @brief Game INI .scene reader (moved out of SceneImportPanel.cpp)
 */

#include "SceneImportParser.h"

#include <cmath>
#include <cstdlib>

namespace SparkEditor
{

    namespace
    {
        /// @brief Parse "a,b,c" into out[3] (strtof, same as TFWorldCollision).
        ///        As in the game, a malformed component reads as 0 and ends the triple. A
        ///        non-finite one ends it without being stored: strtof accepts "nan" and "inf"
        ///        and overflows to infinity, and each of those used to land in the imported
        ///        Transform.
        void ParseFloat3(const std::string& value, float out[3])
        {
            const char* c = value.c_str();
            char* end = nullptr;
            for (int i = 0; i < 3; ++i)
            {
                const float component = std::strtof(c, &end);
                if (!std::isfinite(component))
                {
                    return; // keep the prior values from here on
                }
                out[i] = component;
                if (end == c)
                {
                    return; // malformed tail: keep defaults for the rest
                }
                c = end;
                while (*c == ',' || *c == ' ')
                {
                    ++c;
                }
            }
        }
    } // namespace

    GameSceneIniDocument ParseGameSceneIni(std::string_view text)
    {
        GameSceneIniDocument out;
        std::string currentSection;
        SceneEditTools::SceneObjectRecord current;
        bool inNode = false;

        auto flushNode = [&]()
        {
            if (inNode)
            {
                if (current.type == "cube" || current.type == "Cube" || current.type == "model" ||
                    current.type == "Model")
                {
                    if (current.name.empty())
                    {
                        current.name = current.type + "_" + std::to_string(out.objects.size());
                    }
                    out.objects.push_back(current);
                }
                else
                {
                    // Honest skip accounting: spawnpoints, terrain, unknown types.
                    out.skippedTypes.push_back(current.type.empty() ? "<no type> [" + currentSection + "]"
                                                                    : current.type);
                }
            }
            current = SceneEditTools::SceneObjectRecord{};
            inNode = false;
        };

        size_t lineStart = 0;
        while (lineStart < text.size())
        {
            size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string_view::npos)
            {
                lineEnd = text.size();
            }
            std::string line(text.substr(lineStart, lineEnd - lineStart));
            lineStart = lineEnd + 1;

            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#' || line[0] == ';')
            {
                continue;
            }

            if (line.front() == '[' && line.back() == ']')
            {
                flushNode();
                currentSection = line.substr(1, line.size() - 2);
                // Every non-[Scene] section is a node candidate so unknown
                // sections are reported as skips instead of vanishing.
                inNode = (currentSection != "Scene");
                continue;
            }

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);

            if (currentSection == "Scene")
            {
                if (key == "name")
                {
                    out.sceneName = value;
                }
                continue;
            }
            if (!inNode)
            {
                continue;
            }

            if (key == "type")
            {
                current.type = value;
            }
            else if (key == "name")
            {
                current.name = value;
            }
            else if (key == "model")
            {
                current.model = value;
            }
            else if (key == "material")
            {
                current.material = value;
            }
            else if (key == "position")
            {
                ParseFloat3(value, current.position);
            }
            else if (key == "rotation")
            {
                ParseFloat3(value, current.rotationDeg);
            }
            else if (key == "scale")
            {
                ParseFloat3(value, current.scale);
            }
            // other keys (tag, priority, tf* terrain params) are intentionally ignored
        }
        flushNode();
        return out;
    }

} // namespace SparkEditor
