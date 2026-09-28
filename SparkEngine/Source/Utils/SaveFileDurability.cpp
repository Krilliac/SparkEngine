/**
 * @file SaveFileDurability.cpp
 * @brief Exclusive staging writes, atomic replace, atomic copy and whole-document writes.
 */

#include "SaveFileDurability.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::SaveFileDurability
{
    namespace
    {
        /// Unlink-and-retry rounds when something re-occupies the staging name between the
        /// unlink and the exclusive create. Past this the write fails closed.
        constexpr int kMaxStagingCreateAttempts = 3;

        /// Chunk size for CopyFileAtomically's streamed copy.
        constexpr std::size_t kCopyChunkBytes = 64 * 1024;

#if defined(_WIN32)
        std::error_code LastWindowsError()
        {
            return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        }
#else
        std::error_code LastPosixError()
        {
            return std::error_code(errno, std::generic_category());
        }
#endif

        /**
         * A staging file this process created itself: exclusive create, no link following,
         * and writes and the flush go through the one verified handle. Nothing is ever
         * reopened by name.
         */
        class ExclusiveStagingFile
        {
          public:
            ExclusiveStagingFile() = default;
            ExclusiveStagingFile(const ExclusiveStagingFile&) = delete;
            ExclusiveStagingFile& operator=(const ExclusiveStagingFile&) = delete;
            ~ExclusiveStagingFile() { CloseQuietly(); }

            /// Create @p path; fails when anything (file, link, directory) already has the name.
            bool TryCreate(const std::filesystem::path& path, std::error_code& error)
            {
#if defined(_WIN32)
                m_handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (m_handle == INVALID_HANDLE_VALUE)
                {
                    error = LastWindowsError();
                    return false;
                }
#else
                int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_NOFOLLOW)
                flags |= O_NOFOLLOW;
#endif
#if defined(O_CLOEXEC)
                flags |= O_CLOEXEC;
#endif
                // 0666 minus the umask: the same mode the previous std::ofstream staging produced.
                m_fd = ::open(path.c_str(), flags, 0666);
                if (m_fd < 0)
                {
                    error = LastPosixError();
                    return false;
                }
#endif
                return true;
            }

            /// The handle must name a fresh regular file with one link and no reparse point.
            bool VerifyFreshRegularFile(std::error_code& error) const
            {
#if defined(_WIN32)
                BY_HANDLE_FILE_INFORMATION info{};
                if (!::GetFileInformationByHandle(m_handle, &info))
                {
                    error = LastWindowsError();
                    return false;
                }
                const DWORD rejected = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY;
                if ((info.dwFileAttributes & rejected) != 0 || info.nNumberOfLinks != 1)
                {
                    error = std::make_error_code(std::errc::operation_not_permitted);
                    return false;
                }
#else
                struct stat info
                {
                };
                if (::fstat(m_fd, &info) != 0)
                {
                    error = LastPosixError();
                    return false;
                }
                if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
                {
                    error = std::make_error_code(std::errc::operation_not_permitted);
                    return false;
                }
#endif
                return true;
            }

            bool Write(const char* data, std::size_t size, std::error_code& error)
            {
                while (size > 0)
                {
#if defined(_WIN32)
                    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, std::size_t{1} << 30));
                    DWORD written = 0;
                    if (!::WriteFile(m_handle, data, request, &written, nullptr))
                    {
                        error = LastWindowsError();
                        return false;
                    }
                    if (written == 0)
                    {
                        error = std::make_error_code(std::errc::io_error);
                        return false;
                    }
#else
                    const std::size_t request =
                        std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
                    const ssize_t written = ::write(m_fd, data, request);
                    if (written < 0)
                    {
                        if (errno == EINTR)
                            continue;
                        error = LastPosixError();
                        return false;
                    }
                    if (written == 0)
                    {
                        error = std::make_error_code(std::errc::io_error);
                        return false;
                    }
#endif
                    data += written;
                    size -= static_cast<std::size_t>(written);
                }
                return true;
            }

            /// Flush to stable storage through the handle, then close it and report either failure.
            bool FlushAndClose(std::error_code& error)
            {
#if defined(_WIN32)
                const bool flushed = ::FlushFileBuffers(m_handle) != FALSE;
                const std::error_code flushError = flushed ? std::error_code{} : LastWindowsError();
                const bool closed = ::CloseHandle(m_handle) != FALSE;
                m_handle = INVALID_HANDLE_VALUE;
                if (!flushed)
                {
                    error = flushError;
                    return false;
                }
                if (!closed)
                {
                    error = LastWindowsError();
                    return false;
                }
#else
                const bool flushed = ::fsync(m_fd) == 0;
                const std::error_code flushError = flushed ? std::error_code{} : LastPosixError();
                const bool closed = ::close(m_fd) == 0;
                const std::error_code closeError = closed ? std::error_code{} : LastPosixError();
                m_fd = -1;
                if (!flushed)
                {
                    error = flushError;
                    return false;
                }
                if (!closed)
                {
                    error = closeError;
                    return false;
                }
#endif
                return true;
            }

            /// Close without flushing to stable storage, reporting a failed close. For snapshots
            /// that are atomic against a killed process but deliberately not durable.
            bool Close(std::error_code& error)
            {
#if defined(_WIN32)
                const bool closed = ::CloseHandle(m_handle) != FALSE;
                m_handle = INVALID_HANDLE_VALUE;
                if (!closed)
                    error = LastWindowsError();
#else
                const bool closed = ::close(m_fd) == 0;
                m_fd = -1;
                if (!closed)
                    error = LastPosixError();
#endif
                return closed;
            }

            /// Close without flushing. Callers close before unlinking a failed staging file:
            /// Windows cannot delete a file while this unshared handle is open.
            void CloseQuietly() noexcept
            {
#if defined(_WIN32)
                if (m_handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle(m_handle);
                m_handle = INVALID_HANDLE_VALUE;
#else
                if (m_fd >= 0)
                    ::close(m_fd);
                m_fd = -1;
#endif
            }

          private:
#if defined(_WIN32)
            HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
            int m_fd = -1;
#endif
        };

        /**
         * Create @p staging exclusively. An existing non-directory entry at the name is
         * unlinked (the entry itself, never a link target) and the exclusive create retried.
         */
        bool CreateStaging(ExclusiveStagingFile& file, const std::filesystem::path& staging, std::error_code& error)
        {
            for (int attempt = 0; attempt < kMaxStagingCreateAttempts; ++attempt)
            {
                std::error_code createError;
                if (file.TryCreate(staging, createError))
                {
                    if (file.VerifyFreshRegularFile(error))
                        return true;
                    file.CloseQuietly();
                    std::error_code removeError;
                    std::filesystem::remove(staging, removeError);
                    return false;
                }

                std::error_code statusError;
                const std::filesystem::file_status status = std::filesystem::symlink_status(staging, statusError);
                if (statusError || status.type() == std::filesystem::file_type::not_found)
                {
                    // Nothing occupies the name, so the create failed for another reason.
                    error = createError;
                    return false;
                }
                if (status.type() == std::filesystem::file_type::directory)
                {
                    error = std::make_error_code(std::errc::is_a_directory);
                    return false;
                }

                // A staging file a killed writer left behind, or a planted symlink / hard link.
                // std::filesystem::remove never follows a link: it removes the entry itself.
                std::error_code removeError;
                if (!std::filesystem::remove(staging, removeError) && removeError)
                {
                    error = removeError;
                    return false;
                }
            }
            error = std::make_error_code(std::errc::file_exists);
            return false;
        }

        void RemoveQuietly(const std::filesystem::path& path)
        {
            std::error_code removeError;
            std::filesystem::remove(path, removeError);
        }

        /// Publish @p staging over @p destination; true once committed (durable or not).
        bool Publish(const std::filesystem::path& staging, const std::filesystem::path& destination,
                     std::error_code& error)
        {
            switch (ReplaceFileAtomically(staging, destination, error))
            {
            case ReplaceOutcome::CommittedDurable:
                error.clear();
                return true;
            case ReplaceOutcome::CommittedNotDurable:
                // Committed: the staging name is gone and destination holds the new bytes.
                // error keeps the directory-sync failure for the caller to report.
                return true;
            case ReplaceOutcome::NotCommitted:
                break;
            }
            if (!error)
                error = std::make_error_code(std::errc::io_error);
            RemoveQuietly(staging);
            return false;
        }
    } // namespace

    bool WriteStagingFile(const std::filesystem::path& staging, std::string_view bytes, std::error_code& error)
    {
        ExclusiveStagingFile file;
        if (!CreateStaging(file, staging, error))
            return false;
        if (!file.Write(bytes.data(), bytes.size(), error))
        {
            file.CloseQuietly();
            RemoveQuietly(staging);
            return false;
        }
        // FlushAndClose closes the handle on failure too.
        if (!file.FlushAndClose(error))
        {
            RemoveQuietly(staging);
            return false;
        }
        return true;
    }

    ReplaceOutcome ReplaceFileAtomically(const std::filesystem::path& temporary,
                                         const std::filesystem::path& destination, std::error_code& error)
    {
#if defined(_WIN32)
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return ReplaceOutcome::CommittedDurable;
        }
        error = LastWindowsError();
        return ReplaceOutcome::NotCommitted;
