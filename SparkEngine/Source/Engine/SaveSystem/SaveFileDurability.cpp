/**
 * @file SaveFileDurability.cpp
 * @brief Durable flush, atomic replace and atomic copy for the SaveSystem write path.
 */

#include "SaveFileDurability.h"

#include <cerrno>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Spark::SaveFileDurability
{
    bool FlushFileDurably(const std::filesystem::path& path, std::error_code& error)
    {
#if defined(_WIN32)
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
        }

        const bool flushed = ::FlushFileBuffers(file) != FALSE;
        const DWORD flushError = flushed ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(file);
        if (!flushed)
        {
            error = std::error_code(static_cast<int>(flushError), std::system_category());
            return false;
        }
        return true;
#else
        const int file = ::open(path.c_str(), O_RDONLY);
        if (file < 0)
        {
            error = std::error_code(errno, std::generic_category());
            return false;
        }

        const bool flushed = ::fsync(file) == 0;
        const int flushError = flushed ? 0 : errno;
        ::close(file);
        if (!flushed)
        {
            error = std::error_code(flushError, std::generic_category());
            return false;
        }
        return true;
#endif
    }

    bool ReplaceFileAtomically(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                               std::error_code& error)
    {
#if defined(_WIN32)
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return true;
        }
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
#else
        std::filesystem::rename(temporary, destination, error);
        if (error)
            return false;

        const std::filesystem::path directory = destination.has_parent_path() ? destination.parent_path() : ".";
#if defined(O_DIRECTORY)
        const int directoryFile = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
        const int directoryFile = ::open(directory.c_str(), O_RDONLY);
#endif
        if (directoryFile < 0)
        {
            error = std::error_code(errno, std::generic_category());
            return false;
        }
        const bool flushed = ::fsync(directoryFile) == 0;
        const int flushError = flushed ? 0 : errno;
        ::close(directoryFile);
        if (!flushed)
        {
            error = std::error_code(flushError, std::generic_category());
            return false;
        }
        return true;
#endif
    }

    bool CopyFileAtomically(const std::filesystem::path& source, const std::filesystem::path& destination,
                            std::error_code& error)
    {
        std::filesystem::path staging = destination;
        staging += ".tmp";

        // overwrite_existing also replaces a staging file a killed writer left behind.
        const bool copied =
            std::filesystem::copy_file(source, staging, std::filesystem::copy_options::overwrite_existing, error);
        if (!copied || error || !FlushFileDurably(staging, error) ||
            !ReplaceFileAtomically(staging, destination, error))
        {
            if (!error)
                error = std::make_error_code(std::errc::io_error);
            std::error_code removeError;
            std::filesystem::remove(staging, removeError);
            return false;
        }
        return true;
    }
} // namespace Spark::SaveFileDurability
