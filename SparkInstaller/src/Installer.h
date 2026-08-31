#pragma once

#include "InstallerContext.h"

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
} // namespace SparkInstaller
