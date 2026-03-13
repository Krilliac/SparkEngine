/**
 * @file ModSystem.cpp
 * @brief Implementation of the mod loading and management system
 */

#include "ModSystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
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
            return false;
        }
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
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }
        file << "{\n  \"mods\": [\n";
        bool first = true;
        for (const auto& [id, info] : m_mods)
        {
            if (!first)
            {
                file << ",\n";
            }
            first = false;
            file << "    {\"id\": \"" << id << "\", \"enabled\": " << (info.enabled ? "true" : "false")
                 << ", \"loadOrder\": " << info.loadOrder << "}";
        }
        file << "\n  ]\n}\n";
        return true;
    }

    bool ModSystem::LoadConfig(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        std::regex modRegex(
            R"~~("id"\s*:\s*"(\w+)"\s*,\s*"enabled"\s*:\s*(true|false)\s*,\s*"loadOrder"\s*:\s*(\d+))~~");
        auto begin = std::sregex_iterator(content.begin(), content.end(), modRegex);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it)
        {
            const std::smatch& match = *it;
            std::string modId = match[1].str();
            auto modIt = m_mods.find(modId);
            if (modIt != m_mods.end())
            {
                modIt->second.enabled = (match[2].str() == "true");
                modIt->second.loadOrder = std::stoi(match[3].str());
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

        auto extractString = [&](const std::string& key) -> std::string
        {
            std::regex r("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
            std::smatch m;
            if (std::regex_search(content, m, r))
            {
                return m[1].str();
            }
            return "";
        };

        info.id = extractString("id");
        info.name = extractString("name");
        info.author = extractString("author");
        info.version = extractString("version");
        info.description = extractString("description");

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
