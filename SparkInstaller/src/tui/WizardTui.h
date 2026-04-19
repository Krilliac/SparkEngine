#pragma once

#include "InstallerContext.h"

namespace SparkInstaller
{
    class WizardTui
    {
      public:
        // Run the interactive TUI, filling ctx.destination/ref/options/etc.
        // Returns true if the user chose to proceed, false if they cancelled.
        static bool Run(InstallerContext& ctx);
    };
} // namespace SparkInstaller
