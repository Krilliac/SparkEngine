#pragma once

#include "Platform.h"
#include "Config.h"
#include "ProcessRunner.h"
#include <string>
#include <memory>

namespace SparkBuild
{

    class SparkBuildApp
    {
      public:
        SparkBuildApp();
        ~SparkBuildApp();

        // Run the interactive TUI application
        int Run();

      private:
        // Main menu
        void ShowMainMenu();

        // Environment checks
        void ShowEnvironmentMenu();
        void CheckAll();
        void CheckGit();
        void CheckCMake();
        void CheckCompilers();
        void CheckSubmodules();
        void CheckEngineRepo();
        void CloneEngine();
        void DownloadCMake();
        void InitSubmodules();

        // Configuration
        void ShowConfigureMenu();
        void ShowGeneratorMenu();
        void ShowBuildTypeMenu();
        void ShowOptionsMenu();
        void ShowPresetsMenu();
        void SetEnginePath();
        void SetBuildPath();
        void ShowCMakePresetsMenu();

        // Build
        void ShowBuildMenu();
        void GenerateProject();
        void BuildProject();
        void GenerateAndBuild();
        void CleanBuild();
        void OpenBuildFolder();
        void RunEngine();

        // Show current config summary
        void ShowConfigSummary();

        // Platform-specific helpers
        void ShowPackageHints(const std::string& package);

        ConfigManager m_config;
        ProcessRunner m_processRunner;
        bool m_running = true;
    };

} // namespace SparkBuild
