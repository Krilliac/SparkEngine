#pragma once

#include "InstallerContext.h"

namespace SparkInstaller
{
    class Installer
    {
      public:
        // Drives the full install/update flow using values prepared in ctx.
        // Returns 0 on success, non-zero on failure. Progress/diagnostics are
        // streamed to ctx.log if set.
        static int Run(InstallerContext& ctx);
    };

    constexpr const char* kInstallerVersion = "1.0.0";
} // namespace SparkInstaller