#else
        // Open the directory before the rename. If it cannot be opened the replace is refused
        // uncommitted; opening it afterwards would let that failure land after the commit.
        const std::filesystem::path directory = destination.has_parent_path() ? destination.parent_path() : ".";
        int directoryFlags = O_RDONLY;
#if defined(O_DIRECTORY)
        directoryFlags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
        directoryFlags |= O_CLOEXEC;
#endif
        const int directoryFile = ::open(directory.c_str(), directoryFlags);
        if (directoryFile < 0)
        {
            error = LastPosixError();
            return ReplaceOutcome::NotCommitted;
        }

        std::filesystem::rename(temporary, destination, error);
        if (error)
        {
            ::close(directoryFile);
            return ReplaceOutcome::NotCommitted;
        }

        const bool flushed = ::fsync(directoryFile) == 0;
        const std::error_code flushError = flushed ? std::error_code{} : LastPosixError();
        ::close(directoryFile);
        if (!flushed)
        {
            error = flushError;
            return ReplaceOutcome::CommittedNotDurable;
        }
        return ReplaceOutcome::CommittedDurable;
#endif
    }

    bool CopyFileAtomically(const std::filesystem::path& source, const std::filesystem::path& destination,
                            std::error_code& error)
    {
        std::filesystem::path staging = destination;
        staging += ".tmp";

        std::ifstream input(source, std::ios::binary);
        if (!input.is_open())
        {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        {
            ExclusiveStagingFile file;
            if (!CreateStaging(file, staging, error))
                return false;

            std::vector<char> chunk(kCopyChunkBytes);
            while (input)
            {
                input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                const std::streamsize got = input.gcount();
                if (got > 0 && !file.Write(chunk.data(), static_cast<std::size_t>(got), error))
                {
                    file.CloseQuietly();
                    RemoveQuietly(staging);
                    return false;
                }
            }
            if (input.bad())
            {
                error = std::make_error_code(std::errc::io_error);
                file.CloseQuietly();
                RemoveQuietly(staging);
                return false;
            }
            if (!file.FlushAndClose(error))
            {
                RemoveQuietly(staging);
                return false;
            }
        }

        return Publish(staging, destination, error);
    }

    std::filesystem::path BackupPathFor(const std::filesystem::path& destination)
    {
        std::filesystem::path backup = destination;
        backup += ".bak";
        return backup;
    }

    bool WriteFileAtomically(const std::filesystem::path& destination, std::string_view bytes, bool retainBackup,
                             std::error_code& error)
    {
        error.clear();
        std::filesystem::path staging = destination;
        staging += ".tmp";

        if (!WriteStagingFile(staging, bytes, error))
            return false;

        if (retainBackup)
        {
            std::error_code existsError;
            if (std::filesystem::is_regular_file(destination, existsError))
            {
                std::error_code backupError;
                if (!CopyFileAtomically(destination, BackupPathFor(destination), backupError))
                {
                    error = backupError;
                    RemoveQuietly(staging);
                    return false;
                }
                // A committed-but-not-durable backup refresh still counts as retained.
            }
        }

        return Publish(staging, destination, error);
    }

    namespace
    {
        /// `<destination>.<64-bit random>.<process-wide counter>.tmp`. The random part makes the
        /// name unguessable in advance; the counter keeps concurrent publishers distinct.
        std::filesystem::path UniqueStagingPath(const std::filesystem::path& destination)
        {
            static std::atomic<uint64_t> s_counter{0};
            std::random_device device;
            const uint64_t salt = (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device()) ^
                                  static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            char suffix[64];
            std::snprintf(suffix, sizeof(suffix), ".%016llx.%llu.tmp", static_cast<unsigned long long>(salt),
                          static_cast<unsigned long long>(s_counter.fetch_add(1, std::memory_order_relaxed)));
            std::filesystem::path staging = destination;
            staging += suffix;
            return staging;
        }
    } // namespace

    bool PublishFileAtomically(const std::filesystem::path& destination, std::string_view bytes, std::error_code& error)
    {
        error.clear();
        const std::filesystem::path staging = UniqueStagingPath(destination);
        // CreateStaging creates the unpredictable name exclusively and never through a link. The staging
        // file is removed on every failure, and the destination is never deleted first, so a failed publish
        // keeps the previous complete snapshot.
        //
        // Nothing is fsynced: callers republish a status snapshot every few hundred milliseconds from a
        // server tick, where a file plus directory fsync stalls the tick for tens to hundreds of
        // milliseconds on a busy disk. The rename alone keeps the snapshot atomic against a killed process;
        // surviving a power loss has no value for a snapshot the next tick replaces.
        ExclusiveStagingFile file;
        if (!CreateStaging(file, staging, error))
        {
            if (!error)
                error = std::make_error_code(std::errc::io_error);
            return false;
        }
        if (!file.Write(bytes.data(), bytes.size(), error))
        {
            file.CloseQuietly();
            RemoveQuietly(staging);
            return false;
        }
        if (!file.Close(error))
        {
            RemoveQuietly(staging);
            return false;
        }
        std::filesystem::rename(staging, destination, error);
        if (error)
        {
            RemoveQuietly(staging);
            return false;
        }
        return true;
    }
} // namespace Spark::SaveFileDurability
