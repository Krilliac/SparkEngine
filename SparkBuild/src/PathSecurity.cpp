#include "PathSecurity.h"

#include <cstdlib>
#include <system_error>
#include <utility>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace SparkBuild::PathSecurity
{
    namespace
    {
#ifdef SPARK_PLATFORM_WINDOWS
        constexpr char kPathListSeparator = ';';
#else
        constexpr char kPathListSeparator = ':';
#endif

        std::vector<std::string> SplitPathList(const std::string& list)
        {
            std::vector<std::string> entries;
            size_t start = 0;
            while (start <= list.size())
            {
                size_t end = list.find(kPathListSeparator, start);
                if (end == std::string::npos)
                {
                    end = list.size();
                }
                std::string entry = list.substr(start, end - start);
#ifdef SPARK_PLATFORM_WINDOWS
                // cmd.exe tolerates quoted PATH entries such as "C:\Program Files\Git\cmd".
                if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"')
                {
                    entry = entry.substr(1, entry.size() - 2);
                }
#endif
                entries.push_back(std::move(entry));
                start = end + 1;
            }
            return entries;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        using DirectoryQuery = UINT(WINAPI*)(LPWSTR, UINT);

        void AppendSystemDirectory(DirectoryQuery query, std::vector<fs::path>& directories)
        {
            wchar_t buffer[MAX_PATH] = {};
            const UINT length = query(buffer, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                directories.emplace_back(std::wstring(buffer, length));
            }
        }

        bool IsQualifyingExecutable(const fs::path& candidate)
        {
            std::error_code error;
            return fs::is_regular_file(candidate, error) && !error;
        }

        bool QueryIdentity(HANDLE handle, uint32_t& volume, uint64_t& fileIndex, DWORD& attributes)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(handle, &info))
            {
                return false;
            }
            volume = info.dwVolumeSerialNumber;
            fileIndex = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            attributes = info.dwFileAttributes;
            return true;
        }
#else
        bool IsQualifyingExecutable(const fs::path& candidate)
        {
            struct stat info
            {
            };
            return ::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                   ::access(candidate.c_str(), X_OK) == 0;
        }
#endif
    } // namespace

    bool IsBareProgramName(const std::string& program)
    {
        if (program.empty() || program == "." || program == "..")
        {
            return false;
        }
#ifdef SPARK_PLATFORM_WINDOWS
        return program.find_first_of("/\\:") == std::string::npos;
#else
        return program.find('/') == std::string::npos;
#endif
    }

    std::string ResolveExecutableIn(const std::string& program, const std::string& searchPath)
    {
        if (!IsBareProgramName(program))
        {
            return {};
        }

        const std::vector<std::string> entries = SplitPathList(searchPath);
        std::vector<fs::path> directories;
        directories.reserve(entries.size() + 2);
#ifdef SPARK_PLATFORM_WINDOWS
        // The system directories come first, as in the native search order;
        // only the application and current directories are dropped from it.
        AppendSystemDirectory(&::GetSystemDirectoryW, directories);
        AppendSystemDirectory(&::GetWindowsDirectoryW, directories);
        const fs::path fileName(fs::path(program).has_extension() ? program : program + ".exe");
#else
        const fs::path fileName(program);
#endif
        for (const std::string& entry : entries)
        {
            // Empty entries, ".", "bin", "C:tools" and "\tools" all depend on
            // the current directory or drive, so only absolute entries count.
            const fs::path directory(entry);
            if (!entry.empty() && directory.is_absolute())
            {
                directories.push_back(directory);
            }
        }

        for (const fs::path& directory : directories)
        {
            const fs::path candidate = directory / fileName;
            if (IsQualifyingExecutable(candidate))
            {
                return candidate.lexically_normal().string();
            }
        }
        return {};
    }

    std::string ResolveExecutable(const std::string& program)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        std::string searchPath;
        const DWORD required = ::GetEnvironmentVariableA("PATH", nullptr, 0);
        if (required > 0)
        {
            searchPath.resize(required);
            const DWORD written = ::GetEnvironmentVariableA("PATH", searchPath.data(), required);
            searchPath.resize(written < required ? written : 0);
        }
        return ResolveExecutableIn(program, searchPath);
#else
        const char* path = std::getenv("PATH");
        return ResolveExecutableIn(program, path != nullptr ? std::string(path) : std::string("/usr/bin:/bin"));
