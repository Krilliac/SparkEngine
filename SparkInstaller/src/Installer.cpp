#include "Installer.h"

#include "Config.h"
#include "Downloader.h"
#include "GitBootstrap.h"
#include "GitRunner.h"
#include "InstallState.h"
#include "InstallerPreflight.h"
#include "ProcessRunner.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
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
                log(msg);
        }

        bool PendingMarkerExists(const fs::path& dest)
        {
            std::error_code ec;
            return fs::is_regular_file(fs::symlink_status(dest / InstallState::PendingFileName(), ec));
        }

        // The pending marker holds the ref and the exact commit that was cloned,
        // one "key=value" line each.
        bool WritePendingMarker(const fs::path& tree, const std::string& ref, const std::string& commit)
        {
            std::ofstream out(tree / InstallState::PendingFileName(), std::ios::binary | std::ios::trunc);
            out << "ref=" << ref << '\n' << "commit=" << commit << '\n';
            out.close();
            return static_cast<bool>(out);
        }

        bool ReadPendingMarker(const fs::path& tree, std::string& ref, std::string& commit)
        {
            constexpr std::uintmax_t kMaxPendingMarkerBytes = 4096;
            const fs::path marker = tree / InstallState::PendingFileName();
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(marker, ec);
            if (ec || size > kMaxPendingMarkerBytes)
                return false;
            std::ifstream in(marker, std::ios::binary);
            std::string line;
            ref.clear();
            commit.clear();
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("ref=", 0) == 0)
                    ref = line.substr(4);
                else if (line.rfind("commit=", 0) == 0)
                    commit = line.substr(7);
            }
            return !ref.empty() && !commit.empty();
        }

        // A sibling of the destination, so activation is a same-volume rename.
        fs::path MakeStagingPath(const fs::path& dest)
        {
#ifdef _WIN32
            const long long pid = _getpid();
#else
            const long long pid = getpid();
#endif
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            return dest.parent_path() / ("." + dest.filename().string() + ".sparkinstall-" + std::to_string(pid) + "-" +
                                         std::to_string(stamp));
        }

        bool PathLooksLikeEngineClone(const fs::path& dest)
        {
            std::error_code ec;
            if (!fs::exists(dest, ec) || !fs::is_directory(dest, ec))
                return false;
            return fs::exists(dest / "CMakeLists.txt", ec) && fs::exists(dest / ".git", ec);
        }

        // Configure and build ctx.destination with SparkBuildCore. Returns 0 on
        // success, 6 when configure fails and 7 when the build fails.
        int ConfigureAndBuild(InstallerContext& ctx, const LogSink& log)
        {
            ctx.configManager.config.enginePath = ctx.destination;
            if (ctx.configManager.config.buildPath.empty())
                ctx.configManager.config.buildPath = (fs::path(ctx.destination) / "build").string();

            std::error_code ec;
            fs::create_directories(ctx.configManager.config.buildPath, ec);

            std::string configureCmd;
            try
            {
                configureCmd = ctx.configManager.BuildCMakeConfigureCommand();
            }
            catch (const std::invalid_argument& error)
            {
                Emit(log, std::string("error: unsafe CMake configure input: ") + error.what());
                return 6;
            }
            Emit(log, "Configuring: " + configureCmd);
            {
                SparkBuild::ProcessRunner runner;
                std::string out;
                int rc = runner.RunSync(configureCmd, ctx.destination, out);
                if (!out.empty())
                    Emit(log, out);
                if (rc != 0)
                {
                    Emit(log, "error: cmake configure exited " + std::to_string(rc));
                    return 6;
                }
            }

            std::string buildCmd;
            try
            {
                buildCmd = ctx.configManager.BuildCMakeBuildCommand();
            }
            catch (const std::invalid_argument& error)
            {
                Emit(log, std::string("error: unsafe CMake build input: ") + error.what());
                return 7;
            }
            Emit(log, "Building: " + buildCmd);
            {
                SparkBuild::ProcessRunner runner;
                std::string out;
                int rc = runner.RunSync(buildCmd, ctx.destination, out);
                if (!out.empty())
                    Emit(log, out);
                if (rc != 0)
                {
                    Emit(log, "error: cmake build exited " + std::to_string(rc));
                    return 7;
                }
            }
            return 0;
        }

        // Records that the tree at destination no longer matches a verified
        // build. Best effort: exit code 9 already reports the condition.
        void WriteRepairRequiredMarker(const fs::path& destination, const std::string& reason, const LogSink& log)
        {
            const fs::path marker = destination / InstallState::RepairRequiredFileName();
            std::ofstream out(marker, std::ios::binary | std::ios::trunc);
            out << reason << '\n';
            out.close();
            if (!out)
                Emit(log, "error: could not write repair-required marker " + marker.string());
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
        std::error_code absErr;
        fs::path dest = fs::absolute(fs::path(ctx.destination), absErr).lexically_normal();
        if (absErr)
        {
            Emit(ctx.log, "error: could not resolve destination to absolute path: " + absErr.message());
            return 2;
        }
        // A trailing separator ("C:\SparkEngine\") survives lexically_normal and
        // leaves filename() empty, which would put the staging clone inside the
        // destination instead of beside it. Drop it; a bare root keeps its own.
        if (!dest.has_filename() && dest.has_relative_path())
            dest = dest.parent_path();
        ctx.destination = dest.string();

        // --- Mode detection ------------------------------------------------
        // A pending marker without install state is a clone this installer
        // activated but never finished building; it is never updated.
        if (InstallState::Exists(ctx.destination))
            ctx.mode = Mode::Update;
        else if (PendingMarkerExists(dest))
            ctx.mode = Mode::ResumeInstall;
        else if (PathLooksLikeEngineClone(dest))
            ctx.mode = Mode::Update; // existing clone without our marker
        else
            ctx.mode = Mode::Install;

        Emit(ctx.log, ctx.mode == Mode::Install         ? "Mode: Install"
                      : ctx.mode == Mode::ResumeInstall ? "Mode: Resume install"
                                                        : "Mode: Update");

        // --- Preflight ----------------------------------------------------
        // Every check runs before git bootstrap, directory creation, clone or
        // fetch, so a refused run leaves the destination exactly as found.
        const auto preflightFailures = Preflight::Run(ctx);
        if (!preflightFailures.empty())
        {
            for (const auto& failure : preflightFailures)
                Emit(ctx.log, "preflight " + failure.code + ": " + failure.message);
            Emit(ctx.log, "error: preflight failed; nothing was changed");
            return 10;
        }

        // --- Git ----------------------------------------------------------
        auto gitBoot = GitBootstrap::Ensure(ctx.log);
        if (!gitBoot.ok)
        {
            Emit(ctx.log, "error: " + gitBoot.diagnosticMessage);
            return 3;
        }
        GitRunner git(gitBoot.gitExe);

        // Updates mutate the live checkout before the new build has been
        // proven. Capture the exact working commit first so every later
        // failure has a bounded, non-forcing rollback target.
        const bool isUpdate = ctx.mode == Mode::Update;
        std::string previousCommit;
        if (isUpdate)
        {
            previousCommit = git.HeadCommit(ctx.destination);
            if (previousCommit.empty())
            {
                Emit(ctx.log, "error: could not determine the current install commit; refusing update");
                return 5;
            }
            // A recorded commit that differs from HEAD means an earlier update
            // was interrupted after checkout and before its build was recorded.
            // Only the recorded commit is a verified build, so it is the
            // rollback target rather than the half-updated HEAD.
            InstallState recorded;
            if (InstallState::Load(ctx.destination, recorded) && recorded.commit != previousCommit)
            {
                Emit(ctx.log, "Interrupted update detected: HEAD " + previousCommit +
                                  " differs from the last verified build " + recorded.commit +
                                  "; rollback will restore " + recorded.commit);
                previousCommit = recorded.commit;
            }
        }

        // Restore the previous commit and rebuild it so the install is again
        // the verified build it was before this run. Returns failureCode when
        // that succeeds (or for a fresh install, which has nothing to restore)
        // and 9 when the install needs repair. Never re-enters itself.
        const auto rollbackUpdate = [&](const std::string& reason, int failureCode)
        {
            if (!isUpdate)
            {
                Emit(ctx.log, "Install not finished (" + reason + "); " + ctx.destination +
                                  " stays marked pending and the next run resumes it.");
                return failureCode;
            }

            Emit(ctx.log, "Update failed (" + reason + "); rolling back update to " + previousCommit);
            std::string repairReason;
            if (!git.CheckoutCommit(previousCommit, ctx.destination, ctx.log))
                repairReason = "could not check out " + previousCommit;
            else if (!ctx.skipSubmoduleUpdate && !git.UpdateSubmodules(ctx.destination, ctx.log))
                repairReason = "restored " + previousCommit + " but not its submodules";
            else if (!ctx.skipBuild)
            {
                const int rebuild = ConfigureAndBuild(ctx, ctx.log);
                if (rebuild != 0)
                    repairReason = "restored " + previousCommit + " but its rebuild exited " + std::to_string(rebuild);
            }
            if (repairReason.empty() && git.HeadCommit(ctx.destination) != previousCommit)
                repairReason = "restored checkout does not report HEAD " + previousCommit;

            if (!repairReason.empty())
            {
                Emit(ctx.log, "error: update rollback failed (" + repairReason + ") after exit " +
                                  std::to_string(failureCode) + "; the install requires repair (exit 9)");
                WriteRepairRequiredMarker(
                    dest, "update to " + ctx.ref + " failed (" + reason + "); rollback failed: " + repairReason,
                    ctx.log);
                return 9;
            }
            Emit(ctx.log, ctx.skipBuild ? "Update rollback complete; restored " + previousCommit
                                        : "Update rollback complete; rebuilt " + previousCommit);
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
                Emit(ctx.log, "error: staging path already exists: " + staging.string());
                return 5;
            }
            const auto discardStaging = [&staging]
            {
                std::error_code ignored;
                fs::remove_all(staging, ignored);
            };

            Emit(ctx.log, "Cloning " + ctx.repoUrl + " @ " + ctx.ref + " -> " + staging.string());
            if (!git.Clone(ctx.repoUrl, ctx.ref, staging.string(), ctx.log))
            {
                Emit(ctx.log, "error: git clone failed; " + ctx.destination + " was not changed");
                discardStaging();
                return 5;
            }
            const std::string stagedCommit = git.HeadCommit(staging.string());
            if (stagedCommit.empty())
            {
                Emit(ctx.log, "error: could not determine the cloned commit; " + ctx.destination + " was not changed");
                discardStaging();
                return 5;
            }
            if (!WritePendingMarker(staging, ctx.ref, stagedCommit))
            {
                Emit(ctx.log,
                     "error: could not write the pending-install marker; " + ctx.destination + " was not changed");
                discardStaging();
                return 8;
            }

            std::string activationError;
            if (!detail::ActivateStagedTree(staging, dest, activationError))
            {
                Emit(ctx.log, "error: " + activationError + "; the clone is kept at " + staging.string());
                return 4;
            }
            Emit(ctx.log, "Activated " + stagedCommit + " at " + ctx.destination);
        }
        else if (ctx.mode == Mode::ResumeInstall)
        {
            std::string pendingRef;
            std::string pendingCommit;
            if (!ReadPendingMarker(dest, pendingRef, pendingCommit))
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
        else
        {
            Emit(ctx.log, "Updating existing install at " + ctx.destination);
            if (!git.WorkingTreeClean(ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: existing install has local changes; refusing update");
                return 5;
            }
            if (!git.Fetch(ctx.destination, ctx.log) || !git.CheckoutRef(ctx.ref, ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: git fetch/checkout failed");
                return rollbackUpdate("git fetch/checkout failure", 5);
            }
            if (!ctx.skipSubmoduleUpdate && !git.UpdateSubmodules(ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: submodule update failed");
                return rollbackUpdate("submodule update failure", 5);
            }
        }

        if (ctx.skipBuild)
        {
            Emit(ctx.log, "skipBuild set — stopping before CMake configure.");
            return 0;
        }

        // --- Configure + build via SparkBuildCore -------------------------
        if (const int buildResult = ConfigureAndBuild(ctx, ctx.log); buildResult != 0)
            return rollbackUpdate(buildResult == 6 ? "CMake configure failure" : "CMake build failure", buildResult);

        // --- Persist state ------------------------------------------------
        InstallState state;
        state.ref = ctx.ref;
        state.commit = git.HeadCommit(ctx.destination);
        if (state.commit.empty())
        {
            Emit(ctx.log, "error: could not determine the installed commit; refusing to report success");
            return rollbackUpdate("installed commit verification failure", 8);
        }
        state.generator = SparkBuild::GeneratorToString(ctx.configManager.config.generator);
        state.buildType = SparkBuild::BuildTypeToString(ctx.configManager.config.buildType);
        state.installerVersion = kInstallerVersion;
        for (const auto& opt : ctx.configManager.config.options)
            state.options[opt.cmakeVar] = opt.currentValue;
        if (!state.Save(ctx.destination))
        {
            Emit(ctx.log, "error: could not write install state file");
            return rollbackUpdate("install-state persistence failure", 8);
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

    bool detail::ActivateStagedTree(const fs::path& staging, const fs::path& destination, std::string& error)
    {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(destination, ec);
        const bool existed = fs::exists(status);
        if (existed)
        {
            if (fs::is_symlink(status) || !fs::is_directory(status))
            {
                error = destination.string() + " exists and is not a directory";
                return false;
            }
            if (!fs::is_empty(destination, ec) || ec)
            {
                error = destination.string() + " is no longer empty";
                return false;
            }
            // remove() deletes only an empty directory, so an entry created
            // after the check makes it fail rather than delete anything.
            if (!fs::remove(destination, ec) || ec)
            {
                error = "could not replace the empty destination " + destination.string() + ": " + ec.message();
                return false;
            }
        }
        // rename() never replaces a non-empty directory on any platform.
        fs::rename(staging, destination, ec);
        if (ec)
        {
            error = "could not move the staged clone to " + destination.string() + ": " + ec.message();
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
