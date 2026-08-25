/**
 * @file RegionMapDataSource.cpp
 * @brief Safe Terrafront continent-to-region-map discovery for SparkEditor.
 */

#include "RegionMapDataSource.h"

#include "Utils/JsonUtils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace SparkEditor
{
    bool IsSafeRegionMapFileName(std::string_view value)
    {
        if (value.empty() || value == "." || value == ".." || value.size() > 128)
            return false;
        if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos ||
            value.find(':') != std::string_view::npos)
            return false;
        const fs::path path{std::string(value)};
        if (path.is_absolute() || path.has_root_path() || path.filename() != path || path.extension() != ".json")
            return false;
        return std::all_of(value.begin(), value.end(), [](char c)
        {
            const auto byte = static_cast<unsigned char>(c);
            return std::isalnum(byte) != 0 || c == '_' || c == '-' || c == '.';
        });
    }

    bool LoadRegionMapDataSources(const fs::path& dataDirectory, std::vector<RegionMapDataSource>& outSources,
                                  std::string& outError)
    {
        outSources.clear();
        outError.clear();

        std::error_code ec;
        const fs::path dataRoot = fs::absolute(dataDirectory, ec).lexically_normal();
        if (ec || !fs::is_directory(dataRoot, ec))
        {
            outError = "Terrafront data directory is unavailable: " + dataDirectory.generic_string();
            return false;
        }

        const fs::path registryPath = dataRoot / "continents.json";
        std::ifstream registry(registryPath, std::ios::binary);
        if (!registry.is_open())
        {
            outError = "cannot open continent registry '" + registryPath.generic_string() + "'";
            return false;
        }
        std::ostringstream bytes;
        bytes << registry.rdbuf();

        Spark::Json::Value root;
        std::string parseError;
        if (!Spark::Json::ParseStrict(bytes.str(), &root, &parseError) || !root.IsObject() ||
            !root.HasKey("continents") || !root["continents"].IsArray())
        {
            outError = "invalid continent registry: " + (parseError.empty() ? "missing continents array" : parseError);
            return false;
        }

        std::unordered_set<int> mapIds;
        std::unordered_set<std::string> keys;
        std::unordered_set<std::string> files;
        const Spark::Json::Value& entries = root["continents"];
        for (size_t index = 0; index < entries.Size(); ++index)
        {
            const Spark::Json::Value& entry = entries[index];
            if (!entry.IsObject() || !entry.HasKey("regions"))
                continue;
            if (!entry["regions"].IsString())
            {
                outError = "continent entry " + std::to_string(index) + " has a non-string regions field";
                return false;
            }

            RegionMapDataSource source;
            source.mapId = entry.HasKey("mapId") ? entry["mapId"].AsInt(-1) : -1;
            source.key = entry.HasKey("key") && entry["key"].IsString() ? entry["key"].AsString() : std::string{};
            source.name = entry.HasKey("name") && entry["name"].IsString() ? entry["name"].AsString() : source.key;
            source.regionsFile = entry["regions"].AsString();

            if (source.mapId < 0 || source.key.empty() || source.name.empty())
            {
                outError = "continent entry " + std::to_string(index) + " is missing mapId, key, or name";
                return false;
            }
            if (!IsSafeRegionMapFileName(source.regionsFile))
            {
                outError = "continent '" + source.key + "' has an unsafe regions filename: " + source.regionsFile;
                return false;
            }
            if (!mapIds.insert(source.mapId).second || !keys.insert(source.key).second ||
                !files.insert(source.regionsFile).second)
            {
                outError = "duplicate mapId, key, or regions filename in continent registry";
                return false;
            }

            source.dataPath = (dataRoot / source.regionsFile).lexically_normal();
            if (source.dataPath.parent_path() != dataRoot || !fs::is_regular_file(source.dataPath, ec))
            {
                outError = "continent '" + source.key + "' references a missing region map: " +
                           source.dataPath.generic_string();
                return false;
            }
            outSources.push_back(std::move(source));
        }

        if (outSources.empty())
        {
            outError = "continent registry contains no editable region maps";
            return false;
        }
        std::sort(outSources.begin(), outSources.end(), [](const RegionMapDataSource& a, const RegionMapDataSource& b)
        {
            return a.mapId < b.mapId;
        });
        return true;
    }
} // namespace SparkEditor
