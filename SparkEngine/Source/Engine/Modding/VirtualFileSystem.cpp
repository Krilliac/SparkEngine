/**
 * @file VirtualFileSystem.cpp
 * @brief Implementation of the mount-priority virtual filesystem
 */

#include "VirtualFileSystem.h"
#include "HeldHandles.h"
#include "../../Utils/FileUtils.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

        // Control bytes are never part of a legitimate asset name. An embedded NUL
        // is the dangerous one: every c_str() consumer silently truncates the path
        // at it, so the file opened is not the one that was validated.
        for (const char c : virtualPath)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return false;
        }

        // Decide with Windows separator semantics on every host. On POSIX a
        // backslash is an ordinary filename byte, so "..\..\x" would otherwise
        // normalize to one harmless component here while escaping the root when
        // the same content is loaded on Windows, the primary platform.
        std::string generic = virtualPath;
        std::replace(generic.begin(), generic.end(), '\\', '/');

        const fs::path candidate = fs::path(generic).lexically_normal();
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

    void LocalFileProvider::SetOpenProbeForTesting(std::function<void(const std::string&)> probe)
    {
        m_openProbe = std::move(probe);
    }

    // ResolvePath's lexical and link-resolved checks name a path; whatever sits at that name
    // when the open happens is what would be read. So the file is opened first and every
    // decision is made on the opened handle: it must be a regular file (a directory used to
    // size the buffer from ext4's INT64_MAX directory offset), it must resolve inside the
    // mount root (a link swapped in after ResolvePath escaped it), and exactly its size is
    // read, so a file that shrinks or grows during the read is refused.
#ifdef _WIN32
    bool LocalFileProvider::ReadVerified(const std::string& virtualPath, std::vector<uint8_t>& bytes) const
    {
        const std::string fullPath = ResolvePath(virtualPath);
        if (fullPath.empty())
        {
            return false;
        }
        if (m_openProbe)
        {
            m_openProbe(fullPath);
        }

        const HeldHandles::ScopedHandle file(::CreateFileW(fs::path(fullPath).c_str(), GENERIC_READ,
                                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.IsValid())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Failed to open file '%s'", fullPath.c_str());
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileType(file.Get()) != FILE_TYPE_DISK || !::GetFileInformationByHandle(file.Get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: '%s' is not a regular file", fullPath.c_str());
            return false;
        }
        const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
        if (size > kMaxFileBytes)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: '%s' is larger than the read limit", fullPath.c_str());
            return false;
        }

        const HeldHandles::ScopedHandle root = HeldHandles::OpenPinnedDirectory(m_root, /*allowReparse=*/true);
        const std::wstring rootFinal = root.IsValid() ? HeldHandles::FinalPathOf(root.Get()) : std::wstring();
        if (rootFinal.empty() || !HeldHandles::IsDescendantPath(HeldHandles::FinalPathOf(file.Get()), rootFinal))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: rejected '%s': the opened file is outside the mount root",
                           virtualPath.c_str());
            return false;
        }

        bytes.resize(static_cast<size_t>(size));
        size_t total = 0;
        while (total < bytes.size())
        {
            DWORD got = 0;
            const DWORD request = static_cast<DWORD>(std::min<size_t>(bytes.size() - total, 1U << 30));
            if (!::ReadFile(file.Get(), bytes.data() + total, request, &got, nullptr) || got == 0)
            {
                break;
            }
            total += got;
        }
        uint8_t extra = 0;
        DWORD extraRead = 0;
        const bool atEnd = ::ReadFile(file.Get(), &extra, 1, &extraRead, nullptr) && extraRead == 0;
        if (total != bytes.size() || !atEnd)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: '%s' changed size while it was read", fullPath.c_str());
            bytes.clear();
            return false;
        }
        return true;
    }
