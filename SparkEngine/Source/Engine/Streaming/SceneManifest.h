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

#include "../../Utils/LogMacros.h"
#include "../Modding/VirtualFileSystem.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace Spark::Streaming
{

    /// A .sparkscene is content a shared project or a mod supplies, so it is
    /// untrusted input: bound the file and the entry count before the streaming
    /// loaders are handed anything.
    inline constexpr size_t MAX_SCENE_MANIFEST_BYTES = 8u * 1024u * 1024u;
    inline constexpr size_t MAX_SCENE_MANIFEST_ENTRIES = 100000u;

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

            // ParseFromString is a public entry point, so the byte bound belongs here
            // as well as in ParseFromFile — a caller that already has the text in
            // memory (a VFS read, an archive entry, a test) reaches this overload
            // directly and would otherwise be unbounded.
            if (content.size() > MAX_SCENE_MANIFEST_BYTES)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Scene, "SceneManifest: content is %zu bytes, above the %zu limit",
                               content.size(), MAX_SCENE_MANIFEST_BYTES);
                return manifest;
            }

            std::istringstream stream(content);
            std::string line;

            // A parse-time filter, not the containment gate: a SceneManifest can also
            // be built in code and handed straight to AreaAssetLoader::SetManifest,
            // so containment for every provenance is enforced where the paths are
            // consumed (AreaAssetLoader::BeginAreaLoad). Dropping here as well keeps
            // '../../../../Users/<name>/.ssh/id_rsa' and absolute paths out of the
            // parsed manifest, with a log line rather than silently.
            bool entriesTruncated = false;
            const auto addPath =
                [&manifest, &entriesTruncated](std::vector<std::string>& target, const std::string& value)
            {
                if (manifest.TotalAssetCount() >= MAX_SCENE_MANIFEST_ENTRIES)
                {
                    // Dropping silently would load a truncated asset set that looks
                    // like a complete one. Log once, not once per surplus entry.
                    if (!entriesTruncated)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Scene,
                                       "SceneManifest: entry cap of %zu reached; later entries are dropped",
                                       MAX_SCENE_MANIFEST_ENTRIES);
                        entriesTruncated = true;
                    }
                    return;
                }
                if (!IsVirtualPathSafe(value))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Scene, "SceneManifest: dropped out-of-root asset path '%s'",
                                   value.c_str());
                    return;
                }
                target.push_back(value);
            };

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
                    addPath(manifest.meshPaths, value);
                else if (key == "texture")
                    addPath(manifest.texturePaths, value);
                else if (key == "audio")
                    addPath(manifest.audioPaths, value);
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
            std::error_code ec;
            const auto fileSize = std::filesystem::file_size(filePath, ec);
            if (ec)
            {
                // Fail closed. A path the process can open but not stat (a pipe, a
                // device, a race with a writer) is exactly the case where an
                // unbounded slurp is most dangerous, so an unknown size is a refusal
                // rather than a skipped guard.
                SPARK_LOG_WARN(Spark::LogCategory::Scene, "SceneManifest: cannot size '%s' (%s); refusing to parse",
                               filePath.c_str(), ec.message().c_str());
                return {};
            }
            if (fileSize > MAX_SCENE_MANIFEST_BYTES)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Scene, "SceneManifest: '%s' is %llu bytes, above the %zu limit",
                               filePath.c_str(), static_cast<unsigned long long>(fileSize), MAX_SCENE_MANIFEST_BYTES);
                return {};
            }

            std::ifstream file(filePath);
            if (!file.is_open())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Scene, "SceneManifest: failed to open '%s' (errno=%d: %s)",
                               filePath.c_str(), errno, std::strerror(errno));
                return {};
            }

            std::ostringstream ss;
            ss << file.rdbuf();
            SceneManifest manifest = ParseFromString(ss.str());
            if (manifest.TotalAssetCount() == 0)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Scene,
                               "SceneManifest: '%s' parsed 0 assets — check file format (expected key=value lines)",
                               filePath.c_str());
            }
            return manifest;
        }
    };

} // namespace Spark::Streaming
