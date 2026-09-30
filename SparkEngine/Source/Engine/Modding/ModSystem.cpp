/**
 * @file ModSystem.cpp
 * @brief Implementation of the mod loading and management system
 *
 * Config save/load, mod.json parsing, and console output live in ModSystemIO.cpp; loading,
 * unloading and the script-content check live in ModSystemLifecycle.cpp.
 */

#include "ModSystem.h"
#include "../../Utils/FileUtils.h"
#include "../../Utils/Validate.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace Spark
{

    namespace
    {
        /// UTF-8 rendering of a native path. Never path::string(): on Windows that converts
        /// through the ANSI code page and throws for a name the code page cannot represent.
        std::string PathToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        /// True when @p path is a Windows reparse point (symlink, junction, mount point, or any
        /// other tag) or its attributes cannot be read. MSVC reports a junction as
        /// file_type::junction rather than a symlink, so is_symlink() alone misses it.
        bool IsReparsePointOrUnreadable([[maybe_unused]] const std::filesystem::path& path)
        {
#ifdef _WIN32
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            return false;
#endif
        }

        /// A mod directory must be a real directory that is a direct child of the scanned
        /// mods root: not a symlink, not a junction or other reparse point, and its canonical
        /// location must still sit directly under the canonical root. Anything that cannot be
        /// inspected is refused.
        bool IsPlainChildDirectory(const std::filesystem::directory_entry& entry,
                                   const std::filesystem::path& canonicalRoot)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::file_status status = entry.symlink_status(ec);
            // An exact type match rejects file_type::symlink and MSVC's file_type::junction.
            if (ec || status.type() != fs::file_type::directory)
            {
                return false;
            }
            if (IsReparsePointOrUnreadable(entry.path()))
            {
                return false;
            }
            const fs::path canonicalEntry = fs::canonical(entry.path(), ec);
            return !ec && canonicalEntry.parent_path() == canonicalRoot;
        }

        /// Returns the manifest path of @p entry when it is an acceptable mod directory:
        /// a plain child directory of the mods root holding a plain-file mod.json.
        /// Everything else (files, links, junctions, unreadable entries, missing or linked
        /// manifests) yields nullopt. Throws only if a name cannot be rendered as UTF-8.
        std::optional<std::filesystem::path> AcceptedModManifest(const std::filesystem::directory_entry& entry,
                                                                 const std::filesystem::path& canonicalRoot)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!entry.is_directory(ec) || ec)
            {
                return std::nullopt; // plain files (and dangling links) in the mods root are not mods
            }

            // Reject symlinks, junctions and other reparse points, and anything whose real
            // location is not directly under the mods root, so a crafted mods directory cannot
            // redirect us outside the intended sandbox (e.g. into /etc or the player's home
            // directory). A status query that fails is treated the same way: unknown is not safe.
            if (!IsPlainChildDirectory(entry, canonicalRoot))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "ModSystem: skipping linked, reparse-point or unreadable mod directory '%s'",
                               PathToUtf8(entry.path()).c_str());
                return std::nullopt;
            }

            // The manifest itself must be a plain file inside the mod directory. A symlinked
            // mod.json could point outside the mods tree or at a file whose reported size
            // lies (procfs), so it is refused like a symlinked mod directory.
            fs::path manifestPath = entry.path() / "mod.json";
            const fs::file_status manifestStatus = fs::symlink_status(manifestPath, ec);
            if (manifestStatus.type() == fs::file_type::not_found)
            {
                return std::nullopt;
            }
            if (ec || manifestStatus.type() != fs::file_type::regular || IsReparsePointOrUnreadable(manifestPath))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "ModSystem: skipping mod '%s' whose mod.json is a link or not a regular file",
                               PathToUtf8(entry.path()).c_str());
                return std::nullopt;
            }
            return manifestPath;
        }

        /// Publishes one completed scan. An id claimed by more than one directory is
        /// published from none of them: which one would win depends on directory order,
        /// so a dropped-in mod could otherwise shadow an installed one. For an id already
        /// known, only the manifest metadata is refreshed; enabled, loaded, loadOrder and
        /// the ModState stay, so an Active mod remains Active and UnloadAll still unloads
        /// it. Returns the number of ids published by this scan.
        size_t PublishScannedMods(std::map<std::string, std::vector<ModInfo>>& scanned,
                                  std::unordered_map<std::string, ModInfo>& mods,
                                  std::unordered_map<std::string, ModState>& states)
        {
            size_t published = 0;
            for (auto& [id, claims] : scanned)
            {
                if (claims.size() != 1)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core,
                                   "ModSystem: %zu mod directories declare id '%s'; none of them is published",
                                   claims.size(), id.c_str());
                    continue;
                }
                ModInfo& found = claims.front();
                const auto existing = mods.find(id);
                if (existing == mods.end())
                {
                    states[id] = ModState::Available;
                    mods.emplace(id, std::move(found));
                }
                else
                {
                    ModInfo& info = existing->second;
                    info.name = std::move(found.name);
                    info.author = std::move(found.author);
                    info.version = std::move(found.version);
                    info.description = std::move(found.description);
                    info.previewImage = std::move(found.previewImage);
                    info.dependencies = std::move(found.dependencies);
                    // Keep the path that was validated when the mod became active. A rescan
                    // can target a different root (or observe a replacement directory), but
                    // changing the path underneath an active mod would make its eventual
                    // unload operate on a different resource owner than the one announced to
                    // subscribers. Metadata can refresh while the active ownership anchor stays
                    // stable; an inactive mod may adopt the newly discovered path.
                    if (!info.loaded)
                    {
                        info.path = std::move(found.path);
                    }
                }
                ++published;
            }
            return published;
        }
    } // namespace

    ModSystem::ModSystem() = default;

    size_t ModSystem::ScanForMods(const std::string& modsDirectory)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Scanning for mods in '%s'", modsDirectory.c_str());
        m_modsDirectory = modsDirectory;

        // The mods directory is untrusted: every filesystem query uses the error_code
        // overloads and every path stays an fs::path, converted to UTF-8 only for storage
        // and logging. One unreadable entry or unrepresentable name skips that entry; it
        // never throws out of the scan and never discards the mods already found.
        namespace fs = std::filesystem;
        const fs::path root = FileUtils::PathFromUtf8(modsDirectory);
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec))
        {
            return 0;
        }
        const fs::path canonicalRoot = fs::canonical(root, ec);
        if (ec)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "ModSystem: cannot resolve mods directory '%s' (%s)",
                           modsDirectory.c_str(), ec.message().c_str());
            return 0;
        }

        // Every accepted manifest is parsed before anything is published, so an id that
        // two directories claim is known before either could shadow the other.
        std::map<std::string, std::vector<ModInfo>> scanned;
        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            try
            {
                const std::optional<fs::path> manifestPath = AcceptedModManifest(*it, canonicalRoot);
                if (manifestPath && m_manifestOpenProbe)
                {
                    m_manifestOpenProbe(PathToUtf8(it->path()));
                }
                ModInfo info;
                if (manifestPath && ParseModJson(PathToUtf8(*manifestPath), info))
                {
                    info.path = PathToUtf8(it->path());
                    std::string id = info.id;
                    scanned[std::move(id)].push_back(std::move(info));
                }
            }
            catch (const std::exception& error)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "ModSystem: skipping uninspectable mods-directory entry (%s)",
                               error.what());
            }
        }
        if (ec)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "ModSystem: scan of '%s' stopped early (%s)",
                           modsDirectory.c_str(), ec.message().c_str());
        }

        return PublishScannedMods(scanned, m_mods, m_modStates);
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

    void ModSystem::SetManifestOpenProbeForTesting(std::function<void(const std::string&)> probe)
    {
        m_manifestOpenProbe = std::move(probe);
    }

} // namespace Spark
