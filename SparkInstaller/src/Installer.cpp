#include "Installer.h"

#include "Config.h"
#include "Downloader.h"
#include "GitBootstrap.h"
#include "GitRunner.h"
#include "InstallState.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>

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

        bool PathLooksLikeEngineClone(const fs::path& dest)
        {
            std::error_code ec;
            if (!fs::exists(dest, ec) || !fs::is_directory(dest, ec))
                return false;
            return fs::exists(dest / "CMakeLists.txt", ec) && fs::exists(dest / ".git", ec);
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
        ctx.destination = dest.string();

        // --- Mode detection ------------------------------------------------
        if (InstallState::Exists(ctx.destination))
            ctx.mode = Mode::Update;
        else if (PathLooksLikeEngineClone(dest))
            ctx.mode = Mode::Update; // existing clone without our marker
        else
            ctx.mode = Mode::Install;

        Emit(ctx.log, ctx.mode == Mode::Install ? "Mode: Install" : "Mode: Update");

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
        }

        const auto rollbackUpdate = [&](const std::string& reason)
        {
            if (!isUpdate)
                return true;

            Emit(ctx.log, "Update failed (" + reason + "); rolling back update to " + previousCommit);
            if (!git.CheckoutCommit(previousCommit, ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: update rollback failed; the install requires manual repair");
                return false;
            }
            if (!ctx.skipSubmoduleUpdate && !git.UpdateSubmodules(ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: update rollback restored the commit but not its submodules");
                return false;
            }
            Emit(ctx.log, "Update rollback complete.");
            return true;
        };

        // --- Install mode: clone ------------------------------------------
        if (ctx.mode == Mode::Install)
        {
            std::error_code ec;
            fs::create_directories(dest.parent_path(), ec);

            if (fs::exists(dest, ec) && !fs::is_empty(dest, ec))
            {
                Emit(ctx.log, "error: destination exists and is not empty: " + ctx.destination);
                return 4;
            }

            Emit(ctx.log, "Cloning " + ctx.repoUrl + " @ " + ctx.ref + " -> " + ctx.destination);
            if (!git.Clone(ctx.repoUrl, ctx.ref, ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: git clone failed");
                return 5;
            }
        }
        else
        {
            Emit(ctx.log, "Updating existing install at " + ctx.destination);
            if (!git.Fetch(ctx.destination, ctx.log) || !git.CheckoutRef(ctx.ref, ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: git fetch/checkout failed");
                (void)rollbackUpdate("git fetch/checkout failure");
                return 5;
            }
            if (!ctx.skipSubmoduleUpdate && !git.UpdateSubmodules(ctx.destination, ctx.log))
            {
                Emit(ctx.log, "error: submodule update failed");
                (void)rollbackUpdate("submodule update failure");
                return 5;
            }
        }

        if (ctx.skipBuild)
        {
            Emit(ctx.log, "skipBuild set — stopping before CMake configure.");
            return 0;
        }

        // --- Configure + build via SparkBuildCore -------------------------
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
            Emit(ctx.log, std::string("error: unsafe CMake configure input: ") + error.what());
            (void)rollbackUpdate("unsafe CMake configure input");
            return 6;
        }
        Emit(ctx.log, "Configuring: " + configureCmd);
        {
            SparkBuild::ProcessRunner runner;
            std::string out;
            int rc = runner.RunSync(configureCmd, ctx.destination, out);
            if (!out.empty())
                Emit(ctx.log, out);
            if (rc != 0)
            {
                Emit(ctx.log, "error: cmake configure exited " + std::to_string(rc));
                (void)rollbackUpdate("CMake configure failure");
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
            Emit(ctx.log, std::string("error: unsafe CMake build input: ") + error.what());
            (void)rollbackUpdate("unsafe CMake build input");
            return 7;
        }
        Emit(ctx.log, "Building: " + buildCmd);
        {
            SparkBuild::ProcessRunner runner;
            std::string out;
            int rc = runner.RunSync(buildCmd, ctx.destination, out);
            if (!out.empty())
                Emit(ctx.log, out);
            if (rc != 0)
            {
                Emit(ctx.log, "error: cmake build exited " + std::to_string(rc));
                (void)rollbackUpdate("CMake build failure");
                return 7;
            }
        }

        // --- Persist state ------------------------------------------------
        InstallState state;
        state.ref = ctx.ref;
        state.commit = git.HeadCommit(ctx.destination);
        state.generator = SparkBuild::GeneratorToString(ctx.configManager.config.generator);
        state.buildType = SparkBuild::BuildTypeToString(ctx.configManager.config.buildType);
        state.installerVersion = kInstallerVersion;
        for (const auto& opt : ctx.configManager.config.options)
            state.options[opt.cmakeVar] = opt.currentValue;
        if (!state.Save(ctx.destination))
        {
            Emit(ctx.log, "error: could not write install state file");
            (void)rollbackUpdate("install-state persistence failure");
            return 8;
        }

        Emit(ctx.log, "Done. Engine built at: " + ctx.configManager.config.buildPath);
        return 0;
    }
} // namespace SparkInstaller
