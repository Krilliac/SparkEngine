/**
 * @file VirtualFileSystem.cpp
 * @brief Implementation of the mount-priority virtual filesystem
 */

#include "VirtualFileSystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Spark
{

    namespace
    {
        /// Win32 resolves these names as devices regardless of the directory
        /// prefix, ignoring an extension and any trailing spaces or dots.
        bool IsReservedDeviceName(std::string_view component)
        {
            const size_t dot = component.find('.');
            std::string stem(component.substr(0, dot == std::string_view::npos ? component.size() : dot));
            while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
                stem.pop_back();
            for (char& c : stem)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                return true;
            if (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' &&
                stem[3] <= '9')
            {
                return true;
            }
            return false;
        }

        /// Containment decided on already-normalized paths: @p child must sit under
        /// @p parent without climbing out of it.
        bool IsContainedIn(const fs::path& child, const fs::path& parent)
        {
            const fs::path relative = child.lexically_relative(parent);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        }
    } // namespace

    bool IsVirtualPathSafe(const std::string& virtualPath)
    {
        if (virtualPath.empty())
            return false;

        // ':' is never legitimate in a mount-relative path and carries two Windows
        // escapes at once: "C:evil" (drive-relative) and "logo.png:secret" (NTFS
        // alternate data stream).
        if (virtualPath.find(':') != std::string::npos)
            return false;

        if (virtualPath.front() == '/' || virtualPath.front() == '\\')
            return false;

        const fs::path candidate = fs::path(virtualPath).lexically_normal();
        if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory())
            return false;

        for (const auto& component : candidate)
        {
            const std::string text = component.string();
            if (text == "..")
                return false;
            if (IsReservedDeviceName(text))
                return false;
        }
        return true;
    }

    // =========================================================================
    // LocalFileProvider
    // =========================================================================

    LocalFileProvider::LocalFileProvider(const std::string& rootPath) : m_rootPath(rootPath)
    {
        // Normalize trailing separator
        if (!m_rootPath.empty() && m_rootPath.back() != '/' && m_rootPath.back() != '\\')
        {
            m_rootPath += '/';
        }

        m_root = fs::path(m_rootPath).lexically_normal();
        if (m_root.filename().empty())
            m_root = m_root.parent_path();
        if (m_root.empty())
            m_root = fs::path(".");

        // Resolve the root's own links once so per-path containment compares like
        // with like. A root that does not exist yet keeps its lexical form.
        std::error_code ec;
        m_canonicalRoot = fs::weakly_canonical(m_root, ec);
        if (ec || m_canonicalRoot.empty())
            m_canonicalRoot = m_root;
    }

    std::string LocalFileProvider::ResolvePath(const std::string& virtualPath) const
    {
        // Return empty so Exists/ReadFile fail cleanly instead of silently
        // substituting the sandbox root (which used to mask attacker intent).
        if (!IsVirtualPathSafe(virtualPath))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: rejected unsafe virtual path '%s'", virtualPath.c_str());
            return {};
        }

        const fs::path full = (m_root / fs::path(virtualPath)).lexically_normal();
        if (!IsContainedIn(full, m_root))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: rejected path escaping mount root '%s'",
                           virtualPath.c_str());
            return {};
        }

        // Lexical containment cannot see a symlink or junction placed INSIDE the
        // mount, which is the standard way out of a sandbox whose only check is
        // textual. Resolve links across the part of the path that exists and
        // confirm the result is still under the equally-resolved root.
        // A resolver that cannot answer is not an answer: treating an error as
        // "no link found" switches the guard off for exactly the paths an
        // attacker controls (an unopenable reparse point, a path past MAX_PATH,
        // a dead network mount). Unknown is not safe — reject and say so.
        std::error_code ec;
        const fs::path resolved = fs::weakly_canonical(full, ec);
        if (ec || resolved.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: rejected unresolvable path '%s': %s", virtualPath.c_str(),
                           ec ? ec.message().c_str() : "empty canonical form");
            return {};
        }
        if (!IsContainedIn(resolved, m_canonicalRoot))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: rejected link escaping mount root '%s'",
                           virtualPath.c_str());
            return {};
        }

        return full.string();
    }

    bool LocalFileProvider::Exists(const std::string& virtualPath) const
    {
        return fs::exists(ResolvePath(virtualPath));
    }

    std::vector<uint8_t> LocalFileProvider::ReadFile(const std::string& virtualPath) const
    {
        std::string fullPath = ResolvePath(virtualPath);
        std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Failed to open file '%s'", fullPath.c_str());
            return {};
        }

        auto size = file.tellg();
        if (size <= 0)
        {
            return {};
        }

        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        if (file.bad())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Read error for '%s'", fullPath.c_str());
            return {};
        }
        return buffer;
    }

    std::string LocalFileProvider::ReadTextFile(const std::string& virtualPath) const
    {
        std::string fullPath = ResolvePath(virtualPath);
        std::ifstream file(fullPath);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Failed to open text file '%s'", fullPath.c_str());
            return {};
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::vector<std::string> LocalFileProvider::ListFiles(const std::string& directory,
                                                          const std::string& extension) const
    {
        std::vector<std::string> results;
        // An empty directory means the mount root itself, which is not a path the
        // containment policy is asked about.
        std::string fullDir = directory.empty() ? m_root.string() : ResolvePath(directory);

        if (!fs::exists(fullDir) || !fs::is_directory(fullDir))
        {
            return results;
        }

        for (const auto& entry : fs::directory_iterator(fullDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            std::string filename = entry.path().filename().string();

            if (!extension.empty())
            {
                std::string ext = entry.path().extension().string();
                if (ext != extension)
                {
                    continue;
                }
            }

            // Return virtual path relative to the mount root
            std::string virtualPath = directory;
            if (!virtualPath.empty() && virtualPath.back() != '/')
            {
                virtualPath += '/';
            }
            virtualPath += filename;
            results.push_back(virtualPath);
        }

        return results;
    }

    std::string LocalFileProvider::GetProviderName() const
    {
        return "LocalFile(" + m_rootPath + ")";
    }

    // =========================================================================
    // VirtualFileSystem
    // =========================================================================

    VirtualFileSystem& VirtualFileSystem::GetInstance()
    {
        static VirtualFileSystem instance;
        return instance;
    }

    bool VirtualFileSystem::Initialize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
        {
            return true;
        }

        m_mounts.clear();
        m_initialized = true;
        return true;
    }

    void VirtualFileSystem::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mounts.clear();
        m_initialized = false;
    }

    void VirtualFileSystem::Mount(const std::string& name, std::unique_ptr<IResourceProvider> provider,
                                  int32_t priority)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Remove existing mount with same name, if any
        std::erase_if(m_mounts, [&name](const MountPoint& mp) { return mp.name == name; });

        MountPoint mp;
        mp.name = name;
        mp.provider = std::move(provider);
        mp.priority = priority;
        m_mounts.push_back(std::move(mp));
    }

    void VirtualFileSystem::Unmount(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::erase_if(m_mounts, [&name](const MountPoint& mp) { return mp.name == name; });
    }

    std::vector<const MountPoint*> VirtualFileSystem::GetSortedMounts() const
    {
        std::vector<const MountPoint*> sorted;
        sorted.reserve(m_mounts.size());
        for (const auto& mp : m_mounts)
        {
            sorted.push_back(&mp);
        }

        // Descending priority — highest priority first
        std::sort(sorted.begin(), sorted.end(),
                  [](const MountPoint* a, const MountPoint* b) { return a->priority > b->priority; });

        return sorted;
    }

    bool VirtualFileSystem::Exists(const std::string& virtualPath) const
    {
        if (!IsVirtualPathSafe(virtualPath))
            return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto* mp : GetSortedMounts())
        {
            if (mp->provider->Exists(virtualPath))
            {
                return true;
            }
        }
        return false;
    }

    std::vector<uint8_t> VirtualFileSystem::ReadFile(const std::string& virtualPath) const
    {
        // Decide the policy once, here. Letting each provider reject the path and
        // then continuing the walk re-offers a rejected path to every lower-priority
        // mount, one of which (an archive provider) does no containment check at all.
        if (!IsVirtualPathSafe(virtualPath))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: refusing to read unsafe path '%s'", virtualPath.c_str());
            return {};
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto* mp : GetSortedMounts())
        {
            // The first mount that HAS the file wins, empty or not. Treating an
            // empty read as a failure and falling through silently inverted the
            // priority order for zero-byte files, so a mod that blanks a config by
            // shipping an empty override still got the engine's original.
            if (mp->provider->Exists(virtualPath))
                return mp->provider->ReadFile(virtualPath);
        }
        return {};
    }

    std::string VirtualFileSystem::ReadTextFile(const std::string& virtualPath) const
    {
        if (!IsVirtualPathSafe(virtualPath))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: refusing to read unsafe path '%s'", virtualPath.c_str());
            return {};
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto* mp : GetSortedMounts())
        {
            // First mount that HAS the file wins — see ReadFile above.
            if (mp->provider->Exists(virtualPath))
                return mp->provider->ReadTextFile(virtualPath);
        }
        return {};
    }

    std::vector<std::string> VirtualFileSystem::ListFiles(const std::string& directory,
                                                          const std::string& extension) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Merge results from all mounts, deduplicating by virtual path.
        // Higher-priority mounts are iterated first but all files are included.
        std::unordered_set<std::string> seen;
        std::vector<std::string> results;

        for (const auto* mp : GetSortedMounts())
        {
            auto files = mp->provider->ListFiles(directory, extension);
            for (auto& file : files)
            {
                if (seen.insert(file).second)
                {
                    results.push_back(std::move(file));
                }
            }
        }

        return results;
    }

    std::string VirtualFileSystem::ResolveProvider(const std::string& virtualPath) const
    {
        if (!IsVirtualPathSafe(virtualPath))
            return {};

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto* mp : GetSortedMounts())
        {
            if (mp->provider->Exists(virtualPath))
            {
                return mp->name;
            }
        }
        return {};
    }

    uint32_t VirtualFileSystem::GetMountCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32_t>(m_mounts.size());
    }

    std::string VirtualFileSystem::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::ostringstream ss;
        ss << "VirtualFileSystem: " << m_mounts.size() << " mount(s)\n";

        auto sorted = GetSortedMounts();
        for (const auto* mp : sorted)
        {
            ss << "  [" << mp->priority << "] " << mp->name << " -> " << mp->provider->GetProviderName() << "\n";
        }

        return ss.str();
    }

} // namespace Spark
