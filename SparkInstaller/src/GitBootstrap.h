#pragma once

#include "InstallerContext.h"

#include <string>

namespace SparkInstaller
{
    struct GitBootstrapResult
    {
        bool ok = false;
        std::string gitExe;
        std::string diagnosticMessage;
    };

    class GitBootstrap
    {
      public:
        // Returns a usable git executable path. If git is already on PATH, returns "git".
        // Otherwise attempts to fetch a portable Git for the current OS into the installer
        // cache directory and returns the absolute path.
        static GitBootstrapResult Ensure(const LogSink& log);

        // Shell-less PATH probe used by Ensure and focused regression tests.
        static bool IsGitAvailableOnPath();

        // Platform-appropriate per-user cache dir (created on demand):
        //   Windows: %LOCALAPPDATA%/SparkInstaller/cache
        //   Linux:   $XDG_CACHE_HOME/SparkInstaller or ~/.cache/SparkInstaller
        //   macOS:   ~/Library/Caches/SparkInstaller
        static std::string CacheDir();
    };
} // namespace SparkInstaller
