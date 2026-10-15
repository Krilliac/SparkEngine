#include "Installer.h"

#include "Config.h"
#include "Downloader.h"
#include "GitBootstrap.h"
#include "GitRunner.h"
#include "InstallState.h"
#include "InstallerPreflight.h"
#include "PathUtf8.h"
#include "ProcessRunner.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace SparkInstaller
{
    namespace fs = std::filesystem;

    namespace
    {
        void Emit(const LogSink& log, const std::string& msg)
        {
            if (log)
            {
                log(msg);
            }
        }

        // Keep the lock file in place: unlinking it would allow two processes
        // to lock different file identities for the same destination. Kernel
        // ownership is released even when an installer is killed.
        class DestinationLock
        {
          public:
            explicit DestinationLock(const fs::path& destination)
            {
                fs::path path = destination;
                path += ".sparkinstall-lock";
#ifdef _WIN32
                m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                       FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (m_handle != INVALID_HANDLE_VALUE)
                {
                    BY_HANDLE_FILE_INFORMATION info{};
                    if (!GetFileInformationByHandle(m_handle, &info) ||
                        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
                    {
                        CloseHandle(m_handle);
                        m_handle = INVALID_HANDLE_VALUE;
                    }
                }
#else
                m_fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
                struct stat info
                {
                };
                if (m_fd >= 0 &&
                    (fstat(m_fd, &info) != 0 || !S_ISREG(info.st_mode) || flock(m_fd, LOCK_EX | LOCK_NB) != 0))
                {
                    close(m_fd);
                    m_fd = -1;
                }
#endif
            }

            ~DestinationLock()
            {
#ifdef _WIN32
                if (m_handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(m_handle);
                }
#else
                if (m_fd >= 0)
                {
                    close(m_fd);
                }
#endif
            }

            DestinationLock(const DestinationLock&) = delete;
            DestinationLock& operator=(const DestinationLock&) = delete;

            bool Acquired() const
            {
#ifdef _WIN32
                return m_handle != INVALID_HANDLE_VALUE;
#else
                return m_fd >= 0;
#endif
            }

          private:
#ifdef _WIN32
            HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
            int m_fd = -1;
#endif
        };

        bool PlainDirectory(const fs::path& path)
        {
            std::error_code ec;
            const auto status = fs::symlink_status(path, ec);
            if (ec || !fs::is_directory(status) || fs::is_symlink(status))
            {
                return false;
            }
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
            return true;
#endif
        }

        fs::path PendingPreviousPath(const fs::path& destination)
        {
            fs::path previous = destination;
            previous += ".sparkinstall-previous-pending";
            return previous;
        }

        bool PendingMarkerExists(const fs::path& dest)
        {
            std::error_code ec;
            return fs::is_regular_file(fs::symlink_status(dest / InstallState::PendingFileName(), ec));
        }

        std::string TransactionSuffix()
        {
#ifdef _WIN32
            const long long pid = _getpid();
#else
            const long long pid = getpid();
#endif
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            return std::to_string(pid) + "-" + std::to_string(stamp);
        }

        // A sibling of the destination, so activation is a same-volume rename.
        fs::path MakeStagingPath(const fs::path& dest)
        {
            fs::path leaf = ".";
            leaf += dest.filename();
            leaf += ".sparkinstall-" + TransactionSuffix();
            return dest.parent_path() / leaf;
        }

        bool PathLooksLikeEngineClone(const fs::path& dest)
        {
            std::error_code ec;
            if (!fs::exists(dest, ec) || !fs::is_directory(dest, ec))
            {
                return false;
            }
            return fs::exists(dest / "CMakeLists.txt", ec) && fs::exists(dest / ".git", ec);
        }

        // Configure and build ctx.destination with SparkBuildCore. Returns 0 on
        // success, 6 when configure fails and 7 when the build fails.
        int ConfigureAndBuild(InstallerContext& ctx, const LogSink& log)
        {
            ctx.configManager.config.enginePath = ctx.destination;
            if (ctx.configManager.config.buildPath.empty())
            {
                ctx.configManager.config.buildPath = PathToUtf8(PathFromUtf8(ctx.destination) / "build");
            }

            std::error_code ec;
            fs::create_directories(PathFromUtf8(ctx.configManager.config.buildPath), ec);

            // Every string in the config is UTF-8; ProcessRunner needs the
            // process code page. Commands run from a converted copy, logs show
            // UTF-8. A value the code page cannot hold is refused by name.
            SparkBuild::ConfigManager processConfig = ctx.configManager;
            std::string processDestination;
            const auto convert = [&log](const std::string& value, std::string& converted, const char* what)
            {
                if (ProcessArgFromUtf8(value, converted))
                {
                    return true;
                }
                Emit(log,
                     std::string("error: ") + what + " cannot be passed to CMake in this system's code page: " + value);
                return false;
            };
            bool converted =
                convert(ctx.destination, processDestination, "the install path") &&
                convert(ctx.configManager.config.buildPath, processConfig.config.buildPath, "the build path") &&
                convert(ctx.configManager.config.cmakePath, processConfig.config.cmakePath, "the CMake path") &&
                convert(ctx.configManager.config.cmakePreset, processConfig.config.cmakePreset, "the CMake preset") &&
                convert(ctx.configManager.config.msvcToolset, processConfig.config.msvcToolset, "the MSVC toolset");
            for (auto& option : processConfig.config.options)
            {
                converted = converted && convert(option.cmakeVar, option.cmakeVar, "a CMake option name");
            }
            if (!converted)
            {
                return 6;
            }
            processConfig.config.enginePath = processDestination;

            std::string configureCmd;
            std::string configureDisplay;
            try
            {
                configureCmd = processConfig.BuildCMakeConfigureCommand();
                configureDisplay = ctx.configManager.BuildCMakeConfigureCommand();
                if (ctx.mode == Mode::Update)
                {
                    configureCmd += " -B " + GitRunner::EncodeProcessRunnerArgument(processConfig.config.buildPath) +
                                    " -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON";
                    configureDisplay += " -B " +
                                        GitRunner::EncodeProcessRunnerArgument(ctx.configManager.config.buildPath) +
                                        " -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON";
                }
            }
            catch (const std::invalid_argument& error)
            {
                Emit(log, std::string("error: unsafe CMake configure input: ") + error.what());
                return 6;
            }
            Emit(log, "Configuring: " + configureDisplay);
            {
                SparkBuild::ProcessRunner runner;
                std::string out;
                int rc = runner.RunSync(configureCmd, processDestination, out);
                if (!out.empty())
                {
                    Emit(log, out);
                }
                if (rc != 0)
                {
                    Emit(log, "error: cmake configure exited " + std::to_string(rc));
                    return 6;
                }
            }

            std::string buildCmd;
            std::string buildDisplay;
            try
            {
                buildCmd = processConfig.BuildCMakeBuildCommand();
                buildDisplay = ctx.configManager.BuildCMakeBuildCommand();
            }
            catch (const std::invalid_argument& error)
            {
                Emit(log, std::string("error: unsafe CMake build input: ") + error.what());
                return 7;
            }
            Emit(log, "Building: " + buildDisplay);
            {
                SparkBuild::ProcessRunner runner;
                std::string out;
                int rc = runner.RunSync(buildCmd, processDestination, out);
                if (!out.empty())
                {
                    Emit(log, out);
                }
                if (rc != 0)
                {
                    Emit(log, "error: cmake build exited " + std::to_string(rc));
                    return 7;
                }
            }
            return 0;
        }

        bool PreserveIgnoredFiles(const GitRunner& git, const fs::path& destination, const fs::path& staging,
                                  const fs::path& relativeBuild, const LogSink& log)
        {
            std::vector<std::string> paths;
            if (!git.IgnoredFiles(PathToUtf8(destination), paths, log))
            {
                return false;
            }
            for (const std::string& name : paths)
            {
                // git prints paths as UTF-8.
                const fs::path relative = PathFromUtf8(name);
                if (relative.empty() || relative.has_root_path() || relative.lexically_normal() != relative ||
                    *relative.begin() == "..")
                {
                    Emit(log, "error: unsafe ignored-file path; preserving the existing install");
                    return false;
                }
                const auto first = *relative.begin();
                // The build directory itself or anything below it.
                const bool underBuild =
                    std::mismatch(relativeBuild.begin(), relativeBuild.end(), relative.begin(), relative.end()).first ==
                    relativeBuild.end();
                if (first == ".git" || first == "build" || underBuild || relative == InstallState::FileName() ||
                    relative == InstallState::FileName() + ".tmp" || relative == InstallState::PendingFileName() ||
                    relative == InstallState::RepairRequiredFileName())
                {
                    continue;
                }
                // Do not follow a user link or a link planted by the new ref.
                // A collision is a failed update, never an overwrite of data.
                fs::path from = destination;
                fs::path to = staging;
                for (const auto& component : relative.parent_path())
                {
                    from /= component;
                    to /= component;
                    if (!PlainDirectory(from))
                    {
                        Emit(log, "error: ignored-file parent is linked or unreadable: " + PathToUtf8(from));
                        return false;
                    }
                    std::error_code ec;
                    fs::create_directory(to, ec);
                    if (ec || !PlainDirectory(to))
                    {
                        Emit(log, "error: ignored-file destination is linked or unavailable: " + PathToUtf8(to));
                        return false;
                    }
                }
                from /= relative.filename();
                to /= relative.filename();
                std::error_code ec;
                const auto status = fs::symlink_status(from, ec);
                if (ec || !fs::is_regular_file(status))
                {
                    Emit(log, "error: ignored user file is not a regular file: " + PathToUtf8(from));
                    return false;
                }
#ifdef _WIN32
                const DWORD attributes = GetFileAttributesW(from.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    Emit(log, "error: ignored user file is a reparse point: " + PathToUtf8(from));
                    return false;
                }
#endif
                const auto targetStatus = fs::symlink_status(to, ec);
                if (fs::exists(targetStatus) || (ec && ec != std::errc::no_such_file_or_directory))
                {
                    Emit(log, "error: ignored user file collides with the staged tree: " + PathToUtf8(to));
                    return false;
                }
                if (!fs::copy_file(from, to, fs::copy_options::none, ec))
                {
                    Emit(log, "error: could not preserve ignored user file " + PathToUtf8(from) + ": " + ec.message());
                    return false;
                }
            }
            return true;
        }

        int UpdateInStaging(InstallerContext& ctx, const GitRunner& git, const fs::path& dest)
        {
            const std::string previousCommit = git.HeadCommit(PathToUtf8(dest));
            if (previousCommit.empty())
            {
                Emit(ctx.log, "error: could not determine the current install commit; refusing update");
                return 5;
            }
            InstallState recorded;
            if (InstallState::Load(PathToUtf8(dest), recorded) && recorded.commit != previousCommit)
            {
                Emit(ctx.log, "error: Interrupted update detected: HEAD differs from the recorded build; "
                              "preserving the existing tree for explicit repair");
                return 5;
            }
            if (!git.WorkingTreeClean(PathToUtf8(dest), ctx.log))
            {
                Emit(ctx.log, "error: existing install has local changes; refusing update");
                return 5;
            }

            // An absolute configured build path must still belong to this
            // install. Rebase it into staging so CMake cannot overwrite the
            // existing binaries, including when a preset supplies binaryDir.
            const fs::path configured = ctx.configManager.config.buildPath.empty()
                                            ? fs::path("build")
                                            : PathFromUtf8(ctx.configManager.config.buildPath);
            fs::path relativeBuild =
                (configured.is_absolute() ? configured.lexically_relative(dest) : configured).lexically_normal();
            if (!relativeBuild.has_filename() && relativeBuild.has_relative_path())
            {
                relativeBuild = relativeBuild.parent_path();
            }
            if (relativeBuild.empty() || relativeBuild == "." || relativeBuild.has_root_path() ||
                *relativeBuild.begin() == "..")
            {
                Emit(ctx.log, "error: update build directory must be below the install destination");
                return 6;
            }

            const fs::path staging = MakeStagingPath(dest);
            std::error_code ec;
            if (fs::exists(fs::symlink_status(staging, ec)) || (ec && ec != std::errc::no_such_file_or_directory))
            {
                Emit(ctx.log, "error: staging path is not available: " + PathToUtf8(staging));
                return 5;
            }
            const auto fail = [&](const std::string& reason, int code)
            {
                Emit(ctx.log, "error: " + reason + "; working install was not changed");
                // Only this run's unique clone is discarded. Never delete the
                // previous tree, even if activation/recovery later fails.
                std::error_code ignored;
                fs::remove_all(staging, ignored);
                return code;
            };
            Emit(ctx.log, "Staging update " + ctx.ref + " at " + PathToUtf8(staging));
            if (!git.Clone(ctx.repoUrl, ctx.ref, PathToUtf8(staging), ctx.log) ||
                !git.Fetch(PathToUtf8(staging), ctx.log) || !git.CheckoutRef(ctx.ref, PathToUtf8(staging), ctx.log))
            {
                return fail("git clone/fetch/checkout failed", 5);
            }
            if (!ctx.skipSubmoduleUpdate && !git.UpdateSubmodules(PathToUtf8(staging), ctx.log))
            {
                return fail("submodule update failed", 5);
            }
            const std::string stagedCommit = git.HeadCommit(PathToUtf8(staging));
            if (stagedCommit.empty())
            {
                return fail("could not determine the staged commit", 5);
            }
            if (ctx.skipBuild)
            {
                Emit(ctx.log, "skipBuild set; unverified update kept at " + PathToUtf8(staging) +
                                  "; working install was not changed");
                return 0;
            }

            InstallerContext staged = ctx;
            staged.destination = PathToUtf8(staging);
            staged.configManager.config.enginePath = PathToUtf8(staging);
            staged.configManager.config.buildPath = PathToUtf8(staging / relativeBuild);
            if (const int result = ConfigureAndBuild(staged, ctx.log); result != 0)
            {
                return fail(result == 6 ? "CMake configure failure" : "CMake build failure", result);
            }
            if (git.HeadCommit(PathToUtf8(staging)) != stagedCommit)
            {
                return fail("could not determine the installed commit or it changed during the build", 8);
            }
            if (!PreserveIgnoredFiles(git, dest, staging, relativeBuild, ctx.log))
            {
                return fail("ignored user-data preservation failed", 8);
            }
            InstallState state;
            state.ref = ctx.ref;
            state.commit = stagedCommit;
            state.generator = SparkBuild::GeneratorToString(staged.configManager.config.generator);
            state.buildType = SparkBuild::BuildTypeToString(staged.configManager.config.buildType);
            state.installerVersion = kInstallerVersion;
            for (const auto& option : staged.configManager.config.options)
            {
                state.options[option.cmakeVar] = option.currentValue;
            }
            InstallState verified;
            if (!state.Save(PathToUtf8(staging), PathToUtf8(dest)) ||
                !InstallState::Load(PathToUtf8(staging), verified) || verified.commit != stagedCommit ||
                verified.destination != PathToUtf8(dest))
            {
                return fail("could not write install state file and verify it", 8);
            }
            // Recheck before the only operation that can move the live tree.
            if (git.HeadCommit(PathToUtf8(dest)) != previousCommit || !git.WorkingTreeClean(PathToUtf8(dest), ctx.log))
            {
                return fail("existing install changed while the update was building", 5);
            }
            std::string error;
            if (!detail::ActivateStagedTree(staging, dest, error, PendingPreviousPath(dest)))
            {
                Emit(ctx.log, "error: " + error + "; staged build kept at " + PathToUtf8(staging));
                return 9;
            }
            // This only archives the retained previous tree. A failure leaves
            // it at the fixed recovery path for the next run; it cannot undo
            // the already successful activation or destroy either tree.
            if (!detail::RecoverPreviousTree(dest, error))
            {
                Emit(ctx.log, "warning: " + error);
            }
            ctx.configManager.config.enginePath = PathToUtf8(dest);
            ctx.configManager.config.buildPath = PathToUtf8(dest / relativeBuild);
            Emit(ctx.log, "Done. Engine built at: " + ctx.configManager.config.buildPath);
            return 0;
        }
    } // namespace

    int Installer::Run(InstallerContext& ctx)
    {
        if (ctx.destination.empty())
        {
            Emit(ctx.log, "error: destination is empty");
            return 2;
        }

        // Resolve the destination to an absolute, normalised path up front.
        // Everything downstream (cmake -S, InstallState, log messages) uses
        // this absolute form — otherwise a relative --dest combined with a
        // CWD-change during configure produces a "<dest>/<dest>" source path.
        // Every frontend hands over UTF-8; anything else would name a
        // different directory than the one the user chose.
        if (!IsValidUtf8(ctx.destination))
        {
            Emit(ctx.log, "error: destination is not valid UTF-8");
            return 2;
        }
        std::error_code absErr;
        fs::path dest = fs::absolute(PathFromUtf8(ctx.destination), absErr).lexically_normal();
        if (absErr)
        {
            Emit(ctx.log, "error: could not resolve destination to absolute path: " + absErr.message());
            return 2;
        }
        // A trailing separator ("C:\SparkEngine\") survives lexically_normal and
        // leaves filename() empty, which would put the staging clone inside the
        // destination instead of beside it. Drop it; a bare root keeps its own.
        if (!dest.has_filename() && dest.has_relative_path())
        {
            dest = dest.parent_path();
        }
        ctx.destination = PathToUtf8(dest);

        // git and CMake are started through CreateProcessA, so they can only
        // be pointed at a path the process code page represents. Refuse here,
        // before the lock and preflight, rather than fail later with a
        // misleading git or CMake error.
        if (std::string processDestination; !ProcessArgFromUtf8(ctx.destination, processDestination))
        {
            Emit(ctx.log, "error: destination " + ctx.destination +
                              " contains characters this system's code page cannot represent; git and CMake "
                              "cannot be run there. Choose a path without those characters.");
            return 2;
        }

        // --- Mode detection ------------------------------------------------
        // A pending marker without install state is a clone this installer
        // activated but never finished building; it is never updated.
        if (InstallState::Exists(ctx.destination))
        {
            ctx.mode = Mode::Update;
        }
        else if (PendingMarkerExists(dest))
        {
            ctx.mode = Mode::ResumeInstall;
        }
        else if (PathLooksLikeEngineClone(dest))
        {
            ctx.mode = Mode::Update; // existing clone without our marker
        }
        else
        {
            ctx.mode = Mode::Install;
        }

        Emit(ctx.log, ctx.mode == Mode::Install         ? "Mode: Install"
                      : ctx.mode == Mode::ResumeInstall ? "Mode: Resume install"
                                                        : "Mode: Update");

        // --- Preflight ----------------------------------------------------
        // Ordinary runs pass full preflight before mutation. Recovery checks
        // path safety first so new-build requirements cannot prevent restoring
        // the old tree; the full preflight still runs before any new update.
        std::error_code recoveryStatusError;
        const bool recoveryPending = fs::exists(fs::symlink_status(PendingPreviousPath(dest), recoveryStatusError));
        const auto preflightFailures = recoveryPending ? Preflight::RunRecovery(ctx) : Preflight::Run(ctx);
        if (!preflightFailures.empty())
        {
            for (const auto& failure : preflightFailures)
            {
                Emit(ctx.log, "preflight " + failure.code + ": " + failure.message);
            }
            Emit(ctx.log, "error: preflight failed; nothing was changed");
            return 10;
        }

        std::error_code parentError;
        fs::create_directories(dest.parent_path(), parentError);
        if (parentError)
        {
            Emit(ctx.log, "error: could not create the destination parent: " + parentError.message());
            return 4;
        }
        DestinationLock transaction(dest);
        if (!transaction.Acquired())
        {
            Emit(ctx.log, "error: destination is locked by another installer or the lock file is unsafe");
            return 4;
        }
        std::string recoveryError;
        if (!detail::RecoverPreviousTree(dest, recoveryError))
        {
            Emit(ctx.log, "error: " + recoveryError);
            return 9;
        }
        // A previous process may have stopped between moving the live tree
        // aside and installing its replacement. Recovery restores that tree;
        // recheck its marker and disk budget before attempting an update.
        const bool recoveredUpdate = ctx.mode == Mode::Install && PathLooksLikeEngineClone(dest);
        if (recoveredUpdate)
        {
            ctx.mode = Mode::Update;
        }
        if (recoveryPending || recoveredUpdate)
        {
            for (const auto& failure : Preflight::Run(ctx))
            {
                Emit(ctx.log, "preflight " + failure.code + ": " + failure.message);
                return 10;
            }
        }

        // --- Git ----------------------------------------------------------
        auto gitBoot = GitBootstrap::Ensure(ctx.log);
        if (!gitBoot.ok)
        {
            Emit(ctx.log, "error: " + gitBoot.diagnosticMessage);
            return 3;
        }
        GitRunner git(gitBoot.gitExe);

        if (ctx.mode == Mode::Update)
        {
            return UpdateInStaging(ctx, git, dest);
        }

        const auto unfinishedInstall = [&](const std::string& reason, int failureCode)
        {
            Emit(ctx.log, "Install not finished (" + reason + "); " + ctx.destination +
                              " stays marked pending and the next run resumes it.");
            return failureCode;
        };

        // --- Install mode: clone into staging, then activate ---------------
        if (ctx.mode == Mode::Install)
        {
            std::error_code ec;
            fs::create_directories(dest.parent_path(), ec);

            if (fs::exists(dest, ec) && !fs::is_empty(dest, ec))
            {
                Emit(ctx.log, "error: destination exists and is not empty: " + ctx.destination);
                return 4;
            }

            // The staging path is new to this run (checked below), so removing
            // it on failure can only delete what this run's clone wrote.
            const fs::path staging = MakeStagingPath(dest);
            if (fs::exists(fs::symlink_status(staging, ec)))
            {
                Emit(ctx.log, "error: staging path already exists: " + PathToUtf8(staging));
                return 5;
            }
            const auto discardStaging = [&staging]
            {
                std::error_code ignored;
                fs::remove_all(staging, ignored);
            };

            Emit(ctx.log, "Cloning " + ctx.repoUrl + " @ " + ctx.ref + " -> " + PathToUtf8(staging));
            if (!git.Clone(ctx.repoUrl, ctx.ref, PathToUtf8(staging), ctx.log))
            {
                Emit(ctx.log, "error: git clone failed; " + ctx.destination + " was not changed");
                discardStaging();
                return 5;
            }
            const std::string stagedCommit = git.HeadCommit(PathToUtf8(staging));
            if (stagedCommit.empty())
            {
                Emit(ctx.log, "error: could not determine the cloned commit; " + ctx.destination + " was not changed");
                discardStaging();
                return 5;
            }
            if (!InstallState::WritePendingMarker(PathToUtf8(staging), ctx.ref, stagedCommit))
            {
                Emit(ctx.log,
                     "error: could not write the pending-install marker; " + ctx.destination + " was not changed");
                discardStaging();
                return 8;
            }

            std::string activationError;
            if (!detail::ActivateStagedTree(staging, dest, activationError))
            {
                Emit(ctx.log, "error: " + activationError + "; the clone is kept at " + PathToUtf8(staging));
                return 4;
            }
            Emit(ctx.log, "Activated " + stagedCommit + " at " + ctx.destination);
        }
        else if (ctx.mode == Mode::ResumeInstall)
        {
            std::string pendingRef;
            std::string pendingCommit;
            if (!InstallState::ReadPendingMarker(PathToUtf8(dest), pendingRef, pendingCommit))
            {
                Emit(ctx.log, "error: the pending-install marker in " + ctx.destination +
                                  " cannot be read; remove the destination and install again");
                return 8;
            }
            Emit(ctx.log, "Resuming the unfinished install of " + pendingRef + " @ " + pendingCommit);
            if (pendingRef != ctx.ref)
            {
                Emit(ctx.log, "error: " + ctx.destination + " holds an unfinished install of ref " + pendingRef +
                                  ", not " + ctx.ref + "; rerun with --ref " + pendingRef +
                                  " or remove the destination");
                return 4;
            }
            if (git.HeadCommit(ctx.destination) != pendingCommit)
            {
                Emit(ctx.log, "error: " + ctx.destination + " is no longer at the cloned commit " + pendingCommit +
                                  "; refusing to resume");
                return 5;
            }
        }
        if (ctx.skipBuild)
        {
            Emit(ctx.log, "skipBuild set — stopping before CMake configure.");
            return 0;
        }

        // --- Configure + build via SparkBuildCore -------------------------
        if (const int buildResult = ConfigureAndBuild(ctx, ctx.log); buildResult != 0)
        {
            return unfinishedInstall(buildResult == 6 ? "CMake configure failure" : "CMake build failure", buildResult);
        }

        // --- Persist state ------------------------------------------------
        InstallState state;
        state.ref = ctx.ref;
        state.commit = git.HeadCommit(ctx.destination);
        if (state.commit.empty())
        {
            Emit(ctx.log, "error: could not determine the installed commit; refusing to report success");
            return unfinishedInstall("installed commit verification failure", 8);
        }
        state.generator = SparkBuild::GeneratorToString(ctx.configManager.config.generator);
        state.buildType = SparkBuild::BuildTypeToString(ctx.configManager.config.buildType);
        state.installerVersion = kInstallerVersion;
        for (const auto& opt : ctx.configManager.config.options)
        {
            state.options[opt.cmakeVar] = opt.currentValue;
        }
        if (!state.Save(ctx.destination))
        {
            Emit(ctx.log, "error: could not write install state file");
            return unfinishedInstall("install-state persistence failure", 8);
        }

        // The recorded build now matches the tree, so an earlier failed
        // rollback no longer applies.
        std::error_code repairMarkerError;
        fs::remove(dest / InstallState::RepairRequiredFileName(), repairMarkerError);
        if (repairMarkerError)
        {
            Emit(ctx.log, "error: could not remove the repair-required marker: " + repairMarkerError.message());
            return 8;
        }
        // Removing the pending marker is a fresh install's commit point.
        std::error_code pendingMarkerError;
        fs::remove(dest / InstallState::PendingFileName(), pendingMarkerError);
        if (pendingMarkerError)
        {
            Emit(ctx.log, "error: could not remove the pending-install marker: " + pendingMarkerError.message());
            return 8;
        }

        Emit(ctx.log, "Done. Engine built at: " + ctx.configManager.config.buildPath);
        return 0;
    }

    bool detail::RecoverPreviousTree(const fs::path& destination, std::string& error)
    {
        const fs::path previous = PendingPreviousPath(destination);
        std::error_code ec;
        const auto previousStatus = fs::symlink_status(previous, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(previousStatus)))
        {
            return true;
        }
        if (ec || !PlainDirectory(previous) || !PathLooksLikeEngineClone(previous))
        {
            error = "unsafe or unreadable previous install at " + PathToUtf8(previous) + "; preserved for repair";
            return false;
        }
        const auto destinationStatus = fs::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
        {
            error = "cannot inspect the install during recovery: " + ec.message();
            return false;
        }
        if (!fs::exists(destinationStatus))
        {
            fs::rename(previous, destination, ec);
            if (ec)
            {
                error = "could not restore " + PathToUtf8(previous) + ": " + ec.message();
                return false;
            }
            return true;
        }
        InstallState active;
        if (!PlainDirectory(destination) || !InstallState::Load(PathToUtf8(destination), active) ||
            active.destination != PathToUtf8(destination))
        {
            error = "activation is ambiguous; preserve " + PathToUtf8(previous) + " for explicit recovery";
            return false;
        }
        // A successful activation keeps the whole prior tree, including its
        // build outputs and ignored user data. Never garbage-collect it here.
        fs::path archived = destination;
        archived += ".sparkinstall-previous-" + TransactionSuffix();
        const auto archiveStatus = fs::symlink_status(archived, ec);
        if (fs::exists(archiveStatus) || (ec && ec != std::errc::no_such_file_or_directory))
        {
            error = "previous install archive path is unavailable: " + PathToUtf8(archived);
            return false;
        }
        fs::rename(previous, archived, ec);
        if (ec)
        {
            error = "previous install retained at " + PathToUtf8(previous) + ": " + ec.message();
            return false;
        }
        return true;
    }

    bool detail::ActivateStagedTree(const fs::path& staging, const fs::path& destination, std::string& error,
                                    const fs::path& previous)
    {
        std::error_code ec;
        if (!PlainDirectory(staging))
        {
            error = "staging tree is not an ordinary directory: " + PathToUtf8(staging);
            return false;
        }
        if (!previous.empty())
        {
            InstallState staged;
            if (staging.parent_path() != destination.parent_path() ||
                previous.parent_path() != destination.parent_path() || staging == destination ||
                previous == destination || previous == staging || !PlainDirectory(destination) ||
                !InstallState::Load(PathToUtf8(staging), staged) || staged.destination != PathToUtf8(destination))
            {
                error = "update activation requires verified sibling trees and a separate previous path";
                return false;
            }
            const auto previousStatus = fs::symlink_status(previous, ec);
            if (fs::exists(previousStatus) || (ec && ec != std::errc::no_such_file_or_directory))
            {
                error = "previous install path is unavailable: " + PathToUtf8(previous);
                return false;
            }
            fs::rename(destination, previous, ec);
            if (ec)
            {
                error = "could not retain the previous install: " + ec.message();
                return false;
            }
            fs::rename(staging, destination, ec);
            if (ec)
            {
                error = "could not activate the staged update: " + ec.message();
                std::error_code restoreError;
                fs::rename(previous, destination, restoreError);
                if (restoreError)
                {
                    error += "; restore failed: " + restoreError.message() + "; previous install kept at " +
                             PathToUtf8(previous);
                }
                return false;
            }
            return true;
        }
        const fs::file_status status = fs::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
        {
            error = "could not inspect destination: " + ec.message();
            return false;
        }
        const bool existed = fs::exists(status);
        if (existed)
        {
            if (fs::is_symlink(status) || !fs::is_directory(status))
            {
                error = PathToUtf8(destination) + " exists and is not a directory";
                return false;
            }
            if (!fs::is_empty(destination, ec) || ec)
            {
                error = PathToUtf8(destination) + " is no longer empty";
                return false;
            }
            // remove() deletes only an empty directory, so an entry created
            // after the check makes it fail rather than delete anything.
            if (!fs::remove(destination, ec) || ec)
            {
                error = "could not replace the empty destination " + PathToUtf8(destination) + ": " + ec.message();
                return false;
            }
        }
        // rename() never replaces a non-empty directory on any platform.
        fs::rename(staging, destination, ec);
        if (ec)
        {
            error = "could not move the staged clone to " + PathToUtf8(destination) + ": " + ec.message();
            if (existed)
            {
                std::error_code ignored;
                fs::create_directory(destination, ignored);
            }
            return false;
        }
        return true;
    }
} // namespace SparkInstaller
