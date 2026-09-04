/**
 * @file CrashArtifactDirectory.h
 * @brief Private crash-artifact directory creation with platform-specific security.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::CrashHandlerDetail
{
    namespace Private
    {
        inline bool FillRandomBytes(std::array<std::uint8_t, 16>& bytes)
        {
#ifdef _WIN32
            return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                   BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
            const int fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
                                                    | O_CLOEXEC
#endif
            );
            if (fd < 0)
                return false;

            size_t offset = 0;
            while (offset < bytes.size())
            {
                const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
                offset += static_cast<size_t>(count);
            }
            close(fd);
            return offset == bytes.size();
#endif
        }

        inline std::string RandomDirectorySuffix()
        {
            std::array<std::uint8_t, 16> bytes{};
            if (!FillRandomBytes(bytes))
                return {};

            constexpr char hex[] = "0123456789abcdef";
            std::string suffix;
            suffix.resize(bytes.size() * 2);
            for (size_t index = 0; index < bytes.size(); ++index)
            {
                suffix[index * 2] = hex[bytes[index] >> 4];
                suffix[index * 2 + 1] = hex[bytes[index] & 0x0F];
            }
            return suffix;
        }
    } // namespace Private

    /**
     * @brief Atomically create one owner-only crash-artifact directory.
     *
     * Existing paths, symlinks, and reparse points are never accepted. This
     * exact-candidate entry point is intentionally exposed for deterministic
     * security tests; normal callers should use CreatePrivateCrashArtifactDirectory.
     */
    inline bool TryCreatePrivateCrashArtifactDirectory(const std::filesystem::path& candidate)
    {
        if (candidate.empty())
            return false;

#ifdef _WIN32
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1,
                                                                  &securityDescriptor, nullptr))
        {
            return false;
        }

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = securityDescriptor;
        const BOOL created = CreateDirectoryW(candidate.c_str(), &securityAttributes);
        LocalFree(securityDescriptor);
        if (!created)
            return false;

        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        const bool valid = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        if (!valid)
            RemoveDirectoryW(candidate.c_str());
        return valid;
#else
        if (mkdir(candidate.c_str(), S_IRWXU) != 0)
            return false;

        int flags = O_RDONLY;
#ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const int fd = open(candidate.c_str(), flags);
        struct stat info
        {
        };
        const bool valid = fd >= 0 && fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
                           (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
        if (fd >= 0)
            close(fd);
        if (!valid)
            rmdir(candidate.c_str());
        return valid;
#endif
    }

    /** @brief Create a randomized, exclusive, owner-only crash-artifact directory. */
    inline std::filesystem::path CreatePrivateCrashArtifactDirectory(const std::filesystem::path& baseDirectory,
                                                                     unsigned long processId)
    {
        if (baseDirectory.empty())
            return {};

        std::error_code error;
        const std::filesystem::path absoluteBase = std::filesystem::absolute(baseDirectory, error).lexically_normal();
        if (error)
            return {};

        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const std::string suffix = Private::RandomDirectorySuffix();
            if (suffix.empty())
                return {};
            const std::filesystem::path candidate =
                absoluteBase / ("spark_crash_" + std::to_string(processId) + "_" + suffix);
            if (TryCreatePrivateCrashArtifactDirectory(candidate))
                return candidate;
        }
        return {};
    }
} // namespace Spark::CrashHandlerDetail
