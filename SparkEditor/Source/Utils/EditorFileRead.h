/**
 * @file EditorFileRead.h
 * @brief Whole-file reads of editor documents through one opened, size-checked handle
 *
 * A by-path check (exists, regular file, size) followed by an open by name is a race:
 * whatever sits at the name when the open happens is what gets read, and a file that grew
 * or a pseudo-file whose reported size is 0 was read whole, past the limit the check
 * enforced. ReadRegularFileBounded opens first and decides on the opened handle: it must be
 * a regular file no larger than the limit, exactly that many bytes are read, and the next
 * read must hit end of file, so a file that changes size during the read is refused.
 *
 * Thread affinity: any thread; no shared state.
 * Ownership: the handle is owned by the call and closed before it returns.
 * Allocation: @p contents grows to the file's size, which never exceeds @p maxBytes.
 */

#pragma once

#include "Engine/Modding/HeldHandles.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace SparkEditor
{

    /// @brief Outcome of ReadRegularFileBounded.
    enum class BoundedReadStatus : std::uint8_t
    {
        Ok,             ///< contents holds the whole file
        Missing,        ///< nothing exists at the path
        NotRegularFile, ///< a directory, FIFO, device, or other non-regular object
        TooLarge,       ///< the opened file is larger than the limit; nothing was read
        Failed          ///< the open or a read failed, or the size changed while reading
    };

    /**
     * @brief Read the whole regular file at @p path, deciding everything on the opened handle.
     * @param path File to read.
     * @param maxBytes Largest size accepted; a larger file is refused before any byte is read.
     * @param contents Receives the bytes on success; cleared otherwise.
     */
    inline BoundedReadStatus ReadRegularFileBounded(const std::filesystem::path& path, std::uint64_t maxBytes,
                                                    std::string& contents)
    {
        contents.clear();
#ifdef _WIN32
        const Spark::HeldHandles::ScopedHandle file(::CreateFileW(path.c_str(), GENERIC_READ,
                                                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.IsValid())
        {
            const DWORD error = ::GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? BoundedReadStatus::Missing
                                                                                  : BoundedReadStatus::Failed;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileType(file.Get()) != FILE_TYPE_DISK || !::GetFileInformationByHandle(file.Get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return BoundedReadStatus::NotRegularFile;
        }
        const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
        if (size > maxBytes)
        {
            return BoundedReadStatus::TooLarge;
        }
        contents.resize(static_cast<std::size_t>(size));
        std::size_t total = 0;
        while (total < contents.size())
        {
            DWORD got = 0;
            const auto request = static_cast<DWORD>(std::min<std::size_t>(contents.size() - total, 1U << 30));
            if (!::ReadFile(file.Get(), contents.data() + total, request, &got, nullptr) || got == 0)
            {
                break;
            }
            total += got;
        }
        char extra = 0;
        DWORD extraRead = 0;
        const bool atEnd = ::ReadFile(file.Get(), &extra, 1, &extraRead, nullptr) && extraRead == 0;
#else
        // O_NONBLOCK keeps a FIFO planted at the name from blocking the open; the fstat below
        // refuses it, a directory, a device and every other non-regular file.
        const Spark::HeldHandles::ScopedFd file(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
        if (file.Get() < 0)
        {
            return errno == ENOENT || errno == ENOTDIR ? BoundedReadStatus::Missing : BoundedReadStatus::Failed;
        }
        struct stat info = {};
        if (::fstat(file.Get(), &info) != 0 || !S_ISREG(info.st_mode))
        {
            return BoundedReadStatus::NotRegularFile;
        }
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > maxBytes)
        {
            return BoundedReadStatus::TooLarge;
        }
        contents.resize(static_cast<std::size_t>(info.st_size));
        std::size_t total = 0;
        while (total < contents.size())
        {
            const ssize_t got = ::read(file.Get(), contents.data() + total, contents.size() - total);
            if (got < 0 && errno == EINTR)
            {
                continue;
            }
            if (got <= 0)
            {
                break;
            }
            total += static_cast<std::size_t>(got);
        }
        char extra = 0;
        ssize_t extraRead = 0;
        do
        {
            extraRead = ::read(file.Get(), &extra, 1);
        } while (extraRead < 0 && errno == EINTR);
        const bool atEnd = extraRead == 0;
#endif
        if (total != contents.size() || !atEnd)
        {
            contents.clear();
            return BoundedReadStatus::Failed;
        }
        return BoundedReadStatus::Ok;
    }

} // namespace SparkEditor
