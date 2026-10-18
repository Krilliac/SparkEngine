/**
 * @file ModSystem.cpp
 * @brief Implementation of the mod loading and management system
 *
 * Config save/load, mod.json parsing, and console output live in ModSystemIO.cpp; loading,
 * unloading and the script-content check live in ModSystemLifecycle.cpp.
 */

#include "ModSystem.h"
#include "HeldHandles.h"
#include "../../Utils/FileUtils.h"
#include "../../Utils/Validate.h"

#include <cstddef>
#include <cstdint>
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
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark
{

    namespace
    {
        /// Outcome of reading one mod's manifest through the held mods-root handle.
        enum class ManifestRead : std::uint8_t
        {
            Read,    ///< The manifest bytes were read from a plain file inside a plain mod directory
            Missing, ///< The mod directory holds no mod.json (any more)
            Refused  ///< A link, a non-regular file, an escape, an oversized file or an I/O error
        };

#ifdef _WIN32
        using HeldHandles::FinalPathOf;
        using HeldHandles::IsDirectChildPath;
        using HeldHandles::OpenPinnedDirectory;
        using HeldHandles::ScopedHandle;
#else
        using HeldHandles::ScopedFd;
#endif

        /// Holds the canonical mods root open for one scan and reads each mod's manifest
        /// through it. Every acceptance decision is made on what was actually opened, and the
        /// bytes are read from that same handle. POSIX opens the mod directory with openat()
        /// relative to the held root fd and mod.json relative to that directory fd, both
        /// O_NOFOLLOW, then fstat()s the file. Windows pins the root and the mod directory with
        /// handles that deny FILE_SHARE_DELETE (so neither can be renamed or replaced while
        /// open), opens both without following a reparse point, and checks attributes and final
        /// paths on the handles. The path checks in AcceptedModManifest are only an early,
        /// well-logged reject; a swap after them is caught here instead of being read through.
        class ModsRootReader
        {
          public:
            explicit ModsRootReader(const std::filesystem::path& canonicalRoot)
#ifdef _WIN32
                : m_root(canonicalRoot), m_handle(OpenPinnedDirectory(canonicalRoot, /*allowReparse=*/true))
            {
                if (m_handle.IsValid())
                {
                    m_final = FinalPathOf(m_handle.Get());
                }
            }
#else
                : m_handle(::open(canonicalRoot.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC))
            {
            }
#endif

            [[nodiscard]] bool IsOpen() const
            {
#ifdef _WIN32
                return m_handle.IsValid() && !m_final.empty();
#else
                return m_handle.Get() >= 0;
#endif
            }

            /// Reads <root>/@p directoryName/mod.json into @p content, bounded to
            /// ModSystem::kMaxManifestBytes. @p display labels log messages only.
            ManifestRead Read(const std::filesystem::path& directoryName, const std::string& display,
                              std::string& content) const;

          private:
            static ManifestRead Refuse(const std::string& display, const char* why)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "ModSystem: refusing mod '%s': %s", display.c_str(), why);
                return ManifestRead::Refused;
            }

#ifdef _WIN32
            std::filesystem::path m_root;
            ScopedHandle m_handle;
            std::wstring m_final;
#else
            ScopedFd m_handle;
#endif
        };

