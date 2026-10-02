#pragma once

#include "InstallerContext.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SparkInstaller::Preflight
{
    // Disk budgets, measured 2026-09-27 on Windows (see SparkInstaller/README.md):
    // a fresh clone with submodules is ~1.45 GiB (1.03 GiB pack + 0.42 GiB
    // checkout); a windows-release tree with the default options (tests and
    // game modules on) is ~40 GiB.
    inline constexpr std::uintmax_t kGiB = 1024ull * 1024ull * 1024ull;
    // Clone/fetch only, including an update staged with --skip-build.
    inline constexpr std::uintmax_t kDefaultMinFreeBytesSource = 2 * kGiB;
    // Clone plus a full build, including updates built beside the live tree.
    inline constexpr std::uintmax_t kDefaultMinFreeBytesBuild = 40 * kGiB;

    struct PreflightFailure
    {
        std::string code;
        std::string message;
    };

    // Budget applied when ctx.minFreeBytes is not set.
    std::uintmax_t DefaultMinFreeBytes(const InstallerContext& ctx);

    // Read-only checks run before the installer touches the destination:
    // no existing component of the destination path is a symlink/junction
    // (root-owned POSIX system links excepted), its nearest existing ancestor is
    // writable (one probe file is created and removed) and has enough free
    // space, CMake runs unless skipBuild, and in Update mode a present install
    // marker parses. ctx.destination must already be absolute and ctx.mode
    // detected. An empty result means every check passed.
    std::vector<PreflightFailure> Run(const InstallerContext& ctx);
    // Path/link/writability checks before restoring a retained tree. This does
    // not authorize a new build: Run must still pass after recovery. Restoring
    // a rename needs neither CMake nor another full build's free-space budget.
    std::vector<PreflightFailure> RunRecovery(const InstallerContext& ctx);
} // namespace SparkInstaller::Preflight
