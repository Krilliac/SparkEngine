/**
 * @file TFSavePaths.h
 * @brief One process-stable root for every TERRAFRONT persistence file.
 *
 * The root is resolved once on first use. `TF_SAVE_ROOT` may name an absolute
 * directory (recommended for dedicated servers) or a directory relative to
 * the process working directory. When it is unset, all stores use
 * `<working-directory>/Saves`.
 */
#pragma once

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Terrafront::SavePaths
{
    /**
     * Process-wide exclusive ownership guard for one persistence file.
     *
     * Ownership is the OS handle/lock on a small persistent `<target>.lock`
     * file, so a crashed process releases it automatically without stale-lock
     * cleanup. Locks conflict between processes and between two guards in the
     * same process. Stores use it transaction-scoped (TFDatabase: lock,
     * reload, validate, apply, atomic write, unlock) so several authority
     * processes can share one TF_SAVE_ROOT, or lifetime-scoped where a store
     * still assumes a single authority (TFOutfitStore, TFSocialSystem).
     */
    class ExclusiveFileLock
    {
      public:
        ExclusiveFileLock() = default;
        ExclusiveFileLock(const ExclusiveFileLock&) = delete;
        ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
        ExclusiveFileLock(ExclusiveFileLock&&) = delete;
        ExclusiveFileLock& operator=(ExclusiveFileLock&&) = delete;
        ~ExclusiveFileLock() { Unlock(); }

        bool TryLock(const std::filesystem::path& target, std::error_code& ec) noexcept
        {
            if (IsLocked())
            {
                ec = std::make_error_code(std::errc::device_or_resource_busy);
                return false;
            }
            if (target.empty())
            {
                ec = std::make_error_code(std::errc::invalid_argument);
                return false;
            }

            m_lockPath = target;
            m_lockPath += ".lock";
#ifdef _WIN32
            m_handle = ::CreateFileW(m_lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle == INVALID_HANDLE_VALUE)
            {
                ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
                return false;
            }
#else
            m_fd = ::open(m_lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (m_fd < 0)
            {
                ec = std::error_code(errno, std::generic_category());
                return false;
            }
            if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0)
            {
                ec = std::error_code(errno, std::generic_category());
                ::close(m_fd);
                m_fd = -1;
                return false;
            }
#endif
            ec.clear();
            return true;
        }

        /**
         * Acquire the lock, waiting up to `timeout` while another owner holds
         * it. Errors other than contention fail immediately. On timeout `ec`
         * carries the contention error from the last attempt.
         */
        bool Lock(const std::filesystem::path& target, std::chrono::milliseconds timeout, std::error_code& ec) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            auto backoff = std::chrono::milliseconds(1);
            for (;;)
            {
                if (TryLock(target, ec))
                    return true;
                if (!IsContention(ec) || std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, std::chrono::milliseconds(16));
            }
        }

        void Unlock() noexcept
        {
#ifdef _WIN32
            if (m_handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_handle);
                m_handle = INVALID_HANDLE_VALUE;
            }
#else
            if (m_fd >= 0)
            {
                (void)::flock(m_fd, LOCK_UN);
                (void)::close(m_fd);
                m_fd = -1;
            }
#endif
            m_lockPath.clear();
        }

        bool IsLocked() const noexcept
        {
#ifdef _WIN32
            return m_handle != INVALID_HANDLE_VALUE;
#else
            return m_fd >= 0;
#endif
        }

      private:
        static bool IsContention(const std::error_code& ec) noexcept
        {
#ifdef _WIN32
            return ec.category() == std::system_category() &&
                   (ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_LOCK_VIOLATION);
#else
            return ec.category() == std::generic_category() && (ec.value() == EWOULDBLOCK || ec.value() == EAGAIN);
#endif
        }

        std::filesystem::path m_lockPath;
#ifdef _WIN32
        HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
        int m_fd = -1;
