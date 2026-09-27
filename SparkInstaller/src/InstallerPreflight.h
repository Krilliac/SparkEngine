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
    // Clone/fetch only: Install with --skip-build, or Update (the existing
    // build tree is rewritten in place rather than duplicated).
    inline constexpr std::uintmax_t kDefaultMinFreeBytesSource = 2 * kGiB;
    // Clone plus a full first build.
    inline constexpr std::uintmax_t kDefaultMinFreeBytesBuild = 40 * kGiB;

    struct PreflightFailure
    {
        std::string code;
        std::string message;
    };

    // Budget applied when ctx.minFreeBytes is not set.
    std::uintmax_t DefaultMinFreeBytes(const InstallerContext& ctx);

    // Read-only checks run before the installer touches the destination:
    // destination is not a symlink/junction, its nearest existing ancestor is
    // writable (one probe file is created and removed) and has enough free
    // space, CMake runs unless skipBuild, and in Update mode a present install
    // marker parses. ctx.destination must already be absolute and ctx.mode
    // detected. An empty result means every check passed.
    std::vector<PreflightFailure> Run(const InstallerContext& ctx);
} // namespace SparkInstaller::Preflight
