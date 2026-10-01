#include "GitRunner.h"
#include "Installer.h"
#include "InstallState.h"
#include "InstallerPreflight.h"
#include "ProcessRunner.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr std::string_view kFakeHeadCommit = "fake-head-commit";
    // HEAD the fake git reports after a non-detached checkout of the update ref.
    constexpr std::string_view kFakeNewCommit = "fake-new-commit";

    int Check(bool condition, const std::string& message)
    {
        if (condition)
        {
            return 0;
        }
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    fs::path MakeTestRoot()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() / ("SparkInstallerTransactionTests_" + std::to_string(stamp));
    }

    bool WriteTextFile(const fs::path& path, std::string_view contents)
    {
        std::error_code error;
        if (!path.parent_path().empty())
        {
            fs::create_directories(path.parent_path(), error);
            if (error)
            {
                return false;
            }
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        out.close();
        return static_cast<bool>(out);
    }

    std::string ReadFirstLine(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        std::getline(in, line);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        return line;
    }

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::map<std::string, std::string> ReadBuildOutputs(const fs::path& build)
    {
        std::map<std::string, std::string> outputs;
        for (const auto& entry : fs::recursive_directory_iterator(build))
        {
            if (entry.is_regular_file())
            {
                outputs.emplace(entry.path().lexically_relative(build).generic_string(), ReadBytes(entry.path()));
            }
        }
        return outputs;
    }

    bool EnvironmentFlagSet(const char* name)
    {
        const char* value = std::getenv(name);
        return value && std::string_view(value) == "1";
    }

    // RunAsync goes through cmd /c or /bin/sh; RunSync's argv escaping is
    // deliberately different. These arguments are generated temporary paths.
    std::string QuoteAsyncPath(const fs::path& path)
    {
#ifdef _WIN32
        return "\"" + path.string() + "\"";
#else
        std::string quoted = "'";
        for (const char character : path.string())
        {
            if (character == '\'')
            {
                quoted += "'\\''";
            }
            else
            {
                quoted.push_back(character);
            }
        }
        return quoted + "'";
#endif
    }

    // The fake git and cmake share this executable. It keeps a tiny model of
    // the checkout in its working directory: fake-git-head is HEAD after a
    // checkout (a detached checkout restores the named commit, a ref checkout
    // moves to kFakeNewCommit), and fake-cmake-build-count counts builds.
    int RunFakeTool(int argc, char* argv[])
    {
        if (argc < 2)
        {
            return 97;
        }

        const std::string_view command = argv[1];
        if (const char* logPath = std::getenv("SPARK_FAKE_TOOL_LOG"); logPath && *logPath)
        {
            std::ofstream invocationLog(logPath, std::ios::binary | std::ios::app);
            invocationLog << command << '\n';
            if (!invocationLog)
            {
                return 94;
            }
        }
        if (command == "fetch")
        {
            return EnvironmentFlagSet("SPARK_FAKE_FETCH_FAIL") ? 44 : 0;
        }
        if (command == "-S")
        {
            return EnvironmentFlagSet("SPARK_FAKE_CONFIGURE_FAIL") ? 45 : 0;
        }
        if (command == "--version" || command == "status")
        {
            return 0;
        }
        if (command == "clone")
        {
            // The destination is the last argument. A failing clone still
            // leaves a partial tree behind, as an interrupted git clone does.
            const fs::path cloned = argv[argc - 1];
            std::error_code error;
            fs::create_directories(cloned / ".git", error);
            if (error || !WriteTextFile(cloned / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\n"))
            {
                return 92;
            }
            if (const char* cloneCommit = std::getenv("SPARK_FAKE_CLONE_COMMIT"); cloneCommit && *cloneCommit)
            {
                if (!WriteTextFile(cloned / "fake-git-head", cloneCommit))
                {
                    return 92;
                }
            }
            if (EnvironmentFlagSet("SPARK_FAKE_STAGE_MARKER_DIRECTORY"))
            {
                fs::create_directories(cloned / SparkInstaller::InstallState::FileName(), error);
                if (error)
                {
                    return 92;
                }
            }
            if (EnvironmentFlagSet("SPARK_FAKE_USER_COLLISION") &&
                !WriteTextFile(cloned / "user-data" / "profile.json", "new tracked file"))
            {
                return 92;
            }
            return EnvironmentFlagSet("SPARK_FAKE_CLONE_FAIL") ? 41 : 0;
        }
        if (command == "checkout")
        {
            if (argc < 3)
            {
                return 96;
            }
            const std::string target = argv[argc - 1];
            const bool detached = std::string_view(argv[2]) == "--detach";
            const bool written = WriteTextFile("fake-git-last-checkout", target) &&
                                 WriteTextFile("fake-git-head", detached ? std::string_view(target) : kFakeNewCommit);
            if (const char* acknowledgement = std::getenv("SPARK_FAKE_CHECKOUT_ACK");
                acknowledgement && *acknowledgement)
            {
                if (!WriteTextFile(acknowledgement, target))
                {
                    return 96;
                }
                if (const char* release = std::getenv("SPARK_FAKE_CHECKOUT_RELEASE"); release && *release)
                {
                    for (int attempt = 0; attempt < 200 && !fs::exists(release); ++attempt)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }
            return written ? 0 : 96;
        }
        if (command == "--build")
        {
            if (argc < 3)
            {
                return 93;
            }
            // Simulate a compiler that truncates an old output before failing.
            // Use the actual --build path, so accidentally reusing the live
            // build directory is observable even when the tool's CWD is staging.
            const fs::path buildPath = argv[2];
            constexpr char partialBinary[] = "new or partial binary\0payload";
            if (!WriteTextFile(buildPath / "engine.bin", std::string_view(partialBinary, sizeof(partialBinary) - 1)) ||
                !WriteTextFile(buildPath / "metadata.txt", "new metadata\nsecond line\n"))
            {
                return 93;
            }
            int builds = 0;
            if (const std::string count = ReadFirstLine("fake-cmake-build-count"); !count.empty())
            {
                builds = std::stoi(count);
            }
            if (!WriteTextFile("fake-cmake-build-count", std::to_string(builds + 1)))
            {
                return 93;
            }
            if (fs::exists("force-build-failure") || EnvironmentFlagSet("SPARK_FAKE_BUILD_FAIL") ||
                (EnvironmentFlagSet("SPARK_FAKE_BUILD_FAIL_ON_NEW_REF") &&
                 ReadFirstLine("fake-git-head") == kFakeNewCommit))
            {
                return 42;
            }
            // The update ref's build is broken; every earlier commit still builds.
            if (fs::exists("force-build-failure-on-new-ref") && ReadFirstLine("fake-git-head") == kFakeNewCommit)
            {
                return 43;
            }
            return 0;
        }
        if (command == "show-ref")
        {
            return 1;
        }
        if (command == "ls-files")
        {
            if (fs::exists("build/engine.bin"))
            {
                std::cout << "build/engine.bin" << '\0';
            }
            if (fs::exists("user-data/profile.json"))
            {
                std::cout << "user-data/profile.json" << '\0';
            }
            return 0;
        }
        if (command == "rev-parse")
        {
            if (fs::exists("force-final-head-failure") || EnvironmentFlagSet("SPARK_FAKE_FINAL_HEAD_FAILURE"))
            {
                if (fs::exists("fake-git-initial-head-read"))
                {
                    return 95;
                }
                std::ofstream initialHeadRead("fake-git-initial-head-read", std::ios::binary | std::ios::trunc);
                if (!initialHeadRead)
                {
                    return 96;
                }
            }
            if (const std::string head = ReadFirstLine("fake-git-head"); !head.empty())
            {
                std::cout << head << '\n';
            }
            else if (const char* initialHead = std::getenv("SPARK_FAKE_HEAD"); initialHead && *initialHead)
            {
                std::cout << initialHead << '\n';
            }
            else
            {
                std::cout << kFakeHeadCommit << '\n';
            }
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
            {
                m_previous = current;
            }

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
            {
                (void)_putenv_s("PATH", m_previous.c_str());
            }
            else
            {
                (void)_putenv_s("PATH", "");
            }
#else
            if (m_hadValue)
            {
                (void)setenv("PATH", m_previous.c_str(), 1);
            }
            else
            {
                (void)unsetenv("PATH");
            }
#endif
        }

        bool IsSet() const { return m_ok; }

      private:
        std::string m_previous;
        bool m_hadValue = false;
        bool m_ok = false;
    };

    // An empty value unsets the variable.
    void SetEnvironment(const char* name, const std::string& value)
    {
#ifdef _WIN32
        (void)_putenv_s(name, value.c_str());
#else
        if (value.empty())
        {
            (void)unsetenv(name);
        }
        else
        {
            (void)setenv(name, value.c_str(), 1);
        }
#endif
    }

    // Copies this executable to <root>/tools/git(.exe) so PATH resolves the fake.
    int CreateFakeGit(const fs::path& root, const fs::path& executable, const std::string& name)
    {
        std::error_code error;
        fs::create_directories(root / "tools", error);
        int failures = Check(!error, name + ": could not create test root");
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
        return failures;
    }

    // A destination that mode detection classifies as an existing engine clone.
    int CreateFakeCheckout(const fs::path& destination, const std::string& name)
    {
        std::error_code error;
        fs::create_directories(destination / ".git", error);
        int failures = Check(!error, name + ": could not create fake engine checkout");
        failures += Check(WriteTextFile(destination / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\n"),
                          name + ": could not create fake engine CMakeLists");
        return failures;
    }

    int SaveInstallState(const fs::path& destination, std::string_view commit, const std::string& name)
    {
        SparkInstaller::InstallState state;
        state.ref = "Working";
        state.commit = std::string(commit);
        state.generator = "Ninja";
        state.buildType = "Release";
        state.installerVersion = "1.0.0";
        return Check(state.Save(destination.string()), name + ": could not create prior valid install state");
    }

    SparkInstaller::InstallerContext MakeContext(const fs::path& destination, const fs::path& executable,
                                                 std::string& log)
    {
        SparkInstaller::InstallerContext context;
        context.frontend = SparkInstaller::Frontend::Headless;
        context.destination = destination.string();
        context.ref = "Working";
        context.skipSubmoduleUpdate = true;
        context.minFreeBytes = std::uintmax_t{0};
        context.configManager.config.cmakePath = executable.string();
        context.configManager.config.buildPath = (destination / "build").string();
        context.log = [&log](const std::string& line)
        {
            log += line;
            log.push_back('\n');
        };
        return context;
    }

    int RunInstallStatePersistenceFailureTest(const fs::path& executable)
    {
        const std::string name = "install-state persistence failure";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), "could not prepend fake git to PATH");
        SetEnvironment("SPARK_FAKE_STAGE_MARKER_DIRECTORY", "1");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_STAGE_MARKER_DIRECTORY", "");
        failures += Check(result == 8, "installer accepted a failed install-state replacement as success");
        failures += Check(log.find("could not write install state file") != std::string::npos,
                          "installer did not report the install-state persistence failure");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          "installer reported completion after install-state persistence failed");
        failures += Check(!fs::exists(destination / (SparkInstaller::InstallState::FileName() + ".tmp")),
                          "failed install-state replacement left a temporary marker behind");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    bool HasStagingSibling(const fs::path& parent, const std::string& destinationName);

    // The staged update fails before activation. The previously verified tree,
    // including build outputs and user data, must remain byte-identical.
    int RunUpdateRollbackRebuildFailureTest(const fs::path& executable)
    {
        const std::string name = "rollback rebuild failure";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);
        const std::string verifiedBuild = std::string("verified-build-output\0payload\n", 30);
        failures += Check(WriteTextFile(destination / "build" / "engine.bin", verifiedBuild),
                          name + ": could not create verified build output");
        failures += Check(WriteTextFile(destination / "build" / "metadata.txt", "verified metadata\n"),
                          name + ": could not create verified build metadata");
        failures += Check(WriteTextFile(destination / "user-data" / "profile.json", "player-progress-v1\n"),
                          name + ": could not create user data fixture");

        const std::string originalBuild = ReadBytes(destination / "build" / "engine.bin");
        const std::string originalMetadata = ReadBytes(destination / "build" / "metadata.txt");
        const auto originalOutputs = ReadBuildOutputs(destination / "build");
        const std::string originalUserData = ReadBytes(destination / "user-data" / "profile.json");
        const std::string originalState = ReadBytes(destination / SparkInstaller::InstallState::FileName());

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");

        SetEnvironment("SPARK_FAKE_CLONE_COMMIT", std::string(kFakeNewCommit));
        SetEnvironment("SPARK_FAKE_BUILD_FAIL_ON_NEW_REF", "1");
        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_CLONE_COMMIT", "");
        SetEnvironment("SPARK_FAKE_BUILD_FAIL_ON_NEW_REF", "");
        failures += Check(result == 7,
                          name + ": expected staged build failure exit 7, got " + std::to_string(result) + "\n" + log);
        const std::string currentState = ReadBytes(destination / SparkInstaller::InstallState::FileName());
        failures += Check(currentState == originalState, name + ": failed update changed install state bytes");
        failures += Check(ReadBytes(destination / "build" / "engine.bin") == originalBuild,
                          name + ": failed update changed the verified build output");
        failures += Check(ReadBytes(destination / "build" / "metadata.txt") == originalMetadata,
                          name + ": failed update changed the verified build metadata");
        failures += Check(ReadBuildOutputs(destination / "build") == originalOutputs,
                          name + ": failed update added, removed, or changed build output bytes");
        failures += Check(ReadBytes(destination / "user-data" / "profile.json") == originalUserData,
                          name + ": failed update changed user data");
        SparkInstaller::InstallState loaded;
        failures +=
            Check(SparkInstaller::InstallState::Load(destination.string(), loaded) && loaded.commit == kFakeHeadCommit,
                  name + ": failed update changed the recorded verified commit");
        failures += Check(!fs::exists(destination / SparkInstaller::InstallState::RepairRequiredFileName()),
                          name + ": failed staged update wrote a repair-required marker");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          name + ": installer reported completion after update build failure");
        failures +=
            Check(!HasStagingSibling(root, "install"), name + ": failed staged update left a staging sibling behind");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    int RunStagedUpdateBoundaryTest(const fs::path& executable)
    {
        const std::string name = "staged update boundaries";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);
        failures += Check(WriteTextFile(destination / "build" / "engine.bin", "working binary"),
                          name + ": could not write prior build");
        failures += Check(WriteTextFile(destination / "user-data" / "profile.json", "user progress"),
                          name + ": could not write ignored user file");
        const auto originalOutputs = ReadBuildOutputs(destination / "build");
        const auto originalState = ReadBytes(destination / SparkInstaller::InstallState::FileName());
        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not set fixture PATH");
        std::string log;

        auto context = MakeContext(destination, executable, log);
        context.mode = SparkInstaller::Mode::Update;
        failures +=
            Check(SparkInstaller::Preflight::DefaultMinFreeBytes(context) == 40ull * 1024ull * 1024ull * 1024ull,
                  name + ": transactional updates must reserve the full sibling-build budget");
        context.configManager.config.buildPath = (root / "external-build").string();
        failures += Check(SparkInstaller::Installer::Run(context) == 6,
                          name + ": update accepted a build path outside staging");
        failures += Check(!fs::exists(root / "external-build"), name + ": invalid build path was written");

        context = MakeContext(destination, executable, log);
        SetEnvironment("SPARK_FAKE_CONFIGURE_FAIL", "1");
        const int configureResult = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_CONFIGURE_FAIL", "");
        failures += Check(configureResult == 6, name + ": configure failure was not propagated");

        context = MakeContext(destination, executable, log);
        SetEnvironment("SPARK_FAKE_USER_COLLISION", "1");
        const int collisionResult = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_USER_COLLISION", "");
        failures += Check(collisionResult == 8, name + ": ignored user file collision was not refused");

        context = MakeContext(destination, executable, log);
        context.skipBuild = true;
        failures += Check(SparkInstaller::Installer::Run(context) == 0, name + ": source-only staging failed");
        failures += Check(HasStagingSibling(root, "install"), name + ": source-only update was not left staged");
        failures += Check(ReadBuildOutputs(destination / "build") == originalOutputs,
                          name + ": refused or unbuilt update changed build bytes");
        failures += Check(ReadBytes(destination / SparkInstaller::InstallState::FileName()) == originalState,
                          name + ": refused or unbuilt update changed the verified state");
        failures += Check(ReadBytes(destination / "user-data" / "profile.json") == "user progress",
                          name + ": refused or unbuilt update changed user data");
        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    int RunRecoveryBeforeBuildPreflightTest(const fs::path& executable)
    {
        const std::string name = "recovery precedes build preflight";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        const fs::path previous = destination.string() + ".sparkinstall-previous-pending";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);
        failures += Check(WriteTextFile(destination / "build" / "engine.bin", "verified binary"),
                          name + ": could not write the prior binary");
        const auto stateBytes = ReadBytes(destination / SparkInstaller::InstallState::FileName());
        std::error_code error;
        fs::rename(destination, previous, error);
        failures += Check(!error, name + ": could not simulate the first activation rename");
        std::string log;
        auto context = MakeContext(destination, executable, log);
        context.minFreeBytes = std::numeric_limits<std::uintmax_t>::max();
        context.configManager.config.cmakePath = (root / "missing-cmake").string();
        failures += Check(SparkInstaller::Installer::Run(context) == 10,
                          name + ": the subsequent build preflight was not enforced");
        failures += Check(ReadBytes(destination / "build" / "engine.bin") == "verified binary" &&
                              ReadBytes(destination / SparkInstaller::InstallState::FileName()) == stateBytes &&
                              !fs::exists(previous),
                          name + ": missing build tools or disk space prevented restoration of the working tree");
        fs::remove_all(root, error);
        return failures;
    }

    int RunProcessKillInterruptionTest(const fs::path& testExecutable)
    {
        const std::string name = "process-kill interruption";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, testExecutable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);
        failures += Check(WriteTextFile(destination / "build" / "engine.bin", "verified-build-output\n"),
                          name + ": could not create verified build output");
        failures += Check(WriteTextFile(destination / "user-data" / "profile.json", "player-progress-v1\n"),
                          name + ": could not create user data fixture");

        const fs::path installer = SPARK_INSTALLER_TEST_EXE;
        if (!fs::is_regular_file(installer))
        {
            failures += Check(false, name + ": SparkInstaller binary is missing: " + installer.string());
            std::error_code error;
            fs::remove_all(root, error);
            return failures;
        }

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake tools to PATH");
        const fs::path acknowledgement = root / "checkout-ack";
        const fs::path release = root / "checkout-release";
        SetEnvironment("SPARK_FAKE_CLONE_COMMIT", std::string(kFakeNewCommit));
        SetEnvironment("SPARK_FAKE_CHECKOUT_ACK", acknowledgement.string());
        SetEnvironment("SPARK_FAKE_CHECKOUT_RELEASE", release.string());

        SparkBuild::ProcessRunner runner;
        std::string output;
        std::string command = QuoteAsyncPath(installer) + " --headless --dest " + QuoteAsyncPath(destination) +
                              " --skip-submodules --skip-build";
#ifdef _WIN32
        command = "\"" + command + "\"";
#endif
        failures += Check(runner.RunAsync(command, root.string(), nullptr, nullptr),
                          name + ": could not launch SparkInstaller through ProcessRunner");
        bool acknowledged = false;
        for (int attempt = 0; attempt < 100 && runner.IsRunning(); ++attempt)
        {
            acknowledged = fs::is_regular_file(acknowledgement);
            if (acknowledged)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        failures += Check(acknowledged, name + ": fake git checkout was not observed before timeout");
        runner.Cancel();
        // RunSync git children inherit the real installer's process group/job.
        // Release the bounded fake gate too if termination failed on this host.
        failures += Check(WriteTextFile(release, "release"), name + ": could not release the fake checkout gate");
        SetEnvironment("SPARK_FAKE_CHECKOUT_ACK", "");
        SetEnvironment("SPARK_FAKE_CHECKOUT_RELEASE", "");
        for (int attempt = 0; attempt < 100 && runner.IsRunning(); ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        failures += Check(!runner.IsRunning(), name + ": killed installer process did not terminate");
        failures += Check(runner.GetExitCode() != 0, name + ": installer completed instead of being interrupted");

        failures += Check(ReadBytes(destination / "build" / "engine.bin") == "verified-build-output\n",
                          name + ": interrupted update changed the live build output");
        failures += Check(ReadBytes(destination / "user-data" / "profile.json") == "player-progress-v1\n",
                          name + ": interrupted update changed user data");
        SparkInstaller::InstallState beforeResume;
        failures += Check(SparkInstaller::InstallState::Load(destination.string(), beforeResume) &&
                              beforeResume.commit == kFakeHeadCommit,
                          name + ": interrupted update changed the recorded commit");

        output.clear();
        SparkInstaller::InstallerContext resumedContext = MakeContext(destination, testExecutable, output);
        const int resumed = SparkInstaller::Installer::Run(resumedContext);
        failures += Check(resumed == 0, name + ": rerun failed with exit " + std::to_string(resumed) + "\n" + output);
        SparkInstaller::InstallState afterResume;
        failures += Check(SparkInstaller::InstallState::Load(destination.string(), afterResume) &&
                              afterResume.commit == kFakeNewCommit,
                          name + ": rerun did not record the completed staged commit");
        const std::string archivePrefix = destination.filename().string() + ".sparkinstall-previous-";
        bool archivedUserData = false;
        std::error_code archiveError;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, archiveError))
        {
            if (entry.path().filename().string().rfind(archivePrefix, 0) == 0 &&
                ReadBytes(entry.path() / "user-data" / "profile.json") == "player-progress-v1\n")
            {
                archivedUserData = true;
                break;
            }
        }
        failures += Check(archivedUserData, name + ": rerun did not retain the previous tree's user data for rollback");
        failures += Check(ReadBytes(destination / "user-data" / "profile.json") == "player-progress-v1\n",
                          name + ": rerun did not preserve ignored user data in the active tree");
        SetEnvironment("SPARK_FAKE_CLONE_COMMIT", "");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    std::vector<std::string> ReadToolLog(const fs::path& logPath);

    // Only the staged new ref fails to build; the verified destination is
    // never rebuilt or otherwise mutated.
    int RunUpdateRollbackRebuildTest(const fs::path& executable)
    {
        const std::string name = "rollback rebuild";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);

        const fs::path toolLog = root / "tool-invocations.log";
        SetEnvironment("SPARK_FAKE_TOOL_LOG", toolLog.string());
        SetEnvironment("SPARK_FAKE_CLONE_COMMIT", std::string(kFakeNewCommit));
        SetEnvironment("SPARK_FAKE_BUILD_FAIL_ON_NEW_REF", "1");
        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_TOOL_LOG", "");
        SetEnvironment("SPARK_FAKE_CLONE_COMMIT", "");
        SetEnvironment("SPARK_FAKE_BUILD_FAIL_ON_NEW_REF", "");
        failures += Check(result == 7, name + ": expected the original build failure exit 7, got " +
                                           std::to_string(result) + "\n" + log);
        // The staged update configures and builds only its sibling.
        size_t configures = 0;
        size_t builds = 0;
        for (const std::string& command : ReadToolLog(toolLog))
        {
            if (command == "-S")
            {
                ++configures;
            }
            else if (command == "--build")
            {
                ++builds;
            }
        }
        failures += Check(configures == 1, name + ": staged update did not configure exactly once");
        failures += Check(builds == 1, name + ": staged update did not build exactly once");
        failures += Check(!fs::exists(destination / "fake-git-last-checkout"),
                          name + ": failed staged update wrote checkout state into the live tree");
        failures += Check(!fs::exists(destination / "fake-cmake-build-count"),
                          name + ": failed staged update rebuilt the live tree");
        SparkInstaller::InstallState loaded;
        failures +=
            Check(SparkInstaller::InstallState::Load(destination.string(), loaded) && loaded.commit == kFakeHeadCommit,
                  name + ": failed staged update changed install state");
        failures += Check(!fs::exists(destination / SparkInstaller::InstallState::RepairRequiredFileName()),
                          name + ": a successful rollback rebuild left a repair-required marker");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          name + ": installer reported completion after update build failure");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    // A legacy live tree whose HEAD differs from its recorded build is
    // inconsistent. Transactional updates fail closed before mutating it.
    int RunInterruptedUpdateRollbackTargetTest(const fs::path& executable)
    {
        const std::string name = "interrupted update rollback target";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, "verified-old", name);
        const fs::path userData = destination / "user-data" / "profile.json";
        std::error_code userDataError;
        fs::create_directories(userData.parent_path(), userDataError);
        failures += Check(!userDataError && WriteTextFile(userData, "player-progress-v1"),
                          name + ": could not create external user-data fixture");

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");
        SetEnvironment("SPARK_FAKE_HEAD", "interrupted-head");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_HEAD", "");

        failures += Check(result == 5, name + ": expected inconsistent-tree refusal exit 5, got " +
                                           std::to_string(result) + "\n" + log);
        failures += Check(log.find("Interrupted update detected") != std::string::npos,
                          name + ": the inconsistent update was not reported");
        failures += Check(!fs::exists(destination / "fake-git-last-checkout"),
                          name + ": refusal wrote checkout state into the live tree");
        failures +=
            Check(ReadFirstLine(userData) == "player-progress-v1", name + ": interrupted recovery replaced user data");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    // A failed staged fetch leaves the verified destination untouched.
    int RunUpdateFetchFailureRollbackTest(const fs::path& executable)
    {
        const std::string name = "update fetch failure";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");
        SetEnvironment("SPARK_FAKE_FETCH_FAIL", "1");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_FETCH_FAIL", "");

        failures +=
            Check(result == 5, name + ": expected fetch failure exit 5, got " + std::to_string(result) + "\n" + log);
        failures += Check(!fs::exists(destination / "fake-git-last-checkout"),
                          name + ": failed staged fetch wrote checkout state into the live tree");
        failures += Check(!fs::exists(destination / "fake-cmake-build-count"),
                          name + ": failed staged fetch rebuilt the live tree");
        SparkInstaller::InstallState loaded;
        failures +=
            Check(SparkInstaller::InstallState::Load(destination.string(), loaded) && loaded.commit == kFakeHeadCommit,
                  name + ": a failed fetch changed the recorded install");
        failures += Check(log.find("Done. Engine built at:") == std::string::npos,
                          name + ": installer reported completion after a failed fetch");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    // A successful update supersedes an earlier failed rollback.
    int RunUpdateClearsRepairMarkerTest(const fs::path& executable)
    {
        const std::string name = "update clears repair marker";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += Check(WriteTextFile(destination / SparkInstaller::InstallState::RepairRequiredFileName(),
                                        "earlier rollback failed\n"),
                          name + ": could not create repair-required fixture");

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        failures += Check(result == 0, name + ": update failed, exit " + std::to_string(result) + "\n" + log);
        SparkInstaller::InstallState loaded;
        failures +=
            Check(SparkInstaller::InstallState::Load(destination.string(), loaded) && loaded.commit == kFakeNewCommit,
                  name + ": successful update did not record the new commit");
        failures += Check(!fs::exists(destination / SparkInstaller::InstallState::RepairRequiredFileName()),
                          name + ": successful update left the repair-required marker");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    int RunPostBuildHeadCommitFailureTest(const fs::path& executable)
    {
        const std::string name = "final head verification";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        failures += CreateFakeCheckout(destination, name);
        failures += SaveInstallState(destination, kFakeHeadCommit, name);
        failures += Check(WriteTextFile(destination / "force-final-head-failure", "fail"),
                          "could not create final-head failure fixture");

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), "could not prepend final-head fake git to PATH");
        SetEnvironment("SPARK_FAKE_FINAL_HEAD_FAILURE", "1");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_FINAL_HEAD_FAILURE", "");
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

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    // Commands the fake git/cmake tool was invoked with while a case ran.
    std::vector<std::string> ReadToolLog(const fs::path& logPath)
    {
        std::vector<std::string> commands;
        std::ifstream in(logPath, std::ios::binary);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (!line.empty())
            {
                commands.push_back(line);
            }
        }
        return commands;
    }

    enum class PreflightDestination
    {
        Fresh,
        Link,
        LinkedAncestor, // a link two levels above an ordinary existing directory
        CorruptMarkerUpdate
    };

    // Creates a directory junction (Windows) or symlink at @p link pointing to @p target.
    int CreateDirectoryLink(const fs::path& link, const fs::path& target, const fs::path& root, const std::string& name)
    {
#ifdef _WIN32
        // A junction needs no symlink privilege and is the Windows link
        // shape std::filesystem does not portably report. mklink is a
        // cmd.exe builtin (there is no mklink.exe) and RunSync does not
        // go through a shell, so the builtin must run under cmd /c.
        using SparkInstaller::GitRunner;
        SparkBuild::ProcessRunner runner;
        std::string output;
        int failures =
            Check(runner.RunSync("cmd.exe /c mklink /J " + GitRunner::EncodeProcessRunnerArgument(link.string()) + " " +
                                     GitRunner::EncodeProcessRunnerArgument(target.string()),
                                 root.string(), output) == 0,
                  name + ": could not create junction fixture: " + output);
        return failures + Check(fs::exists(link), name + ": junction fixture is missing");
#else
        (void)root;
        std::error_code error;
        fs::create_directory_symlink(target, link, error);
        return Check(!error, name + ": could not create symlink fixture");
#endif
    }

    // Runs the installer against a fixture that preflight must refuse and
    // proves it returns exit 10 before any git or build command ran. The only
    // tolerated tool invocation is the read-only `cmake --version` probe.
    int RunPreflightRefusalTest(const fs::path& executable, const std::string& name, PreflightDestination kind,
                                const std::string& expectedCode, bool impossibleFreeSpace, bool missingCMake)
    {
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        std::error_code error;

        fs::path destination = root / "install";
        const fs::path linkTarget = root / "real-install";
        const fs::path linkedParent = root / "linked-parent";
        if (kind == PreflightDestination::Link)
        {
            fs::create_directories(linkTarget, error);
            failures += Check(!error, name + ": could not create link target");
            failures += CreateDirectoryLink(destination, linkTarget, root, name);
        }
        else if (kind == PreflightDestination::LinkedAncestor)
        {
            // root/linked-parent -> root/real-install; linked-parent/existing is
            // an ordinary directory, so it is the nearest existing ancestor of
            // the destination and is not itself a link.
            fs::create_directories(linkTarget / "existing", error);
            failures += Check(!error, name + ": could not create link target");
            failures += CreateDirectoryLink(linkedParent, linkTarget, root, name);
            destination = linkedParent / "existing" / "install";
        }
        else if (kind == PreflightDestination::CorruptMarkerUpdate)
        {
            failures += CreateFakeCheckout(destination, name);
            failures += Check(
                WriteTextFile(destination / SparkInstaller::InstallState::FileName(), "{ \"schema\": 1, truncated"),
                name + ": could not create corrupt marker fixture");
        }

        const fs::path toolLog = root / "tool-invocations.log";
        SetEnvironment("SPARK_FAKE_TOOL_LOG", toolLog.string());
        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        context.configManager.config.cmakePath =
            missingCMake ? (root / "missing" / "cmake-does-not-exist").string() : executable.string();
        // Isolate each case: only the free-space case can fail the space check.
        context.minFreeBytes = impossibleFreeSpace ? std::numeric_limits<std::uintmax_t>::max() : std::uintmax_t{0};

        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_TOOL_LOG", "");

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
        else if (kind == PreflightDestination::LinkedAncestor)
        {
            failures += Check(fs::is_empty(linkTarget / "existing", error),
                              name + ": refused install wrote through the linked ancestor");
            fs::remove(linkedParent, error);
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
        // SEC finding 23: a link above the nearest existing ancestor is refused too.
        failures += RunPreflightRefusalTest(executable, "linked destination ancestor",
                                            PreflightDestination::LinkedAncestor, "destination-link",
                                            /*impossibleFreeSpace=*/false, /*missingCMake=*/false);
        failures += RunPreflightRefusalTest(executable, "corrupt marker update",
                                            PreflightDestination::CorruptMarkerUpdate, "corrupt-install-marker",
                                            /*impossibleFreeSpace=*/false, /*missingCMake=*/false);
        return failures;
    }

    bool HasStagingSibling(const fs::path& parent, const std::string& destinationName)
    {
        const std::string prefix = "." + destinationName + ".sparkinstall-";
        std::error_code error;
        for (const fs::directory_entry& entry : fs::directory_iterator(parent, error))
        {
            if (entry.path().filename().string().rfind(prefix, 0) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // A failed clone happens in staging: the destination never appears and
    // the partial staging tree is removed.
    int RunFreshInstallCloneFailureTest(const fs::path& executable)
    {
        const std::string name = "fresh install clone failure";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");
        SetEnvironment("SPARK_FAKE_CLONE_FAIL", "1");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
        context.minFreeBytes = std::uintmax_t{0};
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_CLONE_FAIL", "");

        failures +=
            Check(result == 5, name + ": expected clone failure exit 5, got " + std::to_string(result) + "\n" + log);
        failures += Check(!fs::exists(destination), name + ": a failed clone created the destination");
        failures += Check(!HasStagingSibling(root, "install"), name + ": the partial staging clone was left behind");

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    // A fresh install whose build fails stays pending; it is never mistaken
    // for a working install to update, and the next run resumes it.
    int RunFreshInstallBuildFailureResumeTest(const fs::path& executable)
    {
        const std::string name = "fresh install resume";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";
        const fs::path pendingMarker = destination / SparkInstaller::InstallState::PendingFileName();

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");

        {
            SetEnvironment("SPARK_FAKE_BUILD_FAIL", "1");
            std::string log;
            SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
            context.minFreeBytes = std::uintmax_t{0};
            const int result = SparkInstaller::Installer::Run(context);
            SetEnvironment("SPARK_FAKE_BUILD_FAIL", "");
            failures += Check(result == 7,
                              name + ": expected build failure exit 7, got " + std::to_string(result) + "\n" + log);
            failures += Check(fs::is_regular_file(pendingMarker),
                              name + ": the activated but unbuilt clone has no pending marker");
            failures += Check(!SparkInstaller::InstallState::Exists(destination.string()),
                              name + ": an unbuilt install recorded install state");
            failures += Check(!HasStagingSibling(root, "install"), name + ": activation left the staging tree");
        }
        const fs::path userData = destination / "user-data" / "profile.json";
        std::error_code userDataError;
        fs::create_directories(userData.parent_path(), userDataError);
        failures += Check(!userDataError && WriteTextFile(userData, "pending-install-progress"),
                          name + ": could not create pending-install user data");
        {
            std::string log;
            SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
            context.minFreeBytes = std::uintmax_t{0};
            context.ref = "Other";
            const int result = SparkInstaller::Installer::Run(context);
            failures += Check(result == 4, name + ": a different ref resumed the pending clone, exit " +
                                               std::to_string(result) + "\n" + log);
            failures += Check(fs::is_regular_file(pendingMarker), name + ": a refused resume cleared the marker");
        }
        {
            SetEnvironment("SPARK_FAKE_HEAD", "moved-head");
            std::string log;
            SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
            context.minFreeBytes = std::uintmax_t{0};
            const int result = SparkInstaller::Installer::Run(context);
            SetEnvironment("SPARK_FAKE_HEAD", "");
            failures += Check(result == 5, name + ": a clone moved off its pending commit was resumed, exit " +
                                               std::to_string(result) + "\n" + log);
        }
        {
            const fs::path toolLog = root / "resume-tool-invocations.log";
            SetEnvironment("SPARK_FAKE_TOOL_LOG", toolLog.string());
            std::string log;
            SparkInstaller::InstallerContext context = MakeContext(destination, executable, log);
            context.minFreeBytes = std::uintmax_t{0};
            const int result = SparkInstaller::Installer::Run(context);
            SetEnvironment("SPARK_FAKE_TOOL_LOG", "");
            failures += Check(result == 0, name + ": resume failed, exit " + std::to_string(result) + "\n" + log);
            failures += Check(log.find("Mode: Resume install") != std::string::npos,
                              name + ": the pending clone was not detected as Resume install\n" + log);
            bool built = false;
            for (const std::string& command : ReadToolLog(toolLog))
            {
                failures += Check(command != "clone" && command != "fetch" && command != "checkout",
                                  name + ": resume ran `git " + command + "`");
                built = built || command == "--build";
            }
            failures += Check(built, name + ": resume did not build");
            SparkInstaller::InstallState loaded;
            failures += Check(SparkInstaller::InstallState::Load(destination.string(), loaded) &&
                                  loaded.commit == kFakeHeadCommit,
                              name + ": resume did not record the cloned commit");
            failures += Check(!fs::exists(pendingMarker), name + ": a finished install kept its pending marker");
            failures += Check(ReadFirstLine(userData) == "pending-install-progress",
                              name + ": resumed activation replaced user data");
        }

        std::error_code error;
        fs::remove_all(root, error);
        return failures;
    }

    // A destination typed with a trailing separator ("--dest C:\SparkEngine\")
    // stages beside the destination, not inside it. Staging inside it would
    // make activation find a non-empty destination and wedge every later run.
    int RunFreshInstallTrailingSeparatorTest(const fs::path& executable)
    {
        const std::string name = "fresh install trailing separator";
        const fs::path root = MakeTestRoot();
        int failures = CreateFakeGit(root, executable, name);
        const fs::path destination = root / "install";

        ScopedPathPrefix pathPrefix(root / "tools");
        failures += Check(pathPrefix.IsSet(), name + ": could not prepend fake git to PATH");
        SetEnvironment("SPARK_FAKE_BUILD_FAIL", "1");

        std::string log;
        SparkInstaller::InstallerContext context = MakeContext(destination / "", executable, log);
        context.minFreeBytes = std::uintmax_t{0};
        const int result = SparkInstaller::Installer::Run(context);
        SetEnvironment("SPARK_FAKE_BUILD_FAIL", "");

        failures +=
            Check(result == 7, name + ": expected build failure exit 7, got " + std::to_string(result) + "\n" + log);
        failures += Check(fs::is_regular_file(destination / SparkInstaller::InstallState::PendingFileName()),
                          name + ": the activated clone has no pending marker at the destination\n" + log);
        failures += Check(fs::exists(destination / "CMakeLists.txt"),
                          name + ": the clone was not activated at the destination");
        failures += Check(!HasStagingSibling(root, "install"), name + ": activation left the staging tree");
        std::error_code error;
        if (fs::is_directory(destination, error))
        {
            for (const fs::directory_entry& entry : fs::directory_iterator(destination, error))
            {
                failures += Check(entry.path().filename().string().find(".sparkinstall-") == std::string::npos,
                                  name + ": staging clone nested inside the destination: " + entry.path().string());
            }
        }

        fs::remove_all(root, error);
        return failures;
    }

    // The destination turned non-empty after the emptiness check: activation
    // must refuse and leave both trees intact, and succeed once it is empty.
    int RunActivateStagedTreeTest()
    {
        const std::string name = "staged tree activation";
        const fs::path root = MakeTestRoot();
        const fs::path staging = root / ".install.sparkinstall-test";
        const fs::path destination = root / "install";
        int failures = CreateFakeCheckout(staging, name);
        std::error_code error;
        fs::create_directories(destination, error);
        failures += Check(!error && WriteTextFile(destination / "user.txt", "user data"),
                          name + ": could not create the non-empty destination");

        std::string activationError;
        failures += Check(!SparkInstaller::detail::ActivateStagedTree(staging, destination, activationError),
                          name + ": activation replaced a non-empty destination");
        failures += Check(!activationError.empty(), name + ": refused activation gave no reason");
        failures += Check(ReadFirstLine(destination / "user.txt") == "user data",
                          name + ": refused activation changed the destination");
        failures += Check(fs::exists(staging / "CMakeLists.txt"), name + ": refused activation lost the staged clone");

        fs::remove(destination / "user.txt", error);
        activationError.clear();
        failures += Check(SparkInstaller::detail::ActivateStagedTree(staging, destination, activationError),
                          name + ": activation into an empty destination failed: " + activationError);
        failures += Check(fs::exists(destination / "CMakeLists.txt") && !fs::exists(staging),
                          name + ": activation did not move the staged clone into place");

        // Update activation retains the old tree beside the destination and
        // rejects an unverified stage before the first rename.
        const fs::path stagedUpdate = root / ".install.sparkinstall-update";
        const fs::path liveUpdate = root / "update-install";
        const fs::path previousUpdate = root / "update-install.sparkinstall-previous-pending";
        failures += CreateFakeCheckout(stagedUpdate, name);
        failures += CreateFakeCheckout(liveUpdate, name);
        failures += Check(WriteTextFile(liveUpdate / "user.txt", "old user data"),
                          name + ": could not create retained-tree fixture");
        SparkInstaller::InstallState stagedState;
        stagedState.ref = "Working";
        stagedState.commit = std::string(kFakeNewCommit);
        stagedState.destination = liveUpdate.string();
        stagedState.generator = "Ninja";
        stagedState.buildType = "Release";
        stagedState.installerVersion = "1.0.0";
        failures += Check(stagedState.Save(stagedUpdate.string(), liveUpdate.string()),
                          name + ": could not write staged verification state");
        activationError.clear();
        failures +=
            Check(SparkInstaller::detail::ActivateStagedTree(stagedUpdate, liveUpdate, activationError, previousUpdate),
                  name + ": verified update activation failed: " + activationError);
        failures += Check(fs::exists(previousUpdate / "user.txt"),
                          name + ": successful update did not retain the previous tree");
        failures += Check(fs::exists(liveUpdate / SparkInstaller::InstallState::FileName()),
                          name + ": successful update lost staged install state");

        const fs::path invalidStage = root / ".invalid-stage";
        const fs::path invalidLive = root / "invalid-install";
        const fs::path invalidPrevious = root / "invalid-install.sparkinstall-previous-pending";
        failures += CreateFakeCheckout(invalidStage, name);
        failures += CreateFakeCheckout(invalidLive, name);
        failures += Check(WriteTextFile(invalidLive / "user.txt", "must survive"),
                          name + ": could not create invalid-stage fixture");
        activationError.clear();
        failures += Check(
            !SparkInstaller::detail::ActivateStagedTree(invalidStage, invalidLive, activationError, invalidPrevious),
            name + ": unverified stage was accepted for update activation");
        failures += Check(fs::exists(invalidLive / "user.txt") && !fs::exists(invalidPrevious),
                          name + ": rejected update activation changed the live tree");

        const fs::path recoveryLive = root / "recovery-install";
        const fs::path recoveryPrevious = recoveryLive.string() + ".sparkinstall-previous-pending";
        failures += CreateFakeCheckout(recoveryPrevious, name);
        failures += Check(WriteTextFile(recoveryPrevious / "user.txt", "recover me"),
                          name + ": could not create recovery backup");
        activationError.clear();
        failures += Check(SparkInstaller::detail::RecoverPreviousTree(recoveryLive, activationError),
                          name + ": gap recovery failed: " + activationError);
        failures += Check(fs::exists(recoveryLive / "user.txt") && !fs::exists(recoveryPrevious),
                          name + ": gap recovery did not restore the previous tree");

        const fs::path bothLive = root / "both-install";
        const fs::path bothPrevious = bothLive.string() + ".sparkinstall-previous-pending";
        failures += CreateFakeCheckout(bothLive, name);
        failures += SaveInstallState(bothLive, kFakeHeadCommit, name);
        failures += CreateFakeCheckout(bothPrevious, name);
        failures += Check(WriteTextFile(bothPrevious / "user.txt", "archive me"),
                          name + ": could not create both-present recovery backup");
        activationError.clear();
        failures += Check(SparkInstaller::detail::RecoverPreviousTree(bothLive, activationError),
                          name + ": both-present recovery failed: " + activationError);
        const std::string archivePrefix = bothLive.filename().string() + ".sparkinstall-previous-";
        bool archived = false;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, error))
        {
            if (entry.path().filename().string().rfind(archivePrefix, 0) == 0 && fs::exists(entry.path() / "user.txt"))
            {
                archived = true;
                break;
            }
        }
        failures += Check(archived && fs::exists(bothLive / SparkInstaller::InstallState::FileName()),
                          name + ": both-present recovery did not archive the previous tree");

        fs::remove_all(root, error);
        return failures;
    }
} // namespace

int main(int argc, char* argv[])
{
    // --spark-case=<group> selects one case group for the Installer_* CTest
    // selectors. It is checked before the fake-tool dispatch because every
    // other argument vector is a git/cmake command line aimed at the fake.
    constexpr std::string_view kCaseFlag = "--spark-case=";
    std::string_view group = "all";
    if (argc == 2 && std::string_view(argv[1]).substr(0, kCaseFlag.size()) == kCaseFlag)
    {
        group = std::string_view(argv[1]).substr(kCaseFlag.size());
    }
    else if (argc > 1)
    {
        return RunFakeTool(argc, argv);
    }

    const bool all = group == "all";
    // Installer_AtomicUpdate: a failed fetch, clone, build or verification never
    // replaces a working install; a verified sibling is activated with recovery.
    const bool atomicUpdate = all || group == "atomic-update";
    // Installer_Interrupted: a killed run leaves the verified tree intact and
    // a later run can recover the staged update.
    const bool interrupted = all || group == "interrupted";
    const bool preflight = all || group == "preflight";
    if (!atomicUpdate && !interrupted && !preflight)
    {
        std::cerr << "unknown --spark-case group '" << group
                  << "'; expected atomic-update, interrupted, preflight or all\n";
        return 2;
    }

    const fs::path executable = fs::absolute(argv[0]);
    int failures = 0;
    if (atomicUpdate)
    {
        failures += RunInstallStatePersistenceFailureTest(executable);
        failures += RunUpdateRollbackRebuildFailureTest(executable);
        failures += RunStagedUpdateBoundaryTest(executable);
        failures += RunUpdateRollbackRebuildTest(executable);
        failures += RunUpdateFetchFailureRollbackTest(executable);
        failures += RunUpdateClearsRepairMarkerTest(executable);
        failures += RunPostBuildHeadCommitFailureTest(executable);
        failures += RunFreshInstallCloneFailureTest(executable);
        failures += RunFreshInstallTrailingSeparatorTest(executable);
        failures += RunActivateStagedTreeTest();
    }
    if (interrupted)
    {
        failures += RunProcessKillInterruptionTest(executable);
        failures += RunRecoveryBeforeBuildPreflightTest(executable);
        failures += RunInterruptedUpdateRollbackTargetTest(executable);
        failures += RunFreshInstallBuildFailureResumeTest(executable);
    }
    if (preflight)
    {
        failures += RunPreflightTests(executable);
    }
    std::cout << "SparkInstaller transaction tests (" << group << "): " << failures << " failed checks\n";
    return failures == 0 ? 0 : 1;
}