#else
    namespace
    {
        /// Path the kernel reports for an open descriptor, or empty when it cannot say.
        fs::path PathOfDescriptor(int fd)
        {
#if defined(__APPLE__)
            char buffer[PATH_MAX] = {};
            if (::fcntl(fd, F_GETPATH, buffer) == -1)
            {
                return {};
            }
            return fs::path(buffer);
#else
            std::error_code ec;
            fs::path path = fs::read_symlink("/proc/self/fd/" + std::to_string(fd), ec);
            return ec ? fs::path() : path;
#endif
        }
    } // namespace

    bool LocalFileProvider::ReadVerified(const std::string& virtualPath, std::vector<uint8_t>& bytes) const
    {
        const std::string fullPath = ResolvePath(virtualPath);
        if (fullPath.empty())
        {
            return false;
        }
        if (m_openProbe)
        {
            m_openProbe(fullPath);
        }

        // O_NONBLOCK keeps a FIFO planted at the name from blocking the open; the fstat below
        // refuses it, a directory, a device and every other non-regular file.
        const HeldHandles::ScopedFd file(::open(fullPath.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
        if (file.Get() < 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Failed to open file '%s'", fullPath.c_str());
            return false;
        }
        struct stat info = {};
        if (::fstat(file.Get(), &info) != 0 || !S_ISREG(info.st_mode))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: '%s' is not a regular file", fullPath.c_str());
            return false;
        }
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > kMaxFileBytes)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: '%s' is larger than the read limit", fullPath.c_str());
            return false;
        }

        // The opened object must live inside the (link-resolved) mount root, and that path
        // must still name the very file that was opened.
        std::error_code rootError;
        const fs::path root = fs::canonical(m_root, rootError);
        const fs::path opened = PathOfDescriptor(file.Get());
        struct stat named = {};
        if (rootError || opened.empty() || !IsContainedIn(opened.lexically_normal(), root) ||
            ::stat(opened.c_str(), &named) != 0 || named.st_dev != info.st_dev || named.st_ino != info.st_ino)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: rejected '%s': the opened file is outside the mount root",
                           virtualPath.c_str());
            return false;
        }

        bytes.resize(static_cast<size_t>(info.st_size));
        size_t total = 0;
        while (total < bytes.size())
        {
            const ssize_t got = ::read(file.Get(), bytes.data() + total, bytes.size() - total);
            if (got < 0 && errno == EINTR)
            {
                continue;
            }
            if (got <= 0)
            {
                break;
            }
            total += static_cast<size_t>(got);
        }
        uint8_t extra = 0;
        ssize_t extraRead = 0;
        do
        {
            extraRead = ::read(file.Get(), &extra, 1);
        } while (extraRead < 0 && errno == EINTR);
        if (total != bytes.size() || extraRead != 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: '%s' changed size while it was read", fullPath.c_str());
            bytes.clear();
            return false;
        }
        return true;
    }
#endif

    std::vector<uint8_t> LocalFileProvider::ReadFile(const std::string& virtualPath) const
    {
        std::vector<uint8_t> bytes;
        if (!ReadVerified(virtualPath, bytes))
        {
            return {};
        }
        return bytes;
    }

    std::string LocalFileProvider::ReadTextFile(const std::string& virtualPath) const
    {
        std::vector<uint8_t> bytes;
        if (!ReadVerified(virtualPath, bytes))
        {
            return {};
        }
        std::string text(bytes.begin(), bytes.end());
#ifdef _WIN32
        // This used to read through a text-mode ifstream, which turns CRLF into LF on Windows.
        std::string normalized;
        normalized.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            {
                continue;
            }
            normalized.push_back(text[i]);
        }
        text = std::move(normalized);
#endif
        return text;
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

            if (!extension.empty() && entry.path().extension() != fs::path(extension))
            {
                continue;
            }

            // A listed virtual path is fed back to ResolvePath(), which rebuilds the
            // path from its narrow spelling. A name the Windows ANSI code page cannot
            // spell has none (path::string() throws std::system_error, which used to
            // abort the listing), and no virtual path could reopen it: leave it out.
            const std::optional<std::string> narrowName = FileUtils::TryPathToNarrow(entry.path().filename());
            if (!narrowName)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "VFS: not listing '%s': its name has no spelling in the active code page",
                               FileUtils::TryPathToUtf8(entry.path().filename()).value_or("?").c_str());
                continue;
            }
            const std::string& filename = *narrowName;

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
