/**
 * @file ModSystemLifecycle.cpp
 * @brief ModSystem load and unload: dependency ordering, the script-content refusal and
 *        the guarded OnModLoaded / OnModUnloaded callbacks.
 *
 * Discovery (ScanForMods) and the queries live in ModSystem.cpp, manifest and config I/O
 * in ModSystemIO.cpp. This file is the only one that dispatches subscriber callbacks
 * through SPARK_GUARDED_UPDATE, so the discovery path links without the fault isolator.
 */

#include "ModSystem.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/FileUtils.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Spark
{

    namespace
    {
        enum class ModScriptScan : std::uint8_t
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
            {
                return false;
            }
            for (size_t i = 0; i < native.size(); ++i)
            {
                CharT c = native[i];
                if (c >= static_cast<CharT>('A') && c <= static_cast<CharT>('Z'))
                {
                    c = static_cast<CharT>(c - static_cast<CharT>('A') + static_cast<CharT>('a'));
                }
                if (c != static_cast<CharT>(static_cast<unsigned char>(lowerAscii[i])))
                {
                    return false;
                }
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
                const fs::path root = FileUtils::PathFromUtf8(modPath);
                if (root.empty())
                {
                    return ModScriptScan::Unknown;
                }
                fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
                if (ec)
                {
                    return ModScriptScan::Unknown;
                }

                const fs::recursive_directory_iterator end;
                size_t inspected = 0;
                while (it != end)
                {
                    if (++inspected > kMaxModEntriesInspected)
                    {
                        return ModScriptScan::Unknown;
                    }

                    const fs::path& entryPath = it->path();
                    std::error_code typeEc;
                    if (it->is_directory(typeEc) && NativeEqualsAsciiNoCase(entryPath.filename().native(), "scripts"))
                    {
                        return ModScriptScan::Found;
                    }
                    if (NativeEqualsAsciiNoCase(entryPath.extension().native(), ".as"))
                    {
                        return ModScriptScan::Found;
                    }

                    it.increment(ec);
                    if (ec)
                    {
                        return ModScriptScan::Unknown;
                    }
                }
                return ModScriptScan::None;
            }
            catch (const std::exception&)
            {
                return ModScriptScan::Unknown;
            }
        }
    } // namespace

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
                      {
                          return orderA < orderB;
                      }
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

} // namespace Spark
