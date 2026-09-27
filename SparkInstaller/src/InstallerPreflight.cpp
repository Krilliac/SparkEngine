#include "InstallerPreflight.h"

#include "GitRunner.h"
#include "InstallState.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <fstream>
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
#else
#include <unistd.h>
#endif

namespace SparkInstaller::Preflight
{
    namespace fs = std::filesystem;

    namespace
    {
        bool PathEntryExists(const fs::path& path)
        {
            std::error_code error;
            // symlink_status so a dangling link still counts as an existing entry.
            return fs::exists(fs::symlink_status(path, error));
        }

        fs::path NearestExistingAncestor(const fs::path& destination)
        {
            fs::path current = destination;
            while (!PathEntryExists(current) && current.has_parent_path() && current != current.parent_path())
                current = current.parent_path();
            return current;
        }

        bool IsSymlinkOrJunction(const fs::path& path)
        {
            std::error_code error;
            if (fs::is_symlink(fs::symlink_status(path, error)))
                return true;
#ifdef _WIN32
            // std::filesystem does not portably report NTFS junctions (mount
            // points), which redirect writes exactly like a directory symlink.
            if (path == path.root_path())
                return false;
            WIN32_FIND_DATAW data{};
            const HANDLE find = ::FindFirstFileW(path.wstring().c_str(), &data);
            if (find == INVALID_HANDLE_VALUE)
                return false;
            ::FindClose(find);
            return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                   (data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || data.dwReserved0 == IO_REPARSE_TAG_SYMLINK);
#else
            return false;
#endif
        }

        unsigned long CurrentProcessId()
        {
#ifdef _WIN32
            return static_cast<unsigned long>(::GetCurrentProcessId());
#else
            return static_cast<unsigned long>(::getpid());
#endif
        }

        // Create and remove one uniquely named probe file. Refuses to reuse an
        // existing entry so the probe can never truncate or delete user data.
        bool ProbeWritable(const fs::path& directory, std::string& detail)
        {
            const fs::path probe = directory / (".sparkinstaller-preflight-" + std::to_string(CurrentProcessId()));
            if (PathEntryExists(probe))
            {
                detail = "probe path already exists: " + probe.string();
                return false;
            }
            {
                std::ofstream out(probe, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    detail = "cannot create files in " + directory.string();
                    return false;
                }
            }
            std::error_code error;
            if (!fs::remove(probe, error) || error)
            {
                detail = "cannot remove preflight probe " + probe.string();
                return false;
            }
            return true;
        }

        std::string FormatGiB(std::uintmax_t bytes)
        {
            const std::uintmax_t tenths = bytes / (kGiB / 10);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " GiB";
        }
    } // namespace

    std::uintmax_t DefaultMinFreeBytes(const InstallerContext& ctx)
    {
        return ctx.skipBuild || ctx.mode == Mode::Update ? kDefaultMinFreeBytesSource : kDefaultMinFreeBytesBuild;
    }

    std::vector<PreflightFailure> Run(const InstallerContext& ctx)
    {
        std::vector<PreflightFailure> failures;
        const fs::path destination = fs::path(ctx.destination);
        const fs::path ancestor = NearestExistingAncestor(destination);

        const bool linked = IsSymlinkOrJunction(ancestor);
        if (linked)
        {
            failures.push_back({"destination-link",
                                (ancestor == destination ? "destination " : "destination ancestor ") +
                                    ancestor.string() + " is a symlink or junction; choose the real directory path"});
        }

        std::error_code error;
        const bool isDirectory = fs::is_directory(fs::symlink_status(ancestor, error));
        if (!linked && !isDirectory)
        {
            failures.push_back({"destination-not-directory", ancestor.string() + " exists and is not a directory"});
        }

        if (!linked && isDirectory)
        {
            std::string detail;
            if (!ProbeWritable(ancestor, detail))
                failures.push_back({"destination-not-writable", detail});

            const std::uintmax_t required = ctx.minFreeBytes.value_or(DefaultMinFreeBytes(ctx));
            const fs::space_info space = fs::space(ancestor, error);
            if (error)
            {
                failures.push_back(
                    {"free-space-unknown", "cannot query free space on " + ancestor.string() + ": " + error.message()});
            }
            else if (space.available < required)
            {
                failures.push_back({"insufficient-free-space", FormatGiB(space.available) + " free on " +
                                                                   ancestor.string() + ", " + FormatGiB(required) +
                                                                   " required"});
            }
        }

        if (!ctx.skipBuild)
        {
            const std::string& cmakePath = ctx.configManager.config.cmakePath;
            const std::string cmake = cmakePath.empty() ? "cmake" : GitRunner::EncodeProcessRunnerArgument(cmakePath);
            SparkBuild::ProcessRunner runner;
            std::string output;
            if (runner.RunSync(cmake + " --version", {}, output) != 0)
            {
                failures.push_back(
                    {"cmake-unavailable", (cmakePath.empty() ? std::string("cmake on PATH") : cmakePath) +
                                              " did not run `--version`; install CMake 3.25+ and put "
                                              "it on PATH"});
            }
        }

        if (ctx.mode == Mode::Update)
        {
            const fs::path marker = destination / InstallState::FileName();
            InstallState ignored;
            if (fs::is_regular_file(fs::symlink_status(marker, error)) &&
                !InstallState::Load(destination.string(), ignored))
            {
                failures.push_back({"corrupt-install-marker",
                                    marker.string() + " exists but cannot be parsed; repair or remove it before "
                                                      "updating"});
            }
        }

        return failures;
    }
} // namespace SparkInstaller::Preflight
