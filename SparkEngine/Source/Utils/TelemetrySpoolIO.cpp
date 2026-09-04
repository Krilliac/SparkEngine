/**
 * @file TelemetrySpoolIO.cpp
 * @brief Filesystem safety checks and platform file I/O for the telemetry spool.
 */

#include "Utils/TelemetrySpoolInternal.h"

#include <limits>
#include <system_error>

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

namespace Spark::TelemetryDetail
{
    namespace
    {
        bool HasReparseAttribute(const std::filesystem::path& path)
        {
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            (void)path;
            return false;
#endif
        }

        bool IsSafeRegularFile(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status) ||
                HasReparseAttribute(path))
            {
                return false;
            }
            const auto links = std::filesystem::hard_link_count(path, error);
            return !error && links == 1;
        }
    } // namespace

    bool IsSafeDirectoryPath(const std::filesystem::path& path)
    {
        if (path.empty() || !path.is_absolute())
            return false;

        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
            HasReparseAttribute(path))
        {
            return false;
        }

        std::filesystem::path current = path.root_path();
        for (const auto& component : path.relative_path())
        {
            current /= component;
            const auto componentStatus = std::filesystem::symlink_status(current, error);
            if (error || !std::filesystem::is_directory(componentStatus) ||
                std::filesystem::is_symlink(componentStatus) || HasReparseAttribute(current))
            {
                return false;
            }
        }
        return true;
    }

    bool PathExists(const std::filesystem::path& path, bool& exists)
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (!error)
        {
            exists = std::filesystem::exists(status);
            return true;
        }
        if (error == std::errc::no_such_file_or_directory)
        {
            exists = false;
            return true;
        }
        exists = false;
        return false;
    }

    TelemetrySpoolResult RemoveOwnedFile(const std::filesystem::path& path)
    {
        bool exists = false;
        if (!PathExists(path, exists))
            return TelemetrySpoolResult::IoFailure;
        if (!exists)
            return TelemetrySpoolResult::Success;
        if (!IsSafeRegularFile(path))
            return TelemetrySpoolResult::Rejected;

        std::error_code error;
        if (!std::filesystem::remove(path, error) || error)
            return TelemetrySpoolResult::IoFailure;
        return TelemetrySpoolResult::Success;
    }

    TelemetrySpoolResult ReadOwnedFile(const std::filesystem::path& path, uint64_t maximumBytes,
                                       std::vector<uint8_t>& bytes)
    {
        if (!IsSafeRegularFile(path))
            return TelemetrySpoolResult::Rejected;

#ifdef _WIN32
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return TelemetrySpoolResult::IoFailure;

        BY_HANDLE_FILE_INFORMATION information{};
        LARGE_INTEGER size{};
        const bool safe =
            GetFileInformationByHandle(file, &information) != FALSE &&
            (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
            information.nNumberOfLinks == 1 && GetFileSizeEx(file, &size) != FALSE &&
            size.QuadPart >= static_cast<LONGLONG>(kHeaderBytes) &&
            static_cast<uint64_t>(size.QuadPart) <= maximumBytes;
        if (!safe)
        {
            CloseHandle(file);
            return TelemetrySpoolResult::Rejected;
        }

        if (!TryResize(bytes, static_cast<size_t>(size.QuadPart)))
        {
            CloseHandle(file);
            return TelemetrySpoolResult::IoFailure;
        }
        size_t offset = 0;
        while (offset < bytes.size())
        {
            DWORD read = 0;
            const DWORD request = static_cast<DWORD>(
                (std::min)(bytes.size() - offset, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) || read == 0)
            {
                CloseHandle(file);
                return TelemetrySpoolResult::IoFailure;
            }
            offset += read;
        }
        uint8_t extra = 0;
        DWORD extraRead = 0;
        const bool readSucceeded = ReadFile(file, &extra, 1, &extraRead, nullptr) != FALSE;
        CloseHandle(file);
        if (!readSucceeded)
            return TelemetrySpoolResult::IoFailure;
        return extraRead == 0 ? TelemetrySpoolResult::Success : TelemetrySpoolResult::Rejected;
#else
        const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (file < 0)
            return errno == ELOOP ? TelemetrySpoolResult::Rejected : TelemetrySpoolResult::IoFailure;

        struct stat information
        {
        };
        if (fstat(file, &information) != 0 || !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
            information.st_size < static_cast<off_t>(kHeaderBytes) ||
            static_cast<uint64_t>(information.st_size) > maximumBytes)
        {
            close(file);
            return TelemetrySpoolResult::Rejected;
        }

        if (!TryResize(bytes, static_cast<size_t>(information.st_size)))
        {
            close(file);
            return TelemetrySpoolResult::IoFailure;
        }
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const ssize_t readCount = read(file, bytes.data() + offset, bytes.size() - offset);
            if (readCount < 0 && errno == EINTR)
                continue;
            if (readCount <= 0)
            {
                close(file);
                return TelemetrySpoolResult::IoFailure;
            }
            offset += static_cast<size_t>(readCount);
        }
        uint8_t extra = 0;
        ssize_t extraRead = -1;
        do
        {
            extraRead = read(file, &extra, 1);
        } while (extraRead < 0 && errno == EINTR);
        close(file);
        if (extraRead < 0)
            return TelemetrySpoolResult::IoFailure;
        return extraRead == 0 ? TelemetrySpoolResult::Success : TelemetrySpoolResult::Rejected;
#endif
    }

    TelemetrySpoolResult WriteOwnedFileAtomically(const std::filesystem::path& directory,
                                                  const std::filesystem::path& artifact,
                                                  const std::filesystem::path& staging,
                                                  const std::vector<uint8_t>& bytes)
    {
        const auto staleResult = RemoveOwnedFile(staging);
        if (staleResult != TelemetrySpoolResult::Success)
            return staleResult;

        bool artifactExists = false;
        if (!PathExists(artifact, artifactExists))
            return TelemetrySpoolResult::IoFailure;
        if (artifactExists && !IsSafeRegularFile(artifact))
            return TelemetrySpoolResult::Rejected;

#ifdef _WIN32
        HANDLE file =
            CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return TelemetrySpoolResult::IoFailure;

        size_t offset = 0;
        bool written = true;
        while (offset < bytes.size())
        {
            DWORD count = 0;
            const DWORD request = static_cast<DWORD>(
                (std::min)(bytes.size() - offset, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (!WriteFile(file, bytes.data() + offset, request, &count, nullptr) || count == 0)
            {
                written = false;
                break;
            }
            offset += count;
        }
        written = written && FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        if (!written)
        {
            DeleteFileW(staging.c_str());
            return TelemetrySpoolResult::IoFailure;
        }

        if (!MoveFileExW(staging.c_str(), artifact.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(staging.c_str());
            return TelemetrySpoolResult::IoFailure;
        }
#else
        const int file = open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file < 0)
            return errno == ELOOP ? TelemetrySpoolResult::Rejected : TelemetrySpoolResult::IoFailure;

        size_t offset = 0;
        bool written = true;
        while (offset < bytes.size())
        {
            const ssize_t count = write(file, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
            {
                written = false;
                break;
            }
            offset += static_cast<size_t>(count);
        }
        written = written && fsync(file) == 0;
        close(file);
        if (!written)
        {
            unlink(staging.c_str());
            return TelemetrySpoolResult::IoFailure;
        }
        if (rename(staging.c_str(), artifact.c_str()) != 0)
        {
            unlink(staging.c_str());
            return TelemetrySpoolResult::IoFailure;
        }

        const int directoryHandle = open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (directoryHandle < 0 || fsync(directoryHandle) != 0)
        {
            if (directoryHandle >= 0)
                close(directoryHandle);
            return TelemetrySpoolResult::IoFailure;
        }
        close(directoryHandle);
#endif

        return IsSafeRegularFile(artifact) ? TelemetrySpoolResult::Success : TelemetrySpoolResult::Rejected;
    }

} // namespace Spark::TelemetryDetail