#endif
    }

    PathPin::~PathPin()
    {
        Close();
    }

    bool PathPin::IsOpen() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return m_handle != nullptr;
#else
        return m_fd >= 0;
#endif
    }

    void PathPin::Close()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_handle != nullptr)
        {
            ::CloseHandle(static_cast<HANDLE>(m_handle));
        }
        m_handle = nullptr;
#else
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
        m_fd = -1;
#endif
        m_path.clear();
    }

#ifdef SPARK_PLATFORM_WINDOWS
    bool PathPin::Open(const fs::path& path, Kind kind, std::string& error)
    {
        Close();
        const bool directory = kind == Kind::Directory;
        // FILE_READ_DATA (FILE_LIST_DIRECTORY for a directory) is required for
        // the share mode to be enforced: an attribute-only open records none.
        const DWORD access = directory ? (FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE) : GENERIC_READ;
        // No FILE_SHARE_DELETE: the entry cannot be renamed, deleted or replaced
        // while pinned. A file additionally refuses writers; a directory must
        // still accept the extractor creating entries inside it.
        const DWORD share = directory ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
        const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
        const HANDLE handle =
            ::CreateFileW(path.wstring().c_str(), access, share, nullptr, OPEN_EXISTING, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "could not pin '" + path.string() + "' (Windows error " + std::to_string(::GetLastError()) + ")";
            return false;
        }

        uint32_t volume = 0;
        uint64_t fileIndex = 0;
        DWORD attributes = 0;
        const bool identified = QueryIdentity(handle, volume, fileIndex, attributes);
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!identified || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || isDirectory != directory)
        {
            ::CloseHandle(handle);
            error = "'" + path.string() + "' is a link, a reparse point, or not a " +
                    (directory ? "directory" : "regular file");
            return false;
        }
        m_handle = handle;
        m_volume = volume;
        m_fileIndex = fileIndex;
        m_path = path;
        return true;
    }

    bool PathPin::StillNames(const fs::path& path) const
    {
        if (m_handle == nullptr)
        {
            return false;
        }
        // Attribute-only access is never blocked by the pin's own share mode.
        const HANDLE handle = ::CreateFileW(
            path.wstring().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        uint32_t volume = 0;
        uint64_t fileIndex = 0;
        DWORD attributes = 0;
        const bool identified = QueryIdentity(handle, volume, fileIndex, attributes);
        ::CloseHandle(handle);
        return identified && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && volume == m_volume &&
               fileIndex == m_fileIndex;
    }

    fs::path PathPin::BoundPath() const
    {
        return m_path;
    }
#else
    bool PathPin::Open(const fs::path& path, Kind kind, std::string& error)
    {
        Close();
        const bool directory = kind == Kind::Directory;
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0);
        const int fd = ::open(path.c_str(), flags);
        if (fd < 0)
        {
            error = "could not pin '" + path.string() + "' (errno " + std::to_string(errno) + ")";
            return false;
        }

        struct stat info
        {
        };
        const bool typeMatches = ::fstat(fd, &info) == 0 && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
        if (!typeMatches)
        {
            ::close(fd);
            error = "'" + path.string() + "' is not a " + (directory ? "directory" : "regular file");
            return false;
        }
        m_fd = fd;
        m_device = static_cast<uint64_t>(info.st_dev);
        m_inode = static_cast<uint64_t>(info.st_ino);
        m_path = path;
        return true;
    }

    bool PathPin::StillNames(const fs::path& path) const
    {
        if (m_fd < 0)
        {
            return false;
        }
        struct stat info
        {
        };
        if (::lstat(path.c_str(), &info) != 0 || S_ISLNK(info.st_mode))
        {
            return false;
        }
        return static_cast<uint64_t>(info.st_dev) == m_device && static_cast<uint64_t>(info.st_ino) == m_inode;
    }

    fs::path PathPin::BoundPath() const
    {
        if (m_fd < 0)
        {
            return {};
        }
        // macOS /dev/fd/N duplicates the descriptor (shared offset); Linux reopens it.
        (void)::lseek(m_fd, 0, SEEK_SET);
        return fs::path("/dev/fd") / std::to_string(m_fd);
    }
#endif
} // namespace SparkBuild::PathSecurity
