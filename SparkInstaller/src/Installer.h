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
        // Move a fully cloned staging tree to destination, which must be absent
        // or an empty directory. Never overwrites: on failure both trees are
        // left as they were (an emptied destination is recreated) and error
        // says why.
        bool ActivateStagedTree(const std::filesystem::path& staging, const std::filesystem::path& destination,
                                std::string& error);
    } // namespace detail
} // namespace SparkInstaller
