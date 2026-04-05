/**
 * @file SceneManifest.h
 * @brief Lightweight scene asset manifest for area streaming
 * @author Spark Engine Team
 * @date 2026
 *
 * Lists the assets (meshes, textures, audio) that make up a scene/area.
 * Can be built programmatically or parsed from a .sparkscene text file
 * using the same key=value format as .sparkmat files.
 *
 * ## .sparkscene file format
 * @code
 * // Comment lines start with //
 * name = TownSquare
 * mesh = models/town_square.mesh
 * mesh = models/fountain.mesh
 * texture = textures/cobblestone.dds
 * texture = textures/fountain_diffuse.dds
 * audio = audio/town_ambience.wav
 * @endcode
 *
 * @see SeamlessAreaManager.h, AreaAssetLoader.h
 */

#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Spark::Streaming
{

    /**
     * @brief Asset manifest describing all resources needed for a scene/area
     */
    struct SceneManifest
    {
        std::string name;                      ///< Scene/area name
        std::vector<std::string> meshPaths;    ///< Mesh asset paths
        std::vector<std::string> texturePaths; ///< Texture asset paths
        std::vector<std::string> audioPaths;   ///< Audio asset paths

        /**
         * @brief Total number of assets in this manifest
         */
        size_t TotalAssetCount() const { return meshPaths.size() + texturePaths.size() + audioPaths.size(); }

        /**
         * @brief Collect all asset paths into a single flat list
         */
        std::vector<std::string> AllPaths() const
        {
            std::vector<std::string> all;
            all.reserve(TotalAssetCount());
            all.insert(all.end(), meshPaths.begin(), meshPaths.end());
            all.insert(all.end(), texturePaths.begin(), texturePaths.end());
            all.insert(all.end(), audioPaths.begin(), audioPaths.end());
            return all;
        }

        /**
         * @brief Parse a manifest from a string (key=value lines)
         * @param content The .sparkscene file content
         * @return Parsed SceneManifest
         */
        static SceneManifest ParseFromString(const std::string& content)
        {
            SceneManifest manifest;
            std::istringstream stream(content);
            std::string line;

            while (std::getline(stream, line))
            {
                // Strip leading/trailing whitespace
                auto start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos)
                    continue;
                line = line.substr(start);

                // Skip comments and empty lines
                if (line.empty() || line.starts_with("//"))
                    continue;

                // Parse key = value
                auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);

                // Trim whitespace from key and value
                auto trimEnd = key.find_last_not_of(" \t");
                if (trimEnd != std::string::npos)
                    key = key.substr(0, trimEnd + 1);

                auto valStart = value.find_first_not_of(" \t");
                if (valStart != std::string::npos)
                    value = value.substr(valStart);
                auto valEnd = value.find_last_not_of(" \t\r\n");
                if (valEnd != std::string::npos)
                    value = value.substr(0, valEnd + 1);

                if (key == "name")
                    manifest.name = value;
                else if (key == "mesh")
                    manifest.meshPaths.push_back(value);
                else if (key == "texture")
                    manifest.texturePaths.push_back(value);
                else if (key == "audio")
                    manifest.audioPaths.push_back(value);
            }

            return manifest;
        }

        /**
         * @brief Parse a manifest from a .sparkscene file
         * @param filePath Path to the .sparkscene file
         * @return Parsed SceneManifest (empty if file not found)
         */
        static SceneManifest ParseFromFile(const std::string& filePath)
        {
            std::ifstream file(filePath);
            if (!file.is_open())
                return {};

            std::ostringstream ss;
            ss << file.rdbuf();
            return ParseFromString(ss.str());
        }
    };

} // namespace Spark::Streaming
