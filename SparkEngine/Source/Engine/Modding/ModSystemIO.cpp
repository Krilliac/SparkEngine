/**
 * @file ModSystemIO.cpp
 * @brief Mod system persistence and console output — config save/load, mod.json parsing, console status
 */

#include "ModSystem.h"
#include "../../Utils/JsonUtils.h"
#include "../../Utils/LogMacros.h"

#include <cerrno>
#include <fstream>
#include <sstream>

namespace Spark
{

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
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "ModSystem::LoadConfig failed to open '%s'", filePath.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto root = Json::Parse(content);
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
        std::ifstream file(path);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: cannot open mod manifest '%s' (errno=%d)",
                            path.c_str(), errno);
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto root = Json::Parse(content);
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
