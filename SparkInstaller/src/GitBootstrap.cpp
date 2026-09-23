#include "GitBootstrap.h"
#include "GitRunner.h"

#include "Downloader.h"
#include "Platform.h"
#include "ProcessRunner.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace SparkInstaller
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr const char* kPortableGitDirName = "mingit";
        constexpr const char* kPortableGitMarkerName = ".spark-portable-git.sha256";
        constexpr std::uintmax_t kMaxPortableGitMarkerBytes = 256;

        std::atomic<std::uint64_t> g_portableGitSequence{0};

        bool IsSha256Hex(const std::string& value)
        {
            if (value.size() != 64)
                return false;
            for (const char character : value)
            {
                const bool hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F');
                if (!hex)
                    return false;
            }
            return true;
        }

        std::string LowerAscii(std::string value)
        {
            for (char& character : value)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
            return value;
        }

        // symlink_status never follows a symlink or junction, so a link planted
        // at any of these paths is rejected instead of being trusted through.
        fs::file_type EntryType(const fs::path& path)
        {
            std::error_code error;
            return fs::symlink_status(path, error).type();
        }

        bool IsRealDirectory(const fs::path& path)
        {
            return EntryType(path) == fs::file_type::directory;
        }

        bool IsRealRegularFile(const fs::path& path)
        {
            return EntryType(path) == fs::file_type::regular;
        }

        // Every component below root must be a real directory and the final one
        // a real regular file. Absolute, rooted, empty, "." and ".." components
        // are rejected so the executable can never resolve outside the tree.
        bool IsContainedRegularFile(const fs::path& root, const fs::path& relative)
        {
            if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
                relative.has_root_directory() || !IsRealDirectory(root))
                return false;

            const std::vector<fs::path> parts(relative.begin(), relative.end());
            if (parts.empty())
                return false;
            fs::path current = root;
            for (size_t index = 0; index < parts.size(); ++index)
            {
                const fs::path& part = parts[index];
                if (part.empty() || part == "." || part == "..")
                    return false;
                current /= part;
                const bool last = index + 1 == parts.size();
                if (last ? !IsRealRegularFile(current) : !IsRealDirectory(current))
                    return false;
            }
            return true;
        }

        bool ReadActivationMarker(const fs::path& marker, std::string& contents)
        {
            if (!IsRealRegularFile(marker))
                return false;
            std::error_code error;
            const std::uintmax_t size = fs::file_size(marker, error);
            if (error || size > kMaxPortableGitMarkerBytes)
                return false;

            std::ifstream in(marker, std::ios::binary);
            if (!in)
                return false;
            std::string buffer(static_cast<size_t>(size), '\0');
            if (size != 0)
            {
                in.read(buffer.data(), static_cast<std::streamsize>(size));
                if (in.gcount() != static_cast<std::streamsize>(size))
                    return false;
            }
            while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r'))
                buffer.pop_back();
            contents = buffer;
            return true;
        }

        bool WriteActivationMarker(const fs::path& directory, const std::string& sha256)
        {
            std::ofstream out(directory / kPortableGitMarkerName, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out << sha256 << '\n';
            out.flush();
            out.close();
            return static_cast<bool>(out);
        }

        bool IsVerifiedPortableGitTree(const fs::path& root, const fs::path& exeRelative, const std::string& sha256)
        {
            std::string marker;
            return IsRealDirectory(root) && ReadActivationMarker(root / kPortableGitMarkerName, marker) &&
                   LowerAscii(marker) == sha256 && IsContainedRegularFile(root, exeRelative);
        }

        fs::path UniqueSibling(const fs::path& parent, const std::string& prefix)
        {
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            const std::uint64_t sequence = g_portableGitSequence.fetch_add(1, std::memory_order_relaxed);
            return parent / (prefix + std::to_string(tick) + "-" + std::to_string(sequence));
        }

        // create_directory only returns true for a directory this call created,
        // so the staging tree can never alias a pre-existing (or planted) path.
        bool CreateFreshDirectory(const fs::path& parent, const std::string& prefix, fs::path& created)
        {
            for (int attempt = 0; attempt < 64; ++attempt)
            {
                const fs::path candidate = UniqueSibling(parent, prefix);
                std::error_code error;
                if (fs::create_directory(candidate, error))
                {
                    created = candidate;
                    return true;
                }
                if (error)
                    return false;
            }
            return false;
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
        // MinGit portable release. URL and SHA-256 are pinned to the upstream release manifest.
        constexpr const char* kPortableGitUrl =
            "https://github.com/git-for-windows/git/releases/download/v2.45.2.windows.1/"
            "MinGit-2.45.2-64-bit.zip";
        constexpr const char* kPortableGitSha256 = "7ed2a3ce5bbbf8eea976488de5416894ca3e6a0347cee195a7d768ac146d5290";
        constexpr const char* kPortableGitExeRel = "cmd\\git.exe";
#endif
    } // namespace

    bool GitBootstrap::IsGitAvailableOnPath()
    {
        SparkBuild::ProcessRunner runner;
        std::string output;
        return runner.RunSync(GitRunner::EncodeProcessRunnerArgument("git") + " --version", {}, output) == 0;
    }

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
        if (IsGitAvailableOnPath())
        {
            result.ok = true;
            result.gitExe = "git";
            if (log)
                log("Git found on PATH.");
            return result;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (log)
            log("Git not found on PATH. Using the verified portable MinGit cache...");
        return EnsurePortableGitCache(
            CacheDir(), kPortableGitSha256, kPortableGitExeRel,
            [&log](const std::string& stagingDir)
            {
                if (log)
                    log(std::string("Downloading: ") + kPortableGitUrl);
                return SparkBuild::Downloader::DownloadAndExtract(kPortableGitUrl, stagingDir, kPortableGitSha256);
            },
            log);
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

    GitBootstrapResult GitBootstrap::EnsurePortableGitCache(const std::string& cacheDir,
                                                            const std::string& expectedSha256,
                                                            const std::string& exeRelativePath,
                                                            const PortableGitPopulator& populate, const LogSink& log)
    {
        GitBootstrapResult result;
        const auto fail = [&result](const std::string& message)
        {
            result.ok = false;
            result.gitExe.clear();
            result.diagnosticMessage = message + " Please install Git manually and retry.";
            return result;
        };

        const fs::path exeRelative(exeRelativePath);
        if (cacheDir.empty() || !IsSha256Hex(expectedSha256) || exeRelative.empty() || exeRelative.is_absolute() ||
            exeRelative.has_root_name() || exeRelative.has_root_directory() || !populate)
            return fail("Portable Git cache request is invalid.");
        for (const fs::path& part : exeRelative)
        {
            if (part.empty() || part == "." || part == "..")
                return fail("Portable Git executable path escapes the cache.");
        }

        const std::string sha256 = LowerAscii(expectedSha256);
        const fs::path cache(cacheDir);
        const fs::path active = cache / kPortableGitDirName;

        if (IsVerifiedPortableGitTree(active, exeRelative, sha256))
        {
            if (log)
                log("Using verified portable Git cache: " + active.string());
            result.ok = true;
            result.gitExe = (active / exeRelative).string();
            return result;
        }

        std::error_code error;
        fs::create_directories(cache, error);
        if (error || !IsRealDirectory(cache))
            return fail("Could not create the portable Git cache directory.");

        // Populate a unique staging tree; the trusted path is untouched until
        // the staged tree is complete and verified.
        fs::path staging;
        if (!CreateFreshDirectory(cache, std::string(kPortableGitDirName) + ".staging-", staging))
            return fail("Could not create a portable Git staging directory.");
        const auto discardStaging = [&staging]
        {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
        };

        if (!populate(staging.string()))
        {
            discardStaging();
            return fail("Failed to download/extract portable Git.");
        }
        if (!IsContainedRegularFile(staging, exeRelative))
        {
            discardStaging();
            return fail("Portable Git archive did not contain the expected executable.");
        }
        if (!WriteActivationMarker(staging, sha256))
        {
            discardStaging();
            return fail("Could not record portable Git verification.");
        }

        // Anything already at the trusted path failed verification (partial
        // extraction, stale pin, tampering, or a link). Rename it aside rather
        // than deleting: removal could follow a planted link, and retaining it
        // leaves evidence for diagnosis.
        const fs::file_type activeType = EntryType(active);
        if (activeType == fs::file_type::none)
        {
            discardStaging();
            return fail("Could not inspect the existing portable Git cache.");
        }
        if (activeType != fs::file_type::not_found)
        {
            const fs::path quarantine = UniqueSibling(cache, std::string(kPortableGitDirName) + ".untrusted-");
            fs::rename(active, quarantine, error);
            if (error)
            {
                discardStaging();
                return fail("Could not retire the unverified portable Git cache.");
            }
            if (log)
                log("Retired unverified portable Git cache to: " + quarantine.string());
        }

        fs::rename(staging, active, error);
        if (error)
        {
            discardStaging();
            return fail("Could not activate the verified portable Git cache.");
        }
        if (!IsVerifiedPortableGitTree(active, exeRelative, sha256))
            return fail("Activated portable Git cache failed verification.");

        if (log)
            log("Activated verified portable Git cache: " + active.string());
        result.ok = true;
        result.gitExe = (active / exeRelative).string();
        result.diagnosticMessage.clear();
        return result;
    }
} // namespace SparkInstaller
