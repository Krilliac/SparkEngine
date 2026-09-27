#include "Installer.h"
#include "InstallState.h"
#include "ProcessRunner.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr std::string_view kFakeHeadCommit = "fake-head-commit";

    int Check(bool condition, const std::string& message)
    {
        if (condition)
            return 0;
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    fs::path MakeTestRoot()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() / ("SparkInstallerTransactionTests_" + std::to_string(stamp));
    }

    int RunFakeTool(int argc, char* argv[])
    {
        if (argc < 2)
            return 97;

        const std::string_view command = argv[1];
        if (const char* logPath = std::getenv("SPARK_FAKE_TOOL_LOG"); logPath && *logPath)
        {
            std::ofstream invocationLog(logPath, std::ios::binary | std::ios::app);
            invocationLog << command << '\n';
            if (!invocationLog)
                return 94;
        }
        if (command == "--version" || command == "fetch" || command == "-S" || command == "status")
            return 0;
        if (command == "checkout")
        {
            std::ofstream lastCheckout("fake-git-last-checkout", std::ios::binary | std::ios::trunc);
            if (argc > 2)
                lastCheckout << argv[argc - 1];
            return lastCheckout ? 0 : 96;
        }
        if (command == "--build")
            return fs::exists("force-build-failure") ? 42 : 0;
        if (command == "show-ref")
            return 1;
        if (command == "rev-parse")
        {
            if (fs::exists("force-final-head-failure"))
            {
                if (fs::exists("fake-git-initial-head-read"))
                    return 95;
                std::ofstream initialHeadRead("fake-git-initial-head-read", std::ios::binary | std::ios::trunc);
                if (!initialHeadRead)
                    return 96;
            }
            std::cout << kFakeHeadCommit << '\n';
            return 0;
        }
        return 98;
    }

    class ScopedPathPrefix
    {
      public:
        explicit ScopedPathPrefix(const fs::path& prefix)
        {
            const char* current = std::getenv("PATH");
            m_hadValue = current != nullptr;
            if (m_hadValue)
                m_previous = current;

            std::string value = prefix.string();
#ifdef _WIN32
            constexpr char separator = ';';
#else
            constexpr char separator = ':';
#endif
            if (!m_previous.empty())
            {
                value.push_back(separator);
                value += m_previous;
            }

#ifdef _WIN32
            m_ok = _putenv_s("PATH", value.c_str()) == 0;
#else
            m_ok = setenv("PATH", value.c_str(), 1) == 0;
#endif
        }

        ~ScopedPathPrefix()
        {
#ifdef _WIN32
            if (m_hadValue)
                (void)_putenv_s("PATH", m_previous.c_str());
            else
                (void)_putenv_s("PATH", "");
#else
            if (m_hadValue)
                (void)setenv("PATH", m_previous.c_str(), 1);
            else
                (void)unsetenv("PATH");
#endif
        }

        bool IsSet() const { return m_ok; }

      private:
        std::string m_previous;
        bool m_hadValue = false;
        bool m_ok = false;
    };

    int RunInstallStatePersistenceFailureTest(const fs::path& executable)
    {
        const fs::path root = MakeTestRoot();
        std::error_code error;
        fs::create_directories(root / "tools", error);
        int failures = Check(!error, "could not create transaction test root");
        if (failures != 0)
            return failures;

        const fs::path fakeGit = root / "tools" /
                                 (
#ifdef _WIN32
                                     "git.exe"
#else
                                     "git"
#endif
                                 );
        fs::copy_file(executable, fakeGit, fs::copy_options::overwrite_existing, error);
        failures += Check(!error, "could not create fake git executable");
#ifndef _WIN32
        if (!error)
        {
            fs::permissions(fakeGit, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, error);
            failures += Check(!error, "could not make fake git executable runnable");
        }
#endif

        const fs::path destination = root / "install";
        fs::create_directories(destination / ".git", error);
        failures += Check(!error, "could not create fake engine checkout");
        std::ofstream cmakeLists(destination / "CMakeLists.txt");
        cmakeLists << "cmake_minimum_required(VERSION 3.25)\n";
        cmakeLists.close();
        failures += Check(static_cast<bool>(cmakeLists), "could not create fake engine CMakeLists");

        // A directory at the marker path makes the final atomic replacement fail
        // after the update and build have completed, without relying on ACLs or a
        // read-only filesystem.
        fs::create_directories(destination / SparkInstaller::InstallState::FileName(), error);
        failures += Check(!error, "could not create marker replacement failure fixture");

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), "could not prepend fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context;
        context.frontend = SparkInstaller::Frontend::Headless;
        context.destination = destination.string();
        context.ref = "Working";
        context.skipSubmoduleUpdate = true;
        context.configManager.config.cmakePath = executable.string();
        context.configManager.config.buildPath = (destination / "build").string();
        context.log = [&log](const std::string& line)
        {
            log += line;
            log.push_back('\n');
        };

        const int result = SparkInstaller::Installer::Run(context);
        failures += Check(result != 0, "installer accepted a failed install-state replacement as success");
        failures += Check(log.find("could not write install state file") != std::string::npos,
                          "installer did not report the install-state persistence failure");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          "installer reported completion after install-state persistence failed");
        failures += Check(!fs::exists(destination / (SparkInstaller::InstallState::FileName() + ".tmp")),
                          "failed install-state replacement left a temporary marker behind");

        fs::remove_all(root, error);
        return failures;
    }

    int RunUpdateBuildFailureRollbackTest(const fs::path& executable)
    {
        const fs::path root = MakeTestRoot();
        std::error_code error;
        fs::create_directories(root / "tools", error);
        int failures = Check(!error, "could not create rollback test root");
        if (failures != 0)
            return failures;

        const fs::path fakeGit = root / "tools" /
                                 (
#ifdef _WIN32
                                     "git.exe"
#else
                                     "git"
#endif
                                 );
        fs::copy_file(executable, fakeGit, fs::copy_options::overwrite_existing, error);
        failures += Check(!error, "could not create rollback fake git executable");
#ifndef _WIN32
        if (!error)
        {
            fs::permissions(fakeGit, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, error);
            failures += Check(!error, "could not make rollback fake git executable runnable");
        }
#endif

        const fs::path destination = root / "install";
        fs::create_directories(destination / ".git", error);
        failures += Check(!error, "could not create rollback engine checkout");
        std::ofstream cmakeLists(destination / "CMakeLists.txt");
        cmakeLists << "cmake_minimum_required(VERSION 3.25)\n";
        cmakeLists.close();
        failures += Check(static_cast<bool>(cmakeLists), "could not create rollback CMakeLists");
        std::ofstream forceFailure(destination / "force-build-failure");
        forceFailure << "fail";
        forceFailure.close();
        failures += Check(static_cast<bool>(forceFailure), "could not create build failure fixture");

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), "could not prepend rollback fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context;
        context.frontend = SparkInstaller::Frontend::Headless;
        context.destination = destination.string();
        context.ref = "Working";
        context.skipSubmoduleUpdate = true;
        context.configManager.config.cmakePath = executable.string();
        context.configManager.config.buildPath = (destination / "build").string();
        context.log = [&log](const std::string& line)
        {
            log += line;
            log.push_back('\n');
        };

        const int result = SparkInstaller::Installer::Run(context);
        failures += Check(result != 0, "installer reported success after update build failure");

        std::ifstream lastCheckout(destination / "fake-git-last-checkout", std::ios::binary);
        std::string restoredCommit;
        std::getline(lastCheckout, restoredCommit);
        failures +=
            Check(restoredCommit == kFakeHeadCommit, "failed update did not restore the previously working commit");
        failures += Check(log.find("rolling back update") != std::string::npos,
                          "failed update did not report that rollback was attempted");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          "installer reported completion after update build failure");

        fs::remove_all(root, error);
        return failures;
    }

    int RunPostBuildHeadCommitFailureTest(const fs::path& executable)
    {
        const fs::path root = MakeTestRoot();
        std::error_code error;
        fs::create_directories(root / "tools", error);
        int failures = Check(!error, "could not create final-head verification test root");
        if (failures != 0)
            return failures;

        const fs::path fakeGit = root / "tools" /
                                 (
#ifdef _WIN32
                                     "git.exe"
#else
                                     "git"
#endif
                                 );
        fs::copy_file(executable, fakeGit, fs::copy_options::overwrite_existing, error);
        failures += Check(!error, "could not create final-head fake git executable");
#ifndef _WIN32
        if (!error)
        {
            fs::permissions(fakeGit, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, error);
            failures += Check(!error, "could not make final-head fake git executable runnable");
        }
#endif

        const fs::path destination = root / "install";
        fs::create_directories(destination / ".git", error);
        failures += Check(!error, "could not create final-head engine checkout");
        std::ofstream cmakeLists(destination / "CMakeLists.txt");
        cmakeLists << "cmake_minimum_required(VERSION 3.25)\n";
        cmakeLists.close();
        failures += Check(static_cast<bool>(cmakeLists), "could not create final-head CMakeLists");

        SparkInstaller::InstallState previousState;
        previousState.ref = "Working";
        previousState.commit = std::string(kFakeHeadCommit);
        previousState.generator = "Ninja";
        previousState.buildType = "Release";
        previousState.installerVersion = "1.0.0";
        failures += Check(previousState.Save(destination.string()), "could not create prior valid install state");
        std::ofstream failFinalHead(destination / "force-final-head-failure");
        failFinalHead << "fail";
        failFinalHead.close();
        failures += Check(static_cast<bool>(failFinalHead), "could not create final-head failure fixture");

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), "could not prepend final-head fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context;
        context.frontend = SparkInstaller::Frontend::Headless;
        context.destination = destination.string();
        context.ref = "Working";
        context.skipSubmoduleUpdate = true;
        context.configManager.config.cmakePath = executable.string();
        context.configManager.config.buildPath = (destination / "build").string();
        context.log = [&log](const std::string& line)
        {
            log += line;
            log.push_back('\n');
        };

        const int result = SparkInstaller::Installer::Run(context);
        failures += Check(result != 0, "installer reported success without verifying the installed commit");

        SparkInstaller::InstallState loaded;
        failures += Check(SparkInstaller::InstallState::Load(destination.string(), loaded),
                          "failed commit verification replaced the valid install state with an invalid marker");
        failures += Check(loaded.commit == kFakeHeadCommit,
                          "failed commit verification did not preserve the previous install state");
        failures += Check(log.find("could not determine the installed commit") != std::string::npos,
                          "installer did not report the final commit verification failure");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          "installer reported completion after final commit verification failed");

        fs::remove_all(root, error);
        return failures;
    }

    void SetToolLogEnvironment(const std::string& value)
    {
#ifdef _WIN32
        (void)_putenv_s("SPARK_FAKE_TOOL_LOG", value.c_str());
#else
        if (value.empty())
            (void)unsetenv("SPARK_FAKE_TOOL_LOG");
        else
            (void)setenv("SPARK_FAKE_TOOL_LOG", value.c_str(), 1);
#endif
    }

    // Commands the fake git/cmake tool was invoked with while a preflight case ran.
    std::vector<std::string> ReadToolLog(const fs::path& logPath)
    {
        std::vector<std::string> commands;
        std::ifstream in(logPath, std::ios::binary);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                commands.push_back(line);
        }
        return commands;
    }

    enum class PreflightDestination
    {
        Fresh,
        Link,
        CorruptMarkerUpdate
    };

    // Runs the installer against a fixture that preflight must refuse and
    // proves it returns exit 10 before any git or build command ran. The only
    // tolerated tool invocation is the read-only `cmake --version` probe.
    int RunPreflightRefusalTest(const fs::path& executable, const std::string& name, PreflightDestination kind,
                                const std::string& expectedCode, bool impossibleFreeSpace, bool missingCMake)
    {
        const fs::path root = MakeTestRoot();
        std::error_code error;
        fs::create_directories(root / "tools", error);
        int failures = Check(!error, name + ": could not create preflight test root");
        if (failures != 0)
            return failures;

        const fs::path fakeGit = root / "tools" /
                                 (
#ifdef _WIN32
                                     "git.exe"
#else
                                     "git"
#endif
                                 );
        fs::copy_file(executable, fakeGit, fs::copy_options::overwrite_existing, error);
        failures += Check(!error, name + ": could not create fake git executable");
#ifndef _WIN32
        if (!error)
        {
            fs::permissions(fakeGit, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, error);
            failures += Check(!error, name + ": could not make fake git executable runnable");
        }
#endif

        fs::path destination = root / "install";
        const fs::path linkTarget = root / "real-install";
        if (kind == PreflightDestination::Link)
        {
            fs::create_directories(linkTarget, error);
            failures += Check(!error, name + ": could not create link target");
#ifdef _WIN32
            // A junction needs no symlink privilege and is the Windows link
            // shape std::filesystem does not portably report.
            SparkBuild::ProcessRunner runner;
            std::string output;
            failures +=
                Check(runner.RunSync("mklink /J \"" + destination.string() + "\" \"" + linkTarget.string() + "\"",
                                     root.string(), output) == 0,
                      name + ": could not create junction fixture: " + output);
#else
            fs::create_directory_symlink(linkTarget, destination, error);
            failures += Check(!error, name + ": could not create symlink fixture");
#endif
        }
        else if (kind == PreflightDestination::CorruptMarkerUpdate)
        {
            fs::create_directories(destination / ".git", error);
            failures += Check(!error, name + ": could not create fake engine checkout");
            std::ofstream cmakeLists(destination / "CMakeLists.txt");
            cmakeLists << "cmake_minimum_required(VERSION 3.25)\n";
            cmakeLists.close();
            std::ofstream marker(destination / SparkInstaller::InstallState::FileName(), std::ios::binary);
            marker << "{ \"schema\": 1, truncated";
            marker.close();
            failures += Check(static_cast<bool>(cmakeLists) && static_cast<bool>(marker),
                              name + ": could not create corrupt marker fixture");
        }

        const fs::path toolLog = root / "tool-invocations.log";
        SetToolLogEnvironment(toolLog.string());
        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context;
        context.frontend = SparkInstaller::Frontend::Headless;
        context.destination = destination.string();
        context.ref = "Working";
        context.skipSubmoduleUpdate = true;
        context.configManager.config.cmakePath =
            missingCMake ? (root / "missing" / "cmake-does-not-exist").string() : executable.string();
        context.configManager.config.buildPath = (destination / "build").string();
        // Isolate each case: only the free-space case can fail the space check.
        context.minFreeBytes = impossibleFreeSpace ? std::numeric_limits<std::uintmax_t>::max() : 0;
        context.log = [&log](const std::string& line)
        {
            log += line;
            log.push_back('\n');
        };

        const int result = SparkInstaller::Installer::Run(context);
        SetToolLogEnvironment("");

        failures += Check(result == 10, name + ": preflight refusal did not return exit 10 (got " +
                                            std::to_string(result) + ")\n" + log);
        failures += Check(log.find("error: preflight failed") != std::string::npos,
                          name + ": preflight refusal was not reported");
        failures += Check(log.find("preflight " + expectedCode + ":") != std::string::npos,
                          name + ": expected preflight failure " + expectedCode + " was not reported");
        for (const std::string& command : ReadToolLog(toolLog))
        {
            failures += Check(command == "--version",
                              name + ": tool command `" + command + "` ran before preflight refused the run");
        }
        if (kind == PreflightDestination::Fresh)
        {
            failures += Check(!fs::exists(destination), name + ": refused install created the destination");
        }
        else if (kind == PreflightDestination::Link)
        {
            failures += Check(fs::is_empty(linkTarget, error), name + ": refused install wrote through the link");
            fs::remove(destination, error);
        }

        fs::remove_all(root, error);
        return failures;
    }

    int RunPreflightTests(const fs::path& executable)
    {
        int failures = 0;
        failures += RunPreflightRefusalTest(executable, "insufficient free space", PreflightDestination::Fresh,
                                            "insufficient-free-space",
                                            /*impossibleFreeSpace=*/true, /*missingCMake=*/false);
        failures +=
            RunPreflightRefusalTest(executable, "missing cmake", PreflightDestination::Fresh, "cmake-unavailable",
                                    /*impossibleFreeSpace=*/false, /*missingCMake=*/true);
        failures +=
            RunPreflightRefusalTest(executable, "linked destination", PreflightDestination::Link, "destination-link",
                                    /*impossibleFreeSpace=*/false, /*missingCMake=*/false);
        failures += RunPreflightRefusalTest(executable, "corrupt marker update",
                                            PreflightDestination::CorruptMarkerUpdate, "corrupt-install-marker",
                                            /*impossibleFreeSpace=*/false, /*missingCMake=*/false);
        return failures;
    }
} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1)
        return RunFakeTool(argc, argv);

    const fs::path executable = fs::absolute(argv[0]);
    const int persistenceFailure = RunInstallStatePersistenceFailureTest(executable);
    const int rollbackFailure = RunUpdateBuildFailureRollbackTest(executable);
    const int finalHeadFailure = RunPostBuildHeadCommitFailureTest(executable);
    const int preflightFailure = RunPreflightTests(executable);
    return persistenceFailure == 0 && rollbackFailure == 0 && finalHeadFailure == 0 && preflightFailure == 0 ? 0 : 1;
}
