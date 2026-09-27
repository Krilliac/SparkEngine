/**
 * @file ModSystem.cpp
 * @brief Implementation of the mod loading and management system
 *
 * Config save/load, mod.json parsing, and console output live in ModSystemIO.cpp.
 */

#include "ModSystem.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

namespace Spark
{

    namespace
    {
        enum class ModScriptScan
        {
            None,   ///< No script content found
            Found,  ///< A Scripts/ directory or an .as file is present
            Unknown ///< The tree could not be fully inspected (fail closed)
        };

        /// Upper bound on directory entries inspected per mod; a larger tree is refused.
        constexpr size_t kMaxModEntriesInspected = 20'000;

        /// Case-insensitive comparison of a path component in its native encoding against a
        /// lower-case ASCII literal. Working on native() avoids path::string(), which on
        /// Windows converts UTF-16 to the ANSI code page and throws for any character that
        /// code page cannot represent (e.g. a Japanese file name on an en-US machine).
        /// Non-ASCII characters simply never match.
        bool NativeEqualsAsciiNoCase(const std::filesystem::path::string_type& native, std::string_view lowerAscii)
        {
            using CharT = std::filesystem::path::value_type;
            if (native.size() != lowerAscii.size())
                return false;
            for (size_t i = 0; i < native.size(); ++i)
            {
                CharT c = native[i];
                if (c >= static_cast<CharT>('A') && c <= static_cast<CharT>('Z'))
                    c = static_cast<CharT>(c - static_cast<CharT>('A') + static_cast<CharT>('a'));
                if (c != static_cast<CharT>(static_cast<unsigned char>(lowerAscii[i])))
                    return false;
            }
            return true;
        }

