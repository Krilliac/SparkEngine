#include "GitBootstrap.h"

#include "Downloader.h"
#include "Platform.h"
#include "ProcessRunner.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace SparkInstaller
{
    namespace fs = std::filesystem;

    namespace
    {
        bool IsGitOnPath()
        {
            SparkBuild::ProcessRunner runner;
            std::string output;
#ifdef SPARK_PLATFORM_WINDOWS
            return runner.RunSync("where git", {}, output) == 0;
#else
            return runner.RunSync("command -v git", {}, output) == 0;
#endif
        }

        std::string HomeDir()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            const char* userProfile = std::getenv("USERPROFILE");
            return userProfile ? userProfile : "";
#else
            const char* home = std::getenv("HOME");
            return home ? home : "";
#endif
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // MinGit portable release. Pinned URL; the installer validates archive integrity by unpack success.
        constexpr const char* kPortableGitUrl =
            "https://github.com/git-for-windows/git/releases/download/v2.45.2.windows.1/"
            "MinGit-2.45.2-64-bit.zip";
        constexpr const char* kPortableGitExeRel = "cmd\\git.exe";
#endif
    } // namespace

    std::string GitBootstrap::CacheDir()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        const char* localAppData = std::getenv("LOCALAPPDATA");
        fs::path base = localAppData ? localAppData : HomeDir();
        fs::path p = base / "SparkInstaller" / "cache";
#elif defined(SPARK_PLATFORM_MACOS)
        fs::path p = fs::path(HomeDir()) / "Library" / "Caches" / "SparkInstaller";
#else
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(HomeDir()) / ".cache";
        fs::path p = base / "SparkInstaller";
#endif
        std::error_code ec;
        fs::create_directories(p, ec);
        return p.string();
    }

    GitBootstrapResult GitBootstrap::Ensure(const LogSink& log)
    {
        GitBootstrapResult result;
        if (IsGitOnPath())
        {
            result.ok = true;
            result.gitExe = "git";
            if (log)
                log("Git found on PATH.");
            return result;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (log)
            log("Git not found on PATH. Fetching portable MinGit...");
        fs::path cache = CacheDir();
        fs::path gitRoot = cache / "mingit";
        fs::path gitExe = gitRoot / kPortableGitExeRel;
        if (!fs::exists(gitExe))
        {
            std::error_code ec;
            fs::create_directories(gitRoot, ec);
            if (log)
                log(std::string("Downloading: ") + kPortableGitUrl);
            if (!SparkBuild::Downloader::DownloadAndExtract(kPortableGitUrl, gitRoot.string()))
            {
                result.diagnosticMessage = "Failed to download/extract MinGit. Please install Git manually and retry.";
                return result;
            }
        }
        if (!fs::exists(gitExe))
        {
            result.diagnosticMessage = "MinGit archive extracted but git.exe not found at expected path.";
            return result;
        }
        result.ok = true;
        result.gitExe = gitExe.string();
        return result;
#elif defined(SPARK_PLATFORM_MACOS)
        result.diagnosticMessage = "Git not found. Install via Xcode Command Line Tools:\n"
                                   "    xcode-select --install\n"
                                   "Then re-run SparkInstaller.";
        return result;
#else
        result.diagnosticMessage = "Git not found. Install via your package manager, e.g.:\n"
                                   "    Debian/Ubuntu:  sudo apt-get install git\n"
                                   "    Fedora/RHEL:    sudo dnf install git\n"
                                   "    Arch:           sudo pacman -S git\n"
                                   "Then re-run SparkInstaller.";
        return result;
#endif
    }
} // namespace SparkInstaller
