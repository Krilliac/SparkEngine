/**
 * @file RuntimePackage.cpp
 * @brief Cross-platform executable discovery and packaged-runtime anchoring.
 */
#include "RuntimePackage.h"

#include "Platform.h"

#include <string_view>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#elif defined(SPARK_PLATFORM_MACOS)
#include <mach-o/dyld.h>
#endif

namespace Spark::RuntimePackage
{
    std::filesystem::path GetExecutableDirectory()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        constexpr size_t kInitialPathCapacity = 512;
        constexpr size_t kMaximumPathCapacity = 32768;
        for (size_t capacity = kInitialPathCapacity; capacity <= kMaximumPathCapacity; capacity *= 2)
        {
            std::vector<wchar_t> buffer(capacity);
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return {};
            if (length < buffer.size())
                return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
        }
        return {};
#elif defined(SPARK_PLATFORM_MACOS)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};

        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), error);
        return error ? std::filesystem::path(buffer.data()).parent_path() : canonical.parent_path();
#else
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
            return executable.parent_path();
        return {};
#endif
    }

    WorkingDirectoryResult AnchorWorkingDirectory(const std::filesystem::path& executableDirectory,
                                                  std::error_code& error)
    {
        error.clear();
        if (executableDirectory.empty() ||
            !std::filesystem::is_regular_file(executableDirectory / "manifest.json", error) || error)
        {
            error.clear();
            return WorkingDirectoryResult::NotPackaged;
        }
        if (!std::filesystem::is_regular_file(executableDirectory / "spark.modules.json", error) || error)
        {
            error.clear();
            return WorkingDirectoryResult::NotPackaged;
        }

        const auto current = std::filesystem::current_path(error);
        if (error)
            return WorkingDirectoryResult::Failed;

        if (std::filesystem::equivalent(current, executableDirectory, error) && !error)
            return WorkingDirectoryResult::AlreadyAnchored;
        error.clear();

        std::filesystem::current_path(executableDirectory, error);
        return error ? WorkingDirectoryResult::Failed : WorkingDirectoryResult::Anchored;
    }
} // namespace Spark::RuntimePackage