#ifdef _WIN32
        ManifestRead ModsRootReader::Read(const std::filesystem::path& directoryName, const std::string& display,
                                          std::string& content) const
        {
            // The pinned mod directory cannot be renamed or replaced while it is open, and it
            // must still be a plain (non-reparse) directory directly inside the pinned root.
            const ScopedHandle directory = OpenPinnedDirectory(m_root / directoryName, /*allowReparse=*/false);
            if (!directory.IsValid())
            {
                return Refuse(display, "the mod directory is no longer a plain directory");
            }
            const std::wstring directoryFinal = FinalPathOf(directory.Get());
            if (!IsDirectChildPath(directoryFinal, m_final))
            {
                return Refuse(display, "the mod directory no longer resolves directly inside the mods root");
            }

            const std::filesystem::path manifestPath = m_root / directoryName / L"mod.json";
            const ScopedHandle file(::CreateFileW(manifestPath.c_str(), GENERIC_READ,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file.IsValid())
            {
                const DWORD error = ::GetLastError();
                if (error == ERROR_FILE_NOT_FOUND)
                {
                    return ManifestRead::Missing;
                }
                return Refuse(display, "mod.json cannot be opened");
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (::GetFileType(file.Get()) != FILE_TYPE_DISK || !::GetFileInformationByHandle(file.Get(), &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            {
                return Refuse(display, "mod.json is a link or not a regular file");
            }
            if (!IsDirectChildPath(FinalPathOf(file.Get()), directoryFinal))
            {
                return Refuse(display, "mod.json does not resolve inside the mod directory");
            }
            const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
            if (size > ModSystem::kMaxManifestBytes)
            {
                return Refuse(display, "mod.json is above the manifest size limit");
            }

            // The size above is a fast reject; the read itself is bounded to one byte past the
            // limit, which is what caps memory if the file grows while it is read.
            content.assign(ModSystem::kMaxManifestBytes + 1, '\0');
            size_t total = 0;
            while (total < content.size())
            {
                DWORD got = 0;
                const DWORD request = static_cast<DWORD>(content.size() - total);
                if (!::ReadFile(file.Get(), content.data() + total, request, &got, nullptr))
                {
                    content.clear();
                    return Refuse(display, "mod.json could not be read");
                }
                if (got == 0)
                {
                    break;
                }
                total += got;
            }
            content.resize(total);
            if (total > ModSystem::kMaxManifestBytes)
            {
                content.clear();
                return Refuse(display, "mod.json grew past the manifest size limit while it was read");
            }
            return ManifestRead::Read;
        }
#else
        ManifestRead ModsRootReader::Read(const std::filesystem::path& directoryName, const std::string& display,
                                          std::string& content) const
        {
            // One path component, opened relative to the held root without following a link:
            // whatever now sits at that name must itself be a directory inside the root.
            const ScopedFd directory(
                ::openat(m_handle.Get(), directoryName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (directory.Get() < 0)
            {
                return Refuse(display, "the mod directory is no longer a plain directory");
            }

            // O_NONBLOCK keeps a FIFO planted as mod.json from blocking the open; the fstat
            // below refuses it (and every other non-regular file) on the opened descriptor.
            const ScopedFd file(
                ::openat(directory.Get(), "mod.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
            if (file.Get() < 0)
            {
                if (errno == ENOENT)
                {
                    return ManifestRead::Missing;
                }
                return Refuse(display, "mod.json is a link or cannot be opened");
            }
            struct stat info = {};
            if (::fstat(file.Get(), &info) != 0 || !S_ISREG(info.st_mode))
            {
                return Refuse(display, "mod.json is not a regular file");
            }
            if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > ModSystem::kMaxManifestBytes)
            {
                return Refuse(display, "mod.json is above the manifest size limit");
            }

            // The size above is a fast reject; the read itself is bounded to one byte past the
            // limit, which is what caps memory if the file grows while it is read.
            content.assign(ModSystem::kMaxManifestBytes + 1, '\0');
            size_t total = 0;
            while (total < content.size())
            {
                const ssize_t got = ::read(file.Get(), content.data() + total, content.size() - total);
                if (got < 0 && errno == EINTR)
                {
                    continue;
                }
                if (got < 0)
                {
                    content.clear();
                    return Refuse(display, "mod.json could not be read");
                }
                if (got == 0)
                {
                    break;
                }
                total += static_cast<size_t>(got);
            }
            content.resize(total);
            if (total > ModSystem::kMaxManifestBytes)
            {
                content.clear();
                return Refuse(display, "mod.json grew past the manifest size limit while it was read");
            }
            return ManifestRead::Read;
        }
#endif

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

        // The root is trusted configuration; it is opened once here and every manifest is
        // read relative to that handle, so nothing below it is re-resolved by path.
        const ModsRootReader reader(canonicalRoot);
        if (!reader.IsOpen())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "ModSystem: cannot open mods directory '%s'",
                           modsDirectory.c_str());
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
                if (!manifestPath)
                {
                    continue;
                }
                if (m_manifestOpenProbe)
                {
                    m_manifestOpenProbe(PathToUtf8(it->path()));
                }
                std::string content;
                ModInfo info;
                if (reader.Read(it->path().filename(), PathToUtf8(it->path()), content) == ManifestRead::Read &&
                    ParseModJson(content, PathToUtf8(*manifestPath), info))
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
