/**
 * @file ModSystemIO.cpp
 * @brief Mod system persistence and console output — config save/load, mod.json parsing, console status
 */

#include "ModSystem.h"
#include "../../Utils/JsonUtils.h"
#include "../../Utils/LogMacros.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace Spark
{

    namespace
    {
        /// A mod manifest / mod config is a hand-written document, never a data
        /// dump. 64 KB matches DynamicPluginHost's kMaximumMetadataBytes; depth 16
        /// is far above the two levels these documents actually use.
        constexpr size_t MAX_MOD_JSON_BYTES = 64u * 1024u;
        constexpr Json::JsonLimits MOD_JSON_LIMITS{.maxBytes = MAX_MOD_JSON_BYTES, .maxDepth = 16u, .maxNodes = 4096u};

        /// Read a manifest whole, refusing an oversized file from its directory
        /// entry BEFORE any of its bytes are pulled into memory. A mod directory is
        /// untrusted input: without this a 4 GB mod.json is read into a std::string
        /// and then into a Value tree several times larger before any field is
        /// inspected.
        bool ReadModManifestFile(const std::string& path, std::string& outContent)
        {
            std::error_code ec;
            const auto fileSize = std::filesystem::file_size(path, ec);
            if (ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: cannot stat '%s' (%s)", path.c_str(),
                                ec.message().c_str());
                return false;
            }
            if (fileSize > MAX_MOD_JSON_BYTES)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: '%s' is %llu bytes, above the %zu byte limit",
                                path.c_str(), static_cast<unsigned long long>(fileSize), MAX_MOD_JSON_BYTES);
                return false;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: cannot open '%s' (errno=%d)", path.c_str(),
                                errno);
                return false;
            }

            outContent.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            // The file may have grown between the stat and the read.
            if (outContent.size() > MAX_MOD_JSON_BYTES)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: '%s' grew past the %zu byte limit while reading",
                                path.c_str(), MAX_MOD_JSON_BYTES);
                outContent.clear();
                return false;
            }
            return true;
        }
    } // namespace

    bool ModSystem::SaveConfig(const std::string& filePath) const
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ModSystem::SaveConfig to '%s'", filePath.c_str());
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem::SaveConfig failed to open '%s'", filePath.c_str());
            return false;
        }

        auto modsArray = Json::Value::MakeArray();
        for (const auto& [id, info] : m_mods)
        {
            Json::Value entry;
            entry["id"] = Json::Value(id);
            entry["enabled"] = Json::Value(info.enabled);
            entry["loadOrder"] = Json::Value(info.loadOrder);
            modsArray.PushBack(std::move(entry));
        }

        Json::Value root;
        root["mods"] = std::move(modsArray);
        file << Json::StringifyPretty(root) << "\n";
        file.close();
        if (file.fail())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem::SaveConfig write failed for '%s'", filePath.c_str());
            return false;
        }
        return true;
    }

    bool ModSystem::LoadConfig(const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ModSystem::LoadConfig from '%s'", filePath.c_str());

        std::string content;
        if (!ReadModManifestFile(filePath, content))
        {
            return false;
        }

        Json::Value root;
        std::string parseError;
        if (!Json::ParseBounded(content, MOD_JSON_LIMITS, &root, &parseError))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "ModSystem::LoadConfig rejected '%s': %s", filePath.c_str(),
                           parseError.c_str());
            return false;
        }

        if (!root.IsObject() || !root.HasKey("mods") || !root["mods"].IsArray())
        {
            return false;
        }

        const auto& modsArray = root["mods"];
        for (size_t i = 0; i < modsArray.Size(); ++i)
        {
            const auto& entry = modsArray[i];
            if (!entry.IsObject() || !entry.HasKey("id") || !entry["id"].IsString())
            {
                continue;
            }

            std::string modId = entry["id"].AsString();
            auto modIt = m_mods.find(modId);
            if (modIt != m_mods.end())
            {
                modIt->second.enabled =
                    entry.HasKey("enabled") && entry["enabled"].IsBool() ? entry["enabled"].AsBool() : false;
                modIt->second.loadOrder =
                    entry.HasKey("loadOrder") && entry["loadOrder"].IsNumber() ? entry["loadOrder"].AsInt() : 0;
                m_modStates[modId] = modIt->second.enabled ? ModState::Available : ModState::Disabled;
            }
        }
        return true;
    }

    bool ModSystem::ParseModJson(const std::string& path, ModInfo& info)
    {
        std::string content;
        if (!ReadModManifestFile(path, content))
        {
            return false;
        }

        Json::Value root;
        std::string parseError;
        if (!Json::ParseBounded(content, MOD_JSON_LIMITS, &root, &parseError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: mod manifest '%s' rejected: %s", path.c_str(),
                            parseError.c_str());
            return false;
        }

        if (!root.IsObject())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: mod manifest '%s' is not a JSON object",
                            path.c_str());
            return false;
        }

        // Extract string fields
        auto getString = [&](const std::string& key) -> std::string
        {
            if (root.HasKey(key) && root[key].IsString())
            {
                return root[key].AsString();
            }
            return "";
        };

        info.id = getString("id");
        info.name = getString("name");
        info.author = getString("author");
        info.version = getString("version");
        info.description = getString("description");
        info.previewImage = getString("previewImage");

        // Extract dependencies array
        if (root.HasKey("dependencies") && root["dependencies"].IsArray())
        {
            const auto& deps = root["dependencies"];
            for (size_t i = 0; i < deps.Size(); ++i)
            {
                if (deps[i].IsString())
                {
                    info.dependencies.push_back(deps[i].AsString());
                }
            }
        }

        // Extract load order if present
        if (root.HasKey("loadOrder") && root["loadOrder"].IsNumber())
        {
            info.loadOrder = root["loadOrder"].AsInt();
        }

        return !info.id.empty();
    }

    std::string ModSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Mod System ===\n";
        oss << "Mods directory: " << m_modsDirectory << "\n";
        oss << "Total mods: " << m_mods.size() << "\n";
        size_t active = 0;
        for (const auto& [id, state] : m_modStates)
        {
            if (state == ModState::Active)
            {
                ++active;
            }
        }
        oss << "Active mods: " << active << "\n";
        return oss.str();
    }

    std::string ModSystem::Console_ListMods() const
    {
        std::ostringstream oss;
        oss << "=== Installed Mods ===\n";
        const char* stateNames[] = {"Available", "Loading", "Active", "Error", "Disabled"};
        for (const auto& [id, info] : m_mods)
        {
            auto stateIt = m_modStates.find(id);
            const char* state =
                (stateIt != m_modStates.end()) ? stateNames[static_cast<int>(stateIt->second)] : "Unknown";
            oss << "  " << info.name << " v" << info.version << " by " << info.author << " [" << state << "]\n";
        }
        return oss.str();
    }

} // namespace Spark
