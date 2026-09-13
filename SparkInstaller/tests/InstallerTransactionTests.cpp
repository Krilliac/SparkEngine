#include "Installer.h"
#include "InstallState.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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
        if (command == "--version" || command == "fetch" || command == "-S")
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
        failures += Check(restoredCommit == kFakeHeadCommit,
                          "failed update did not restore the previously working commit");
        failures += Check(log.find("rolling back update") != std::string::npos,
                          "failed update did not report that rollback was attempted");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          "installer reported completion after update build failure");

        fs::remove_all(root, error);
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
    return persistenceFailure == 0 && rollbackFailure == 0 ? 0 : 1;
}
