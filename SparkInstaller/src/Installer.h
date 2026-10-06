#pragma once

#include "InstallerContext.h"

#include <filesystem>
#include <string>

namespace SparkInstaller
{
#ifndef SPARK_INSTALLER_VERSION
#error "SPARK_INSTALLER_VERSION must be supplied by the build system"
#endif

    class Installer
    {
      public:
        // Drives the full install/update flow using values prepared in ctx.
        // Returns 0 on success, non-zero on failure. Progress/diagnostics are
        // streamed to ctx.log if set.
        static int Run(InstallerContext& ctx);
    };

    inline constexpr const char* kInstallerVersion = SPARK_INSTALLER_VERSION;

    namespace detail
    {
        // Without previous, destination must be absent or empty. For updates,
        // previous names an absent sibling: retain the live tree there before
        // activating the verified staging tree. A failed activation restores it.
        // The caller serializes activation/recovery for this destination.
        bool ActivateStagedTree(const std::filesystem::path& staging, const std::filesystem::path& destination,
                                std::string& error, const std::filesystem::path& previous = {});
        // Recover a process interrupted between the two activation renames.
        // When both trees exist, archive the previous tree without deleting it.
        bool RecoverPreviousTree(const std::filesystem::path& destination, std::string& error);
    } // namespace detail
} // namespace SparkInstaller
