/** @file LauncherPaths.cpp @brief SparkLauncher executable and template path discovery. */
#include "LauncherPaths.h"

#include <array>
#include <cstdint>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace SparkLauncher
{
    std::filesystem::path GetLauncherExecutablePath()
    {
#ifdef _WIN32
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return {};
        return std::filesystem::path(buffer.data(), buffer.data() + length);
#elif defined(__APPLE__)
        uint32_t requiredSize = 0;
        (void)_NSGetExecutablePath(nullptr, &requiredSize);
        if (requiredSize == 0)
            return {};

        std::vector<char> buffer(requiredSize);
        if (_NSGetExecutablePath(buffer.data(), &requiredSize) != 0)
            return {};

        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), error);
        return error ? std::filesystem::path(buffer.data()) : canonical;
#else
        std::array<char, 4096> buffer{};
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size() - 1)
            return {};
        buffer[static_cast<std::size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data());
#endif
    }

    std::filesystem::path FindLauncherTemplatesDirectory(const std::filesystem::path& executablePath,
                                                         const std::filesystem::path& currentDirectory)
    {
        const std::filesystem::path executableDirectory = executablePath.parent_path();
        const std::array candidates = {
            // An installed launcher must be self-contained and deterministic:
            // never let the caller's current directory shadow packaged data.
            executableDirectory / ".." / "share" / "SparkEngine" / "templates",
            executableDirectory / "Templates",
            currentDirectory / "Templates",
            currentDirectory.parent_path() / "Templates",
        };

        for (const auto& candidate : candidates)
        {
            // Normalize "bin/../share" before touching the filesystem.  POSIX
            // path traversal requires every intermediate component to exist;
            // an installed-layout probe must still work when the synthetic or
            // not-yet-launched executable path has no physical bin directory.
            const auto normalizedCandidate = candidate.lexically_normal();
            std::error_code error;
            if (!std::filesystem::is_directory(normalizedCandidate, error) || error)
                continue;
            const auto canonical = std::filesystem::canonical(normalizedCandidate, error);
            if (!error)
                return canonical;
        }
        return {};
    }
} // namespace SparkLauncher