#endif
    };

    /** Resolve a configured root against a known working directory. */
    inline std::filesystem::path ResolveRootWide(std::wstring_view configuredRoot,
                                                 const std::filesystem::path& workingDirectory)
    {
        if (configuredRoot.empty())
            return workingDirectory.empty() ? std::filesystem::path{} : (workingDirectory / "Saves").lexically_normal();

        // MinGW's std::filesystem classifies a backslash-form UNC path as
        // relative even on Windows. Preserve UNC/device roots rather than
        // accidentally placing them underneath the process working directory.
        const auto isSeparator = [](wchar_t value) { return value == L'\\' || value == L'/'; };
        const bool hasNetworkOrDeviceRoot =
            configuredRoot.size() >= 2 && isSeparator(configuredRoot[0]) && isSeparator(configuredRoot[1]);

        std::filesystem::path root{configuredRoot};
        if (root.is_relative() && !hasNetworkOrDeviceRoot)
        {
            if (workingDirectory.empty())
                return {};
            root = workingDirectory / root;
        }
#if defined(__MINGW32__)
        // lexically_normal() collapses MinGW's leading UNC separator pair
        // because libstdc++ does not parse it as a root name.
        if (hasNetworkOrDeviceRoot)
            return root;
#endif
        return root.lexically_normal();
    }

    /** UTF-8/ASCII convenience overload; Windows callers should prefer the wide overload. */
    inline std::filesystem::path ResolveRoot(std::string_view configuredRoot,
                                             const std::filesystem::path& workingDirectory)
    {
#ifdef _WIN32
        return ResolveRootWide(std::filesystem::u8path(std::string(configuredRoot)).wstring(), workingDirectory);
#else
        if (configuredRoot.empty())
            return workingDirectory.empty() ? std::filesystem::path{} : (workingDirectory / "Saves").lexically_normal();
        std::filesystem::path root{configuredRoot};
        if (root.is_relative())
        {
            if (workingDirectory.empty())
                return {};
            root = workingDirectory / root;
        }
        return root.lexically_normal();
#endif
    }

    /** Process-stable persistence root shared by account, world, and social stores. */
    inline const std::filesystem::path& Root()
    {
        static const std::filesystem::path root = []
        {
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
#ifdef _WIN32
            const wchar_t* configured = _wgetenv(L"TF_SAVE_ROOT");
            return ResolveRootWide(configured ? std::wstring_view(configured) : std::wstring_view{},
                                   ec ? std::filesystem::path{} : cwd);
#else
            const char* configured = std::getenv("TF_SAVE_ROOT");
            return ResolveRoot(configured ? std::string_view(configured) : std::string_view{},
                               ec ? std::filesystem::path{} : cwd);
#endif
        }();
        return root;
    }

    /**
     * Resolve one store filename under Root(). Only a leaf filename is
     * accepted so an accidentally user-derived value cannot escape the save
     * root. An invalid name returns an empty path and therefore fails closed.
     */
    inline std::filesystem::path File(std::string_view leafName)
    {
        const std::filesystem::path leaf{std::string(leafName)};
        if (leaf.empty() || leaf == "." || leaf == ".." || leaf.is_absolute() || leaf.has_root_name() ||
            leaf.has_parent_path() || leaf.filename() != leaf)
        {
            return {};
        }
        const std::filesystem::path& root = Root();
        return root.empty() ? std::filesystem::path{} : root / leaf;
    }

    /** Stable keys are deliberately filename-safe and never user paths. */
    inline bool IsValidContinentKey(std::string_view key)
    {
        if (key.empty() || key.size() > 64)
            return false;
        for (const unsigned char c : key)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    /** Resolve a continent-qualified JSON store, e.g. terrafront_state.cindral_wastes.json. */
    inline std::filesystem::path ContinentFile(std::string_view stem, std::string_view continentKey)
    {
        if (!IsValidContinentKey(continentKey) || stem.empty())
            return {};
        for (const unsigned char c : stem)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return {};
        }
        return File(std::string(stem) + "." + std::string(continentKey) + ".json");
    }

    /** UTF-8 text for diagnostics only; filesystem calls must continue using path objects. */
    inline std::string Utf8ForLog(const std::filesystem::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    /** Find a legacy/current corrupt-primary recovery backup beside a store. */
    inline bool FindRecoveryBackup(const std::filesystem::path& storePath, std::filesystem::path& found,
                                   std::error_code& ec)
    {
        found.clear();
        ec.clear();
        std::filesystem::path parent = storePath.parent_path();
        if (parent.empty())
            parent = ".";
        const std::string prefix = Utf8ForLog(storePath.filename()) + ".corrupt-";
        for (std::filesystem::directory_iterator
                 it(parent, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code typeEc;
            if (!it->is_regular_file(typeEc))
                continue;
            const std::string name = Utf8ForLog(it->path().filename());
            if (name.starts_with(prefix) && name.ends_with(".bak"))
            {
                found = it->path();
                return true;
            }
        }
        return false;
    }

    /** Crash windows inside WriteDurableReplace, in commit order. */
    enum class DurableCommitStage : uint8_t
    {
        StagedAndSynced, ///< staging file complete and flushed; destination still holds the previous commit
        Renamed,         ///< staging file renamed over the destination; parent directory not yet synced
    };

    /** Observer called at each DurableCommitStage (`destination` is the path being committed). */
    using DurableCommitStageObserver = void (*)(DurableCommitStage stage, const std::filesystem::path& destination);

    /**
     * Crash-drill seam (DATA-120 recovery drill): the observer WriteDurableReplace notifies at each
     * DurableCommitStage. It is null in production. Tests install one in a spawned child process that
     * _exit()s at a stage, so the parent can prove the store reopens to its last committed state after a
     * process dies inside the commit. The observer must not throw (WriteDurableReplace is noexcept).
     */
    inline DurableCommitStageObserver& DurableCommitObserver() noexcept
    {
        static DurableCommitStageObserver observer = nullptr;
        return observer;
    }

    /**
     * Durably replace `destination` with `bytes`; the only commit primitive for TERRAFRONT stores.
     *
     * The bytes are staged in `<destination>.tmp`, forced to stable storage, and then swapped over the
     * destination in one rename, so a crash or power loss leaves either the complete previous file or the
     * complete new one, never an empty or truncated "committed" file:
     *   - POSIX: any stale staging entry is unlinked (a directory there fails the write), the staging file is
     *     created O_CREAT|O_EXCL|O_NOFOLLOW (mode 0600, the stores hold credential hashes), written in full,
     *     fsync()ed and closed, renamed over the destination, and the parent directory is fsync()ed so the
     *     rename itself survives power loss.
     *   - Windows: the staging file is created CREATE_NEW, written in full, FlushFileBuffers()ed, closed, and
     *     moved with MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), which does not return
     *     until the rename is flushed.
     * The caller must create the parent directory and serialize writers of one destination (ExclusiveFileLock).
     * Return contract: false means nothing was committed; any failure before the swap removes the staging
     * file and leaves the destination untouched. true means the destination now holds `bytes`, and callers
     * must adopt that state. Once the rename has succeeded the commit is never reported as failed: if the
     * POSIX parent-directory sync then fails (the rename is visible but may not survive power loss), the
     * function still returns true, leaves that error in `ec` as a durability warning, and logs it; the next
     * successful write re-establishes durability. `ec` is cleared on a fully durable commit.
     */
    inline bool WriteDurableReplace(const std::filesystem::path& destination, std::string_view bytes,
                                    std::error_code& ec) noexcept
    {
        ec.clear();
        if (destination.empty() || !destination.has_filename())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        std::filesystem::path temporary = destination;
        temporary += ".tmp";
        std::error_code removeEc;

#ifdef _WIN32
        if (!::DeleteFileW(temporary.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
        }
        HANDLE file =
            ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
        }
        const auto failStaged = [&](DWORD error) noexcept
        {
            ::CloseHandle(file);
            ec = std::error_code(static_cast<int>(error), std::system_category());
            std::filesystem::remove(temporary, removeEc);
            return false;
        };
        std::string_view remaining = bytes;
        while (!remaining.empty())
        {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining.size(), 1u << 30));
            DWORD written = 0;
            if (!::WriteFile(file, remaining.data(), chunk, &written, nullptr))
                return failStaged(::GetLastError());
            if (written == 0)
                return failStaged(ERROR_WRITE_FAULT);
            remaining.remove_prefix(written);
        }
        if (!::FlushFileBuffers(file))
            return failStaged(::GetLastError());
        if (!::CloseHandle(file))
        {
            ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            std::filesystem::remove(temporary, removeEc);
            return false;
        }
        if (const DurableCommitStageObserver observer = DurableCommitObserver())
            observer(DurableCommitStage::StagedAndSynced, destination);
        if (!::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            std::filesystem::remove(temporary, removeEc);
            return false;
        }
        if (const DurableCommitStageObserver observer = DurableCommitObserver())
            observer(DurableCommitStage::Renamed, destination);
        return true;
#else
        // A crashed writer can leave a staging file behind; the destination's writer lock is held, so it is
        // stale. unlink() refuses directories, so an occupied staging path still fails the write.
        if (::unlink(temporary.c_str()) != 0 && errno != ENOENT)
        {
            ec = std::error_code(errno, std::generic_category());
            return false;
        }
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
        {
            ec = std::error_code(errno, std::generic_category());
            return false;
        }
        const auto failStaged = [&](int error, bool closeFd) noexcept
        {
            if (closeFd)
                (void)::close(fd);
            ec = std::error_code(error, std::generic_category());
            std::filesystem::remove(temporary, removeEc);
            return false;
        };
        std::string_view remaining = bytes;
        while (!remaining.empty())
        {
            const ssize_t written = ::write(fd, remaining.data(), remaining.size());
            if (written < 0)
            {
                if (errno == EINTR)
                    continue;
                return failStaged(errno, true);
            }
            if (written == 0)
                return failStaged(EIO, true);
            remaining.remove_prefix(static_cast<size_t>(written));
        }
        if (::fsync(fd) != 0)
            return failStaged(errno, true);
        if (::close(fd) != 0)
            return failStaged(errno, false);
        if (const DurableCommitStageObserver observer = DurableCommitObserver())
            observer(DurableCommitStage::StagedAndSynced, destination);

        std::filesystem::rename(temporary, destination, ec);
        if (ec)
        {
            std::filesystem::remove(temporary, removeEc);
            return false;
        }
        if (const DurableCommitStageObserver observer = DurableCommitObserver())
            observer(DurableCommitStage::Renamed, destination);

        // The new bytes are committed and visible from here on, so the result is true whatever happens next;
        // a directory-sync failure only means the rename may not survive power loss.
        std::filesystem::path parent = destination.parent_path();
        if (parent.empty())
            parent = ".";
        int syncError = 0;
        const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
        {
            syncError = errno;
        }
        else
        {
            // EINVAL means the filesystem cannot sync a directory at all (the rename is as durable as it gets).
            if (::fsync(dirFd) != 0 && errno != EINVAL)
                syncError = errno;
            (void)::close(dirFd);
        }
        if (syncError != 0)
        {
            ec = std::error_code(syncError, std::generic_category());
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] %s committed, but syncing its directory failed (%s); the commit may not survive "
                           "power loss until the next successful write",
                           Utf8ForLog(destination).c_str(), ec.message().c_str());
        }
        return true;
#endif
    }

    /** Legacy pre-unified save location beside the executable (migration input only). */
    inline std::filesystem::path LegacyExecutableFile(std::string_view leafName)
    {
        const std::filesystem::path leaf{std::string(leafName)};
        if (leaf.empty() || leaf.has_parent_path() || leaf.filename() != leaf)
            return {};
#ifdef _WIN32
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0)
                return {};
            if (size < buffer.size() - 1)
            {
                buffer.resize(size);
                return std::filesystem::path(buffer).parent_path() / "Saves" / leaf;
            }
            if (buffer.size() >= 32768)
                return {};
            buffer.resize(buffer.size() * 2);
        }
#else
        return {};
#endif
    }
} // namespace Terrafront::SavePaths
