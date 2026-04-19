#pragma once

#include "InstallerContext.h"

namespace SparkInstaller
{
    class WizardGui
    {
      public:
        // Run the ImGui wizard. Returns true if user clicked "Install" and ctx is populated.
        static bool Run(InstallerContext& ctx);
    };

    // Platform backends (implemented by one of BackendWindows.cpp or BackendLinux.cpp).
    bool GuiBackend_Init(int width, int height, const char* title);
    bool GuiBackend_BeginFrame();
    void GuiBackend_EndFrame();
    void GuiBackend_Shutdown();
} // namespace SparkInstaller