        /// Looks for executable mod content: a Scripts/ directory (the documented layout)
        /// or any AngelScript (.as) file anywhere in the mod tree. Directory symlinks are
        /// not followed. Any failure while walking the untrusted tree reports Unknown, so
        /// the caller refuses the mod instead of letting an exception escape LoadMod.
        ModScriptScan FindModScriptContent(const std::string& modPath)
        {
            namespace fs = std::filesystem;
            try
            {
                std::error_code ec;
                fs::recursive_directory_iterator it(modPath, fs::directory_options::none, ec);
                if (ec)
                    return ModScriptScan::Unknown;

                const fs::recursive_directory_iterator end;
                size_t inspected = 0;
                while (it != end)
                {
                    if (++inspected > kMaxModEntriesInspected)
                        return ModScriptScan::Unknown;

                    const fs::path& entryPath = it->path();
                    std::error_code typeEc;
                    if (it->is_directory(typeEc) && NativeEqualsAsciiNoCase(entryPath.filename().native(), "scripts"))
                        return ModScriptScan::Found;
                    if (NativeEqualsAsciiNoCase(entryPath.extension().native(), ".as"))
                        return ModScriptScan::Found;

                    it.increment(ec);
                    if (ec)
                        return ModScriptScan::Unknown;
                }
                return ModScriptScan::None;
            }
            catch (const std::exception&)
            {
                return ModScriptScan::Unknown;
            }
        }
    } // namespace

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
            // Reject symlinks so a crafted mods directory cannot redirect us outside
            // the intended sandbox (e.g. into /etc or the player's home directory).
            // A status query that fails is treated the same way: unknown is not safe.
            std::error_code symEc;
            const fs::file_status status = entry.symlink_status(symEc);
            if (symEc || fs::is_symlink(status))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "ModSystem: skipping symlinked or unreadable mod directory '%s'",
                               entry.path().string().c_str());
                continue;
            }

            std::string modJsonPath = entry.path().string() + "/mod.json";
            if (!fs::exists(modJsonPath))
            {
                continue;
            }
            // The manifest itself must be a plain file inside the mod directory. A symlinked
            // mod.json could point outside the mods tree or at a file whose reported size
            // lies (procfs), so it is refused like a symlinked mod directory.
            std::error_code manifestEc;
            const fs::file_status manifestStatus = fs::symlink_status(modJsonPath, manifestEc);
            if (manifestEc || !fs::is_regular_file(manifestStatus))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "ModSystem: skipping mod '%s' whose mod.json is a symlink or not a regular file",
                               entry.path().string().c_str());
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
        // Collect enabled mods, then order them by loadOrder (id as tiebreaker) so the
        // topological walk below is deterministic regardless of map iteration order.
        std::vector<std::string> enabled;
        for (const auto& [id, info] : m_mods)
        {
            if (info.enabled)
            {
                enabled.push_back(id);
            }
        }
        std::sort(enabled.begin(), enabled.end(),
                  [this](const std::string& a, const std::string& b)
                  {
                      const int orderA = m_mods[a].loadOrder;
                      const int orderB = m_mods[b].loadOrder;
                      if (orderA != orderB)
                          return orderA < orderB;
                      return a < b;
                  });

        // Topologically sort so each mod is loaded after the (enabled) dependencies it
        // declares. DFS post-order with cycle detection; loadOrder acts only as a
        // tiebreaker among mods with no ordering constraint between them.
        std::vector<std::string> ordered;
        ordered.reserve(enabled.size());
        std::unordered_map<std::string, int> visitState; // 0=unvisited, 1=in-progress, 2=done
        bool cycleDetected = false;

        std::function<void(const std::string&)> visit = [&](const std::string& id)
        {
            int& state = visitState[id];
            if (state == 2)
            {
                return;
            }
            if (state == 1)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "LoadEnabledMods: dependency cycle detected involving mod '%s'", id.c_str());
                cycleDetected = true;
                return;
            }
            state = 1;
            auto it = m_mods.find(id);
            if (it != m_mods.end())
            {
                for (const auto& dep : it->second.dependencies)
                {
                    // Only order against enabled dependencies; missing or disabled
                    // dependencies are reported by LoadMod's dependency check.
                    auto depIt = m_mods.find(dep);
                    if (depIt != m_mods.end() && depIt->second.enabled)
                    {
                        visit(dep);
                    }
                }
            }
            state = 2;
            ordered.push_back(id);
        };

        for (const auto& id : enabled)
        {
            visit(id);
        }

        if (cycleDetected)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "LoadEnabledMods: aborting — dependency cycle among enabled mods");
            return false;
        }

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

        // Every declared dependency must already be loaded, not merely enabled — otherwise
        // this mod would half-initialize against a dependency that has not run yet.
        // LoadEnabledMods loads in topological order so this holds; a direct out-of-order
        // LoadMod call fails loudly here instead of silently loading against nothing.
        for (const auto& dep : it->second.dependencies)
        {
            auto depIt = m_mods.find(dep);
            if (depIt == m_mods.end() || !depIt->second.loaded)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "LoadMod '%s' failed — dependency '%s' is not loaded yet",
                                modId.c_str(), dep.c_str());
                m_modStates[modId] = ModState::Error;
                return false;
            }
        }

        // The engine has no sandboxed loader for mod scripts, and mod.allowScriptMods /
        // mod.sandboxMods are not enforced by anything. A mod that ships executable
        // content must therefore never be reported Active: that status would claim its
        // scripts were vetted and running when nothing ran them.
        switch (FindModScriptContent(it->second.path))
        {
        case ModScriptScan::None:
            break;
        case ModScriptScan::Found:
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "LoadMod '%s' refused — it ships script content (Scripts/ or *.as) and no sandboxed "
                            "mod-script loader exists",
                            modId.c_str());
            m_modStates[modId] = ModState::Error;
            return false;
        case ModScriptScan::Unknown:
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "LoadMod '%s' refused — its directory '%s' could not be fully inspected for script content",
                            modId.c_str(), it->second.path.c_str());
            m_modStates[modId] = ModState::Error;
            return false;
        }

        m_modStates[modId] = ModState::Loading;

        // The engine loads no mod content itself. Active means the mod passed validation
        // and was announced to the OnModLoaded subscribers, which own loading its assets.
        it->second.loaded = true;
        m_modStates[modId] = ModState::Active;

        for (const auto& callback : m_loadCallbacks)
        {
            SPARK_GUARDED_UPDATE("Mod:LoadCallback", "Game", { callback(modId); });
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
            SPARK_GUARDED_UPDATE("Mod:UnloadCallback", "Game", { callback(modId); });
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

    void ModSystem::OnModLoaded(std::function<void(const std::string&)> callback)
    {
        m_loadCallbacks.push_back(std::move(callback));
    }

    void ModSystem::OnModUnloaded(std::function<void(const std::string&)> callback)
    {
        m_unloadCallbacks.push_back(std::move(callback));
    }

} // namespace Spark
