/**
 * @file CrashArtifactRetention.h
 * @brief Bounded retention for the per-process spark_crash_<pid>_<random> directories.
 *
 * Every InstallCrashHandler() creates one private artifact directory under the
 * user's temp directory, and a crash leaves its dump, log, manifest and
 * screenshot there. Nothing else ever removes them, so without this policy
 * empty directories pile up on every launch and crash dumps (which can hold
 * process memory) outlive their use.
 *
 * Contract:
 * - Thread affinity: any thread, but called once from InstallCrashHandler()
 *   before the new directory is created. Never from a crash path.
 * - Ownership: deletes only directories that match the exact generated name,
 *   are real directories (no symlink or reparse point), are owned by the
 *   current user, and whose recorded process is no longer running.
 * - Allocation: ordinary heap use; startup only, never a hot path.
 * - Scalability: examines at most kMaxRetentionEntriesExamined temp entries.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <climits>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Spark::CrashHandlerDetail
{
    /// Retention limits for crash-artifact directories left by earlier processes.
    struct CrashArtifactRetention
    {
        std::chrono::hours maxAge{24 * 14};                     ///< Older non-empty directories are removed
        std::size_t maxDirectories = 16;                        ///< Newest non-empty directories kept
        std::uintmax_t maxTotalBytes = std::uintmax_t{2} << 30; ///< Byte budget across kept directories
    };

    /// Upper bound on temp-directory entries one prune pass examines.
    inline constexpr std::size_t kMaxRetentionEntriesExamined = 16384;

    /// Parse "spark_crash_<pid>_<32 lowercase hex>" (the exact generated name).
    inline bool ParseCrashArtifactDirectoryName(std::string_view name, unsigned long& processId)
    {
        constexpr std::string_view prefix = "spark_crash_";
        constexpr std::size_t kSuffixLength = 32;
        if (!name.starts_with(prefix))
        {
            return false;
        }
        name.remove_prefix(prefix.size());
        const std::size_t separator = name.find('_');
        if (separator == 0 || separator == std::string_view::npos || separator > 10 ||
            name.size() - separator - 1 != kSuffixLength)
        {
            return false;
        }

        std::uint64_t parsed = 0;
        for (std::size_t index = 0; index < separator; ++index)
        {
            const char digit = name[index];
            if (digit < '0' || digit > '9')
            {
                return false;
            }
            parsed = parsed * 10 + static_cast<std::uint64_t>(digit - '0');
        }
        if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
        for (const char character : name.substr(separator + 1))
        {
            if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            {
                return false;
            }
        }
        processId = static_cast<unsigned long>(parsed);
        return true;
    }

    namespace Private
    {
        /// True unless the process is known to have exited. Unknown counts as alive.
        inline bool ProcessMayBeRunning(unsigned long processId)
        {
#ifdef _WIN32
            // Windows ignores the low two PID bits when looking a process up,
            // so a PID that is not a multiple of four is never a real process.
            if (processId % 4 != 0)
                return false;
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (!process)
                return GetLastError() != ERROR_INVALID_PARAMETER;
            DWORD exitCode = 0;
            const bool running = !GetExitCodeProcess(process, &exitCode) || exitCode == STILL_ACTIVE;
            CloseHandle(process);
            return running;
#else
            // kill() with a negative or zero PID addresses a process group.
            if (processId == 0 || processId > static_cast<unsigned long>(INT_MAX))
            {
                return false;
            }
            if (kill(static_cast<pid_t>(processId), 0) == 0)
            {
                return true;
            }
            return errno != ESRCH;
#endif
        }

#ifdef _WIN32
        inline bool CurrentUserOwns(const std::filesystem::path& directory)
        {
            const HANDLE handle =
                CreateFileW(directory.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return false;
            BY_HANDLE_FILE_INFORMATION info{};
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            bool owned = GetFileInformationByHandle(handle, &info) &&
                         (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                         (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                         GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                         nullptr, &descriptor) == ERROR_SUCCESS &&
                         owner != nullptr;
            CloseHandle(handle);

            if (owned)
            {
                owned = false;
                HANDLE token = nullptr;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                {
                    // The directory's owner is the token's default owner: the
                    // user, or Administrators for an elevated process.
                    alignas(TOKEN_USER) unsigned char userBuffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
                    alignas(TOKEN_OWNER) unsigned char ownerBuffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_OWNER)];
                    DWORD length = 0;
                    if (GetTokenInformation(token, TokenUser, userBuffer, sizeof(userBuffer), &length))
                        owned = EqualSid(owner, reinterpret_cast<TOKEN_USER*>(userBuffer)->User.Sid) != FALSE;
                    if (!owned && GetTokenInformation(token, TokenOwner, ownerBuffer, sizeof(ownerBuffer), &length))
                        owned = EqualSid(owner, reinterpret_cast<TOKEN_OWNER*>(ownerBuffer)->Owner) != FALSE;
                    CloseHandle(token);
                }
            }
            if (descriptor)
                LocalFree(descriptor);
            return owned;
        }
#else
        inline bool CurrentUserOwns(const std::filesystem::path& directory)
        {
            struct stat info
            {
            };
            return lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
                   (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
        }
#endif

        /// Copy the leaf name of @p path into @p name when it is at most 64
        /// ASCII characters. Generated names are ASCII; anything else is
        /// rejected before a narrowing conversion could throw.
        inline bool TryGetAsciiLeafName(const std::filesystem::path& path, std::string& name)
        {
            // filename() returns by value: keep it alive for as long as the
            // reference returned by native() is read.
            const std::filesystem::path leaf = path.filename();
            const auto& nativeName = leaf.native();
            if (nativeName.size() > 64 ||
                !std::all_of(nativeName.begin(), nativeName.end(), [](auto character)
                             { return character > 0 && static_cast<std::uint32_t>(character) < 0x80; }))
            {
                return false;
            }
            name.clear();
            name.reserve(nativeName.size());
            for (const auto character : nativeName)
            {
                name.push_back(static_cast<char>(character));
            }
            return true;
        }

        struct RetentionCandidate
        {
            std::filesystem::path path;
            std::filesystem::file_time_type lastWrite;
            std::uintmax_t bytes = 0;
            bool empty = true;
        };

        /// Sum regular-file sizes in one flat artifact directory. Returns false
        /// when the directory holds anything but regular files (never generated
        /// by the crash handler), so such a directory is left alone.
        inline bool MeasureArtifactDirectory(RetentionCandidate& candidate)
        {
            std::error_code error;
            std::size_t entries = 0;
            std::filesystem::directory_iterator it(candidate.path, error);
            for (; !error && it != std::filesystem::directory_iterator(); it.increment(error))
            {
                if (++entries > 4096)
                {
                    return false;
                }
                std::error_code entryError;
                const std::filesystem::file_status status = it->symlink_status(entryError);
                if (entryError || !std::filesystem::is_regular_file(status))
                {
                    return false;
                }
                const std::uintmax_t size = it->file_size(entryError);
                if (entryError)
                {
                    return false;
                }
                candidate.bytes += size;
                candidate.empty = false;
            }
            return !error;
        }
    } // namespace Private

    /**
     * @brief Remove crash-artifact directories left by processes that have exited.
     *
     * Empty directories are always removed. Non-empty ones are kept newest
     * first up to @p retention.maxDirectories and @p retention.maxTotalBytes,
     * and any older than @p retention.maxAge are removed.
     *
     * @param baseDirectory The temp directory that holds spark_crash_* roots.
     * @param retention The limits to apply.
     * @return The number of directories removed.
     */
    inline std::size_t PruneStaleCrashArtifactDirectories(const std::filesystem::path& baseDirectory,
                                                          const CrashArtifactRetention& retention = {})
    {
        std::error_code error;
        std::vector<Private::RetentionCandidate> candidates;
        std::size_t examined = 0;
        std::filesystem::directory_iterator it(baseDirectory, error);
        for (; !error && it != std::filesystem::directory_iterator(); it.increment(error))
        {
            if (++examined > kMaxRetentionEntriesExamined)
            {
                break;
            }
            const std::filesystem::directory_entry& entry = *it;
            std::string name;
            if (!Private::TryGetAsciiLeafName(entry.path(), name))
            {
                continue;
            }
            unsigned long processId = 0;
            if (!ParseCrashArtifactDirectoryName(name, processId))
            {
                continue;
            }
            std::error_code statusError;
            if (!std::filesystem::is_directory(entry.symlink_status(statusError)) || statusError)
            {
                continue; // symlinks and plain files are never followed or removed
            }
            if (!Private::CurrentUserOwns(entry.path()) || Private::ProcessMayBeRunning(processId))
            {
                continue;
            }

            Private::RetentionCandidate candidate;
            candidate.path = entry.path();
            candidate.lastWrite = std::filesystem::last_write_time(candidate.path, statusError);
            if (statusError || !Private::MeasureArtifactDirectory(candidate))
            {
                continue;
            }
            candidates.push_back(std::move(candidate));
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Private::RetentionCandidate& left, const Private::RetentionCandidate& right)
                  { return left.lastWrite > right.lastWrite; });

        const auto now = std::filesystem::file_time_type::clock::now();
        std::size_t kept = 0;
        std::uintmax_t keptBytes = 0;
        std::size_t removed = 0;
        for (const Private::RetentionCandidate& candidate : candidates)
        {
            const bool tooOld = now - candidate.lastWrite > retention.maxAge;
            const bool overBudget =
                kept >= retention.maxDirectories || candidate.bytes > retention.maxTotalBytes - keptBytes;
            if (!candidate.empty && !tooOld && !overBudget)
            {
                ++kept;
                keptBytes += candidate.bytes;
                continue;
            }
            std::error_code removeError;
            if (std::filesystem::remove_all(candidate.path, removeError) != static_cast<std::uintmax_t>(-1) &&
                !removeError)
            {
                ++removed;
            }
        }
        return removed;
    }
} // namespace Spark::CrashHandlerDetail
