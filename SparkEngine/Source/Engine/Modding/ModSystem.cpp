/**
 * @file ModSystem.cpp
 * @brief Implementation of the mod loading and management system
 */

#include "ModSystem.h"
#include "../../Utils/JsonUtils.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Spark
{

    ModSystem::ModSystem() = default;

    size_t ModSystem::ScanForMods(const std::string& modsDirectory)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Scanning for mods in '%s'", modsDirectory.c_str());
        m_modsDirectory = modsDirectory;
        size_t found = 0;

        namespace fs = std::filesystem;
        if (!fs::exists(modsDirectory))
        {
            return 0;
        }

        for (const auto& entry : fs::directory_iterator(modsDirectory))
        {
            if (!entry.is_directory())
            {
                continue;
            }

            std::string modJsonPath = entry.path().string() + "/mod.json";
            if (!fs::exists(modJsonPath))
            {
                continue;
            }

            ModInfo info;
            if (ParseModJson(modJsonPath, info))
            {
                info.path = entry.path().string();
                m_mods[info.id] = info;
                m_modStates[info.id] = ModState::Available;
                ++found;
            }
        }

        return found;
    }

    bool ModSystem::EnableMod(const std::string& modId)
    {
        auto it = m_mods.find(modId);
        if (it == m_mods.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "EnableMod: mod '%s' not found", modId.c_str());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "EnableMod '%s'", modId.c_str());
        it->second.enabled = true;
        if (m_modStates[modId] == ModState::Disabled)
        {
            m_modStates[modId] = ModState::Available;
        }
        return true;
    }

    void ModSystem::DisableMod(const std::string& modId)
    {
        auto it = m_mods.find(modId);
        if (it != m_mods.end())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "DisableMod '%s'", modId.c_str());
            it->second.enabled = false;
            m_modStates[modId] = ModState::Disabled;
        }
    }

    bool ModSystem::LoadEnabledMods()
    {
        // Sort by load order
        std::vector<std::string> ordered;
        for (const auto& [id, info] : m_mods)
        {
            if (info.enabled)
            {
                ordered.push_back(id);
            }
        }
        std::sort(ordered.begin(), ordered.end(), [this](const std::string& a, const std::string& b)
                  { return m_mods[a].loadOrder < m_mods[b].loadOrder; });

        bool allSuccess = true;
        for (const auto& id : ordered)
        {
            if (!LoadMod(id))
            {
                allSuccess = false;
            }
        }
        return allSuccess;
    }

    void ModSystem::UnloadAll()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Unloading all mods");
        for (auto& [id, info] : m_mods)
        {
            if (info.loaded)
            {
                UnloadMod(id);
            }
        }
    }

    bool ModSystem::LoadMod(const std::string& modId)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Loading mod '%s'", modId.c_str());
        auto it = m_mods.find(modId);
        if (it == m_mods.end())
        {
            return false;
        }

        if (!AreDependenciesMet(modId))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "LoadMod '%s' failed — unmet dependencies", modId.c_str());
            m_modStates[modId] = ModState::Error;
            return false;
        }

        m_modStates[modId] = ModState::Loading;

        // Load mod assets, scripts, etc.
        // This is a framework — actual loading depends on mod contents
        it->second.loaded = true;
        m_modStates[modId] = ModState::Active;

        for (const auto& callback : m_loadCallbacks)
        {
            callback(modId);
        }
        return true;
    }

    void ModSystem::UnloadMod(const std::string& modId)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Unloading mod '%s'", modId.c_str());
        auto it = m_mods.find(modId);
        if (it == m_mods.end())
        {
            return;
        }

        it->second.loaded = false;
        m_modStates[modId] = it->second.enabled ? ModState::Available : ModState::Disabled;

        for (const auto& callback : m_unloadCallbacks)
        {
            callback(modId);
        }
    }

    std::vector<ModInfo> ModSystem::GetAllMods() const
    {
        std::vector<ModInfo> result;
        result.reserve(m_mods.size());
        for (const auto& [id, info] : m_mods)
        {
            result.push_back(info);
        }
        return result;
    }

    std::vector<ModInfo> ModSystem::GetEnabledMods() const
    {
        std::vector<ModInfo> result;
        for (const auto& [id, info] : m_mods)
        {
            if (info.enabled)
            {
                result.push_back(info);
            }
        }
        return result;
    }

    const ModInfo* ModSystem::GetModInfo(const std::string& modId) const
    {
        auto it = m_mods.find(modId);
        return it != m_mods.end() ? &it->second : nullptr;
    }

    ModState ModSystem::GetModState(const std::string& modId) const
    {
        auto it = m_modStates.find(modId);
        return it != m_modStates.end() ? it->second : ModState::Available;
    }

    bool ModSystem::IsModActive(const std::string& modId) const
    {
        auto it = m_modStates.find(modId);
        return it != m_modStates.end() && it->second == ModState::Active;
    }

    void ModSystem::SetLoadOrder(const std::vector<std::string>& orderedModIds)
    {
        for (size_t i = 0; i < orderedModIds.size(); ++i)
        {
            auto it = m_mods.find(orderedModIds[i]);
            if (it != m_mods.end())
            {
                it->second.loadOrder = static_cast<int>(i);
            }
        }
    }

    bool ModSystem::AreDependenciesMet(const std::string& modId) const
    {
        auto it = m_mods.find(modId);
        if (it == m_mods.end())
        {
            return false;
        }
        for (const auto& dep : it->second.dependencies)
        {
            auto depIt = m_mods.find(dep);
            if (depIt == m_mods.end() || !depIt->second.enabled)
            {
                return false;
            }
        }
        return true;
    }

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

    void ModSystem::OnModLoaded(std::function<void(const std::string&)> callback)
    {
        m_loadCallbacks.push_back(std::move(callback));
    }

    void ModSystem::OnModUnloaded(std::function<void(const std::string&)> callback)
    {
        m_unloadCallbacks.push_back(std::move(callback));
    }

    bool ModSystem::ParseModJson(const std::string& path, ModInfo& info)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto root = Json::Parse(content);
        if (!root.IsObject())
        {
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
