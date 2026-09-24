#pragma once

#include "InstallerContext.h"

#include <functional>
#include <string>

namespace SparkInstaller
{
    struct GitBootstrapResult
    {
        bool ok = false;
        std::string gitExe;
        std::string diagnosticMessage;
    };

    // Fills a freshly created, empty staging directory with the contents of a
    // hash-verified portable Git archive. Returns false on any failure.
    using PortableGitPopulator = std::function<bool(const std::string& stagingDir)>;

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

        // Resolves the portable Git tree at <cacheDir>/mingit. A cached tree is
        // trusted only when it is a real directory (not a symlink or junction)
        // carrying the activation marker for expectedSha256 and exeRelativePath
        // names a real regular file inside it. Otherwise `populate` fills a
        // unique staging directory, the staged tree is verified and marked, any
        // untrusted previous tree is renamed aside (never deleted or followed),
        // and the staged tree is renamed into place. An interrupted, failed, or
        // wrong-content population therefore never becomes the trusted tree.
        static GitBootstrapResult EnsurePortableGitCache(const std::string& cacheDir, const std::string& expectedSha256,
                                                         const std::string& exeRelativePath,
                                                         const PortableGitPopulator& populate, const LogSink& log);
    };
} // namespace SparkInstaller
