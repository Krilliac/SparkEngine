#include "SparkBuild.h"
#include "Terminal.h"
#include "Downloader.h"
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkBuild
{
    namespace
    {
#ifndef SPARK_PLATFORM_WINDOWS
        bool LaunchDetachedProcess(const std::string& executable, const std::vector<std::string>& arguments,
                                   const std::string& workingDir = {})
        {
            pid_t pid = fork();
            if (pid == -1)
                return false;

            if (pid == 0)
            {
                if (setsid() == -1)
                    _exit(127);

                const pid_t detachedPid = fork();
                if (detachedPid == -1)
                    _exit(127);
                if (detachedPid != 0)
                    _exit(0);

                if (!workingDir.empty() && chdir(workingDir.c_str()) != 0)
                    _exit(127);

                std::vector<char*> argv;
                argv.reserve(arguments.size() + 2);
                argv.push_back(const_cast<char*>(executable.c_str()));
                for (const auto& argument : arguments)
                    argv.push_back(const_cast<char*>(argument.c_str()));
                argv.push_back(nullptr);

                execvp(executable.c_str(), argv.data());
                _exit(127);
            }

            int status = 0;
            while (waitpid(pid, &status, 0) == -1)
            {
                if (errno == EINTR)
                    continue;
                // A process-wide SIGCHLD policy may already reap children.
                return errno == ECHILD;
            }
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
#endif
    } // namespace

    SparkBuildApp::SparkBuildApp()
    {
        m_config.Load(ConfigManager::GetDefaultIniPath());
    }

    SparkBuildApp::~SparkBuildApp()
    {
        m_config.Save(ConfigManager::GetDefaultIniPath());
    }

    int SparkBuildApp::Run()
    {
        Term::EnableColors();
        Term::ClearScreen();

        Term::PrintHeader("SparkBuild - SparkEngine Build Tool v2.1");
        std::cout << Term::Dim("  Platform: " SPARK_PLATFORM_NAME) << "\n";
        std::cout << Term::Dim("  Config:   " + ConfigManager::GetDefaultIniPath()) << "\n\n";

        while (m_running)
        {
            ShowMainMenu();
        }

        m_config.Save(ConfigManager::GetDefaultIniPath());
        std::cout << "\n" << Term::Green("Configuration saved. Goodbye!") << "\n\n";
        return 0;
    }

    void SparkBuildApp::ShowMainMenu()
    {
        Term::PrintDivider();
        std::cout << Term::Bold(Term::Cyan("  MAIN MENU")) << "\n\n";

        std::cout << "  " << Term::Bold("1") << "  Environment Check\n";
        std::cout << "  " << Term::Bold("2") << "  Configure Build\n";
        std::cout << "  " << Term::Bold("3") << "  Build\n";
        std::cout << "  " << Term::Bold("4") << "  Show Config Summary\n";
        std::cout << "  " << Term::Bold("0") << "  Exit\n\n";

        int choice = Term::ReadInt("> ", 0, 4);
        switch (choice)
        {
        case 1:
            ShowEnvironmentMenu();
            break;
        case 2:
            ShowConfigureMenu();
            break;
        case 3:
            ShowBuildMenu();
            break;
        case 4:
            ShowConfigSummary();
            break;
        case 0:
            m_running = false;
            break;
        default:
            std::cout << Term::Red("Invalid choice. Try again.") << "\n";
        }
    }

    // ============================================================================
    // Environment
    // ============================================================================

    void SparkBuildApp::ShowEnvironmentMenu()
    {
        bool inMenu = true;
        while (inMenu)
        {
            Term::PrintSectionHeader("Environment");

            std::cout << "  " << Term::Bold("1") << "  Check All Dependencies\n";
            std::cout << "  " << Term::Bold("2") << "  Set Engine Path\n";
            std::cout << "  " << Term::Bold("3") << "  Clone SparkEngine Repository\n";
            std::cout << "  " << Term::Bold("4") << "  Download CMake\n";
            std::cout << "  " << Term::Bold("5") << "  Initialize Git Submodules\n";
            std::cout << "  " << Term::Bold("0") << "  Back\n\n";

            int choice = Term::ReadInt("> ", 0, 5);
            switch (choice)
            {
            case 1:
                CheckAll();
                break;
            case 2:
                SetEnginePath();
                break;
            case 3:
                CloneEngine();
                break;
            case 4:
                DownloadCMake();
                break;
            case 5:
                InitSubmodules();
                break;
            case 0:
                inMenu = false;
                break;
            }
        }
    }

    void SparkBuildApp::CheckAll()
    {
        std::cout << "\n" << Term::Bold("Checking development environment...") << "\n\n";
        CheckEngineRepo();
        CheckGit();
        CheckCMake();
        CheckCompilers();
        CheckSubmodules();
        std::cout << "\n" << Term::Green("Environment check complete.") << "\n";
    }

    void SparkBuildApp::CheckEngineRepo()
    {
        std::string enginePath = m_config.config.enginePath;

        if (enginePath.empty())
        {
            // Try current directory
            if (std::filesystem::exists("CMakeLists.txt") && std::filesystem::exists("Source"))
            {
                enginePath = std::filesystem::current_path().string();
            }
        }

        if (!enginePath.empty() && std::filesystem::exists(std::filesystem::path(enginePath) / "CMakeLists.txt"))
        {
            m_config.config.enginePath = enginePath;
            Term::PrintStatus("Engine Repository", true, enginePath);
        }
        else
        {
            Term::PrintStatus("Engine Repository", false, "Not found - use 'Set Engine Path' or 'Clone'");
        }
    }

    void SparkBuildApp::CheckGit()
    {
        ProcessRunner pr;
        std::string output;
        int rc = pr.RunSync("git --version", "", output);

        if (rc == 0 && output.find("git version") != std::string::npos)
        {
            auto pos = output.find("git version ");
            std::string ver = (pos != std::string::npos) ? output.substr(pos + 12) : output;
            while (!ver.empty() && (ver.back() == '\r' || ver.back() == '\n' || ver.back() == ' '))
                ver.pop_back();
            Term::PrintStatus("Git", true, ver);
        }
        else
        {
            Term::PrintStatus("Git", false);
            ShowPackageHints("git");
        }
    }

    void SparkBuildApp::CheckCMake()
    {
        std::string cmakeCmd = "cmake";
        if (!m_config.config.cmakePath.empty())
        {
            cmakeCmd = "\"" + m_config.config.cmakePath + "\"";
        }

        ProcessRunner pr;
        std::string output;
        int rc = pr.RunSync(cmakeCmd + " --version", "", output);

        if (rc == 0 && output.find("cmake version") != std::string::npos)
        {
            auto pos = output.find("cmake version ");
            std::string ver;
            if (pos != std::string::npos)
            {
                ver = output.substr(pos + 14);
                auto nl = ver.find_first_of("\r\n");
                if (nl != std::string::npos)
                    ver = ver.substr(0, nl);
            }
            Term::PrintStatus("CMake", true, ver);
        }
        else
        {
            Term::PrintStatus("CMake", false);
            ShowPackageHints("cmake");
        }
    }

    void SparkBuildApp::CheckCompilers()
    {
        ProcessRunner pr;
        std::string output;

#ifdef SPARK_PLATFORM_WINDOWS
        // Try vswhere
        std::string vswhere = "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"";
        vswhere += " -latest -property installationVersion";
        int rc = pr.RunSync(vswhere, "", output);
        if (rc == 0 && !output.empty())
        {
            while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
                output.pop_back();
            Term::PrintStatus("C++ Compiler", true, "MSVC (VS " + output + ")");
            return;
        }

        // Try cl.exe
        rc = pr.RunSync("cl", "", output);
        if (output.find("Microsoft") != std::string::npos)
        {
            Term::PrintStatus("C++ Compiler", true, "MSVC (cl.exe)");
            return;
        }
#endif

        // Try g++
        int rc2 = pr.RunSync("g++ --version", "", output);
        if (rc2 == 0 && (output.find("g++") != std::string::npos || output.find("GCC") != std::string::npos))
        {
            auto pos = output.find('\n');
            std::string firstLine = (pos != std::string::npos) ? output.substr(0, pos) : output;
            while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == '\n'))
                firstLine.pop_back();
            Term::PrintStatus("C++ Compiler", true, firstLine);
            return;
        }

        // Try clang++
        rc2 = pr.RunSync("clang++ --version", "", output);
        if (rc2 == 0 && output.find("clang") != std::string::npos)
        {
            auto pos = output.find('\n');
            std::string firstLine = (pos != std::string::npos) ? output.substr(0, pos) : output;
            while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == '\n'))
                firstLine.pop_back();
            Term::PrintStatus("C++ Compiler", true, firstLine);
            return;
        }

        Term::PrintStatus("C++ Compiler", false);
        ShowPackageHints("compiler");
    }

    void SparkBuildApp::CheckSubmodules()
    {
        if (m_config.config.enginePath.empty())
        {
            Term::PrintStatus("Git Submodules", false, "Engine path not set");
            return;
        }

        ProcessRunner pr;
        std::string output;
        int rc = pr.RunSync("git submodule status", m_config.config.enginePath, output);

        if (rc != 0)
        {
            Term::PrintStatus("Git Submodules", false, "Not a git repository");
            return;
        }

        int total = 0, initialized = 0, missing = 0;
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.empty())
                continue;
            total++;
            if (line[0] == '-')
                missing++;
            else
                initialized++;
        }

        if (total == 0)
        {
            Term::PrintStatus("Git Submodules", true, "No submodules");
        }
        else if (missing == 0)
        {
            Term::PrintStatus("Git Submodules", true,
                              std::to_string(initialized) + "/" + std::to_string(total) + " initialized");
        }
        else
        {
            Term::PrintStatus("Git Submodules", false,
                              std::to_string(missing) + "/" + std::to_string(total) +
                                  " missing - run 'Init Submodules'");
        }
    }

    void SparkBuildApp::CloneEngine()
    {
        std::string destPath = Term::ReadLine("Clone destination directory (or press Enter for current dir): ");
        if (destPath.empty())
            destPath = ".";

        std::cout << "\n"
                  << Term::Yellow("Cloning SparkEngine to: " + destPath + SPARK_PATH_SEP + "SparkEngine ...") << "\n";
        std::cout << Term::Dim("This may take a while...") << "\n\n";

        ProcessRunner pr;
        std::string output;
        int rc = pr.RunSync("git clone --recursive https://github.com/Krilliac/SparkEngine.git", destPath, output);

        if (rc == 0)
        {
            std::string enginePath = (std::filesystem::path(destPath) / "SparkEngine").string();
            m_config.config.enginePath = enginePath;
            std::cout << Term::Green("Engine cloned successfully to: " + enginePath) << "\n";
        }
        else
        {
            std::cout << Term::Red("Clone failed:") << "\n" << output << "\n";
        }
    }

    void SparkBuildApp::DownloadCMake()
    {
        std::string destDir = ".";
        if (!m_config.config.enginePath.empty())
        {
            destDir = (std::filesystem::path(m_config.config.enginePath) / "Tools").string();
        }

#ifdef SPARK_PLATFORM_WINDOWS
        std::string url = "https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-windows-x86_64.zip";
        std::string cmakeBin =
            (std::filesystem::path(destDir) / "cmake-3.31.5-windows-x86_64" / "bin" / "cmake.exe").string();
#elif defined(SPARK_PLATFORM_MACOS)
        std::string url =
            "https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-macos-universal.tar.gz";
        std::string cmakeBin = (std::filesystem::path(destDir) / "cmake-3.31.5-macos-universal" / "CMake.app" /
                                "Contents" / "bin" / "cmake")
                                   .string();
#else
        std::string url = "https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-linux-x86_64.tar.gz";
        std::string cmakeBin =
            (std::filesystem::path(destDir) / "cmake-3.31.5-linux-x86_64" / "bin" / "cmake").string();
#endif

        std::cout << "\n" << Term::Yellow("Downloading CMake 3.31.5...") << "\n";

        bool ok = Downloader::DownloadAndExtract(url, destDir);

        if (ok)
        {
            m_config.config.cmakePath = cmakeBin;
            std::cout << Term::Green("CMake downloaded to: " + cmakeBin) << "\n";
        }
        else
        {
            std::cout << Term::Red("CMake download failed.") << "\n";
            ShowPackageHints("cmake");
        }
    }

    void SparkBuildApp::InitSubmodules()
    {
        if (m_config.config.enginePath.empty())
        {
            std::cout << Term::Red("Error: Engine path not set.") << "\n";
            return;
        }

        std::cout << "\n" << Term::Yellow("Initializing git submodules...") << "\n";
        std::cout << Term::Dim("This may take a while...") << "\n\n";

        ProcessRunner pr;
        std::string output;
        int rc = pr.RunSync("git submodule update --init --recursive", m_config.config.enginePath, output);

        if (rc == 0)
        {
            std::cout << Term::Green("Submodules initialized successfully.") << "\n";
        }
        else
        {
            std::cout << Term::Red("Submodule init failed:") << "\n" << output << "\n";
        }
    }

    void SparkBuildApp::ShowPackageHints(const std::string& package)
    {
#ifdef SPARK_PLATFORM_LINUX
        std::cout << Term::Dim("  Install hints:") << "\n";
        if (package == "git")
        {
            std::cout << Term::Dim("    Ubuntu/Debian: sudo apt install git") << "\n";
            std::cout << Term::Dim("    Fedora:        sudo dnf install git") << "\n";
            std::cout << Term::Dim("    Arch:          sudo pacman -S git") << "\n";
        }
        else if (package == "cmake")
        {
            std::cout << Term::Dim("    Ubuntu/Debian: sudo apt install cmake") << "\n";
            std::cout << Term::Dim("    Fedora:        sudo dnf install cmake") << "\n";
            std::cout << Term::Dim("    Arch:          sudo pacman -S cmake") << "\n";
        }
        else if (package == "compiler")
        {
            std::cout << Term::Dim("    Ubuntu/Debian: sudo apt install g++ build-essential") << "\n";
            std::cout << Term::Dim("    Fedora:        sudo dnf install gcc-c++ make") << "\n";
            std::cout << Term::Dim("    Arch:          sudo pacman -S gcc make") << "\n";
        }
        else if (package == "ninja")
        {
            std::cout << Term::Dim("    Ubuntu/Debian: sudo apt install ninja-build") << "\n";
            std::cout << Term::Dim("    Fedora:        sudo dnf install ninja-build") << "\n";
            std::cout << Term::Dim("    Arch:          sudo pacman -S ninja") << "\n";
        }
#elif defined(SPARK_PLATFORM_MACOS)
        std::cout << Term::Dim("  Install hints:") << "\n";
        if (package == "git" || package == "compiler")
        {
            std::cout << Term::Dim("    xcode-select --install") << "\n";
        }
        else if (package == "cmake")
        {
            std::cout << Term::Dim("    brew install cmake") << "\n";
        }
        else if (package == "ninja")
        {
            std::cout << Term::Dim("    brew install ninja") << "\n";
        }
#elif defined(SPARK_PLATFORM_WINDOWS)
        std::cout << Term::Dim("  Install hints:") << "\n";
        if (package == "git")
        {
            std::cout << Term::Dim("    Download from: https://git-scm.com") << "\n";
        }
        else if (package == "cmake")
        {
            std::cout << Term::Dim("    Download from: https://cmake.org") << "\n";
            std::cout << Term::Dim("    Or use the 'Download CMake' option") << "\n";
        }
        else if (package == "compiler")
        {
            std::cout << Term::Dim("    Install Visual Studio 2022+ with C++ workload") << "\n";
        }
#endif
    }

    // ============================================================================
    // Configure
    // ============================================================================

    void SparkBuildApp::ShowConfigureMenu()
    {
        bool inMenu = true;
        while (inMenu)
        {
            Term::PrintSectionHeader("Configure Build");

            std::cout << "  Current: " << Term::Cyan(GeneratorDisplayName(m_config.config.generator)) << " | "
                      << Term::Yellow(BuildTypeToString(m_config.config.buildType)) << "\n";
            if (!m_config.config.enginePath.empty())
                std::cout << "  Engine:  " << Term::Dim(m_config.config.enginePath) << "\n";
            if (!m_config.config.buildPath.empty())
                std::cout << "  Build:   " << Term::Dim(m_config.config.buildPath) << "\n";
            std::cout << "\n";

            std::cout << "  " << Term::Bold("1") << "  Select Generator\n";
            std::cout << "  " << Term::Bold("2") << "  Select Build Type\n";
            std::cout << "  " << Term::Bold("3") << "  Set Engine Path\n";
            std::cout << "  " << Term::Bold("4") << "  Set Build Path\n";
            std::cout << "  " << Term::Bold("5") << "  Toggle Build Options\n";
            std::cout << "  " << Term::Bold("6") << "  Apply Preset\n";
            std::cout << "  " << Term::Bold("7") << "  CMake Presets\n";
#ifdef SPARK_PLATFORM_WINDOWS
            std::cout << "  " << Term::Bold("8") << "  Set MSVC Toolset\n";
#endif
            std::cout << "  " << Term::Bold("9") << "  Set Parallel Jobs (" << m_config.config.parallelJobs << ")\n";
            std::cout << "  " << Term::Bold("0") << "  Back\n\n";

            int choice = Term::ReadInt("> ", 0, 9);
            switch (choice)
            {
            case 1:
                ShowGeneratorMenu();
                break;
            case 2:
                ShowBuildTypeMenu();
                break;
            case 3:
                SetEnginePath();
                break;
            case 4:
                SetBuildPath();
                break;
            case 5:
                ShowOptionsMenu();
                break;
            case 6:
                ShowPresetsMenu();
                break;
            case 7:
                ShowCMakePresetsMenu();
                break;
#ifdef SPARK_PLATFORM_WINDOWS
            case 8:
            {
                std::string ts = Term::ReadLine("MSVC Toolset (e.g. v143, v145): ");
                if (!ts.empty())
                    m_config.config.msvcToolset = ts;
                break;
            }
#endif
            case 9:
            {
                std::string js = Term::ReadLine("Parallel jobs (0=auto): ");
                if (!js.empty())
                {
                    try
                    {
                        m_config.config.parallelJobs = std::stoi(js);
                    }
                    catch (...)
                    {
                    }
                }
                break;
            }
            case 0:
                inMenu = false;
                break;
            }
        }
    }

    void SparkBuildApp::ShowGeneratorMenu()
    {
        auto gens = GetAvailableGenerators();
        std::cout << "\n" << Term::Bold("Select CMake Generator:") << "\n\n";

        for (int i = 0; i < (int)gens.size(); i++)
        {
            std::string marker = (gens[i] == m_config.config.generator) ? " *" : "";
            std::cout << "  " << Term::Bold(std::to_string(i + 1)) << "  " << GeneratorDisplayName(gens[i])
                      << Term::Green(marker) << "\n";
        }
        std::cout << "\n";

        int choice = Term::ReadInt("> ", 1, (int)gens.size());
        if (choice >= 1 && choice <= (int)gens.size())
        {
            m_config.config.generator = gens[choice - 1];
            std::cout << Term::Green("Generator set to: ") << GeneratorDisplayName(m_config.config.generator) << "\n";
        }
    }

    void SparkBuildApp::ShowBuildTypeMenu()
    {
        std::cout << "\n" << Term::Bold("Select Build Type:") << "\n\n";

        BuildType types[] = {BuildType::Debug, BuildType::Release, BuildType::RelWithDebInfo, BuildType::MinSizeRel};
        for (int i = 0; i < 4; i++)
        {
            std::string marker = (types[i] == m_config.config.buildType) ? " *" : "";
            std::cout << "  " << Term::Bold(std::to_string(i + 1)) << "  " << BuildTypeToString(types[i])
                      << Term::Green(marker) << "\n";
        }
        std::cout << "\n";

        int choice = Term::ReadInt("> ", 1, 4);
        if (choice >= 1 && choice <= 4)
        {
            m_config.config.buildType = types[choice - 1];
            std::cout << Term::Green("Build type set to: ") << BuildTypeToString(m_config.config.buildType) << "\n";
        }
    }

    void SparkBuildApp::ShowOptionsMenu()
    {
        OptionCategory catOrder[] = {OptionCategory::Core,      OptionCategory::Graphics,
                                     OptionCategory::Rendering, OptionCategory::EditorTools,
                                     OptionCategory::Scripting, OptionCategory::Gameplay,
                                     OptionCategory::Shipping,  OptionCategory::Experimental};

        bool inMenu = true;
        while (inMenu)
        {
            std::cout << "\n" << Term::Bold("Build Options:") << "\n\n";

            int idx = 1;
            // Build mapping from display index to options index
            std::vector<int> indexMap;

            for (auto cat : catOrder)
            {
                bool hasAny = false;
                for (const auto& opt : m_config.config.options)
                {
                    if (opt.category == cat)
                    {
                        hasAny = true;
                        break;
                    }
                }
                if (!hasAny)
                    continue;

                std::cout << "  " << Term::Bold(Term::Yellow(CategoryDisplayName(cat))) << "\n";
                for (int i = 0; i < (int)m_config.config.options.size(); i++)
                {
                    if (m_config.config.options[i].category != cat)
                        continue;
                    const auto& opt = m_config.config.options[i];
                    std::string state = opt.currentValue ? Term::Green("[ON] ") : Term::Red("[OFF]");
                    std::string num = std::to_string(idx);
                    if (idx < 10)
                        num = " " + num;
                    std::cout << "   " << Term::Bold(num) << "  " << state << " " << opt.displayName << " "
                              << Term::Dim("(" + opt.cmakeVar + ")") << "\n";
                    indexMap.push_back(i);
                    idx++;
                }
                std::cout << "\n";
            }

            std::cout << "  Enter number to toggle, or " << Term::Bold("0") << " to go back\n\n";

            int choice = Term::ReadInt("> ", 0, idx - 1);
            if (choice == 0)
            {
                inMenu = false;
            }
            else if (choice >= 1 && choice <= (int)indexMap.size())
            {
                int optIdx = indexMap[choice - 1];
                m_config.config.options[optIdx].currentValue = !m_config.config.options[optIdx].currentValue;
                std::string state = m_config.config.options[optIdx].currentValue ? "ON" : "OFF";
                std::cout << Term::Green(m_config.config.options[optIdx].displayName + " -> " + state) << "\n";
            }
        }
    }

    void SparkBuildApp::ShowPresetsMenu()
    {
        std::cout << "\n" << Term::Bold("Apply Preset:") << "\n\n";
        std::cout << "  " << Term::Bold("1") << "  All On\n";
        std::cout << "  " << Term::Bold("2") << "  All Off\n";
        std::cout << "  " << Term::Bold("3") << "  Defaults\n";
        std::cout << "  " << Term::Bold("4") << "  Minimal (Disables AI, Animation, Networking, etc.)\n";
        std::cout << "  " << Term::Bold("5") << "  Linux-Friendly (SDL2 + OpenGL)\n";
        std::cout << "  " << Term::Bold("6") << "  Shipping (Release, no editor/profiling/tests)\n";
        std::cout << "  " << Term::Bold("7") << "  Development (Debug, all tools enabled)\n";
        std::cout << "  " << Term::Bold("0") << "  Cancel\n\n";

        int choice = Term::ReadInt("> ", 0, 7);
        switch (choice)
        {
        case 1:
            m_config.ApplyPresetAllOn();
            std::cout << Term::Green("All options enabled.") << "\n";
            break;
        case 2:
            m_config.ApplyPresetAllOff();
            std::cout << Term::Green("All options disabled.") << "\n";
            break;
        case 3:
            m_config.ApplyPresetDefaults();
            std::cout << Term::Green("Defaults applied.") << "\n";
            break;
        case 4:
            m_config.ApplyPresetMinimal();
            std::cout << Term::Green("Minimal preset applied.") << "\n";
            break;
        case 5:
            m_config.ApplyPresetLinuxFriendly();
            std::cout << Term::Green("Linux-friendly preset applied.") << "\n";
            break;
        case 6:
            m_config.ApplyPresetShipping();
            std::cout << Term::Green("Shipping preset applied.") << "\n";
            break;
        case 7:
            m_config.ApplyPresetDevelopment();
            std::cout << Term::Green("Development preset applied.") << "\n";
            break;
        }
    }

    void SparkBuildApp::SetEnginePath()
    {
        std::cout << "\n  Current: " << (m_config.config.enginePath.empty() ? "(not set)" : m_config.config.enginePath)
                  << "\n";
        std::string path = Term::ReadLine("Engine path (or Enter to keep): ");
        if (!path.empty())
        {
#ifdef SPARK_PLATFORM_UNIX
            if (path[0] == '~')
            {
                const char* home = std::getenv("HOME");
                if (home)
                    path = std::string(home) + path.substr(1);
            }
#endif
            if (std::filesystem::exists(path))
            {
                m_config.config.enginePath = std::filesystem::absolute(path).string();
                std::cout << Term::Green("Engine path set to: " + m_config.config.enginePath) << "\n";
            }
            else
            {
                std::cout << Term::Red("Path does not exist: " + path) << "\n";
            }
        }
    }

    void SparkBuildApp::SetBuildPath()
    {
        std::cout << "\n  Current: " << m_config.config.buildPath << "\n";
        std::string path = Term::ReadLine("Build path (or Enter to keep): ");
        if (!path.empty())
        {
            m_config.config.buildPath = path;
            std::cout << Term::Green("Build path set to: " + path) << "\n";
        }
    }

    void SparkBuildApp::ShowCMakePresetsMenu()
    {
        auto presets = m_config.DetectCMakePresets();

        if (presets.empty())
        {
            std::cout << "\n" << Term::Yellow("No CMakePresets.json found in engine directory.") << "\n";
            if (!m_config.config.cmakePreset.empty())
            {
                std::cout << "  Current preset: " << m_config.config.cmakePreset << "\n";
                if (Term::ReadYesNo("Clear current preset?", false))
                {
                    m_config.config.cmakePreset = "";
                    std::cout << Term::Green("Preset cleared. Using manual configuration.") << "\n";
                }
            }
            return;
        }

        std::cout << "\n" << Term::Bold("Available CMake Presets:") << "\n\n";
        std::cout << "  " << Term::Bold("0") << "  (None - use manual config)\n";
        for (int i = 0; i < (int)presets.size(); i++)
        {
            std::string marker = (presets[i] == m_config.config.cmakePreset) ? " *" : "";
            std::cout << "  " << Term::Bold(std::to_string(i + 1)) << "  " << presets[i] << Term::Green(marker) << "\n";
        }
        std::cout << "\n";

        int choice = Term::ReadInt("> ", 0, (int)presets.size());
        if (choice == 0)
        {
            m_config.config.cmakePreset = "";
            std::cout << Term::Green("Using manual configuration.") << "\n";
        }
        else if (choice >= 1)
        {
            m_config.config.cmakePreset = presets[choice - 1];
            std::cout << Term::Green("Preset set to: " + m_config.config.cmakePreset) << "\n";
        }
    }

    // ============================================================================
    // Build
    // ============================================================================

    void SparkBuildApp::ShowBuildMenu()
    {
        bool inMenu = true;
        while (inMenu)
        {
            Term::PrintSectionHeader("Build");

            std::cout << "  " << Term::Bold("1") << "  Generate Project (cmake configure)\n";
            std::cout << "  " << Term::Bold("2") << "  Build Project (cmake build)\n";
            std::cout << "  " << Term::Bold("3") << "  Generate & Build\n";
            std::cout << "  " << Term::Bold("4") << "  Clean Build Directory\n";
            std::cout << "  " << Term::Bold("5") << "  Open Build Folder\n";
            std::cout << "  " << Term::Bold("6") << "  Run Engine\n";
            std::cout << "  " << Term::Bold("0") << "  Back\n\n";

            int choice = Term::ReadInt("> ", 0, 6);
            switch (choice)
            {
            case 1:
                GenerateProject();
                break;
            case 2:
                BuildProject();
                break;
            case 3:
                GenerateAndBuild();
                break;
            case 4:
                CleanBuild();
                break;
            case 5:
                OpenBuildFolder();
                break;
            case 6:
                RunEngine();
                break;
            case 0:
                inMenu = false;
                break;
            }
        }
    }

    void SparkBuildApp::GenerateProject()
    {
        std::string cmd;
        try
        {
            cmd = m_config.BuildCMakeConfigureCommand();
        }
        catch (const std::invalid_argument& error)
        {
            std::cout << "\n" << Term::Red(std::string("Configuration command rejected: ") + error.what()) << "\n";
            return;
        }

        std::cout << "\n" << Term::Dim(">>> " + cmd) << "\n\n";

        std::atomic<bool> done{false};
        std::atomic<bool> success{false};

        m_processRunner.RunAsync(
            cmd, m_config.config.enginePath, [](const std::string& line) { std::cout << "  " << line << "\n"; },
            [&done, &success](int /*exitCode*/, bool ok)
            {
                success.store(ok);
                done.store(true);
            });

        while (!done.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (success.load())
        {
            std::cout << "\n" << Term::Green("=== Configuration complete ===") << "\n";
        }
        else
        {
            std::cout << "\n" << Term::Red("=== Configuration FAILED ===") << "\n";
        }
    }

    void SparkBuildApp::BuildProject()
    {
        std::string cmd;
        try
        {
            cmd = m_config.BuildCMakeBuildCommand();
        }
        catch (const std::invalid_argument& error)
        {
            std::cout << "\n" << Term::Red(std::string("Build command rejected: ") + error.what()) << "\n";
            return;
        }

        std::cout << "\n" << Term::Dim(">>> " + cmd) << "\n\n";

        std::atomic<bool> done{false};
        std::atomic<bool> success{false};

        m_processRunner.RunAsync(
            cmd, m_config.config.enginePath, [](const std::string& line) { std::cout << "  " << line << "\n"; },
            [&done, &success](int /*exitCode*/, bool ok)
            {
                success.store(ok);
                done.store(true);
            });

        while (!done.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (success.load())
        {
            std::cout << "\n" << Term::Green("=== Build complete ===") << "\n";
        }
        else
        {
            std::cout << "\n" << Term::Red("=== Build FAILED ===") << "\n";
        }
    }

    void SparkBuildApp::GenerateAndBuild()
    {
        GenerateProject();
        if (m_processRunner.GetExitCode() == 0)
        {
            BuildProject();
        }
    }

    void SparkBuildApp::CleanBuild()
    {
        std::string buildDir = m_config.config.buildPath;
        if (buildDir.empty())
            buildDir = "build";

        if (!std::filesystem::path(buildDir).is_absolute() && !m_config.config.enginePath.empty())
        {
            buildDir = (std::filesystem::path(m_config.config.enginePath) / buildDir).string();
        }

        if (!std::filesystem::exists(buildDir))
        {
            std::cout << Term::Yellow("Build directory does not exist: " + buildDir) << "\n";
            return;
        }

        if (Term::ReadYesNo("Delete entire build directory '" + buildDir + "'?", false))
        {
            std::error_code ec;
            std::filesystem::remove_all(buildDir, ec);
            if (ec)
            {
                std::cout << Term::Red("Error cleaning: " + ec.message()) << "\n";
            }
            else
            {
                std::cout << Term::Green("Build directory cleaned.") << "\n";
            }
        }
    }

    void SparkBuildApp::OpenBuildFolder()
    {
        std::string buildDir = m_config.config.buildPath;
        if (buildDir.empty())
            buildDir = "build";
        if (!std::filesystem::path(buildDir).is_absolute() && !m_config.config.enginePath.empty())
        {
            buildDir = (std::filesystem::path(m_config.config.enginePath) / buildDir).string();
        }

        if (!std::filesystem::exists(buildDir))
        {
            std::cout << Term::Yellow("Build directory does not exist.") << "\n";
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        ShellExecuteA(nullptr, "open", buildDir.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
#elif defined(SPARK_PLATFORM_MACOS)
        (void)LaunchDetachedProcess("open", {buildDir});
#else
        (void)LaunchDetachedProcess("xdg-open", {buildDir});
#endif
        std::cout << Term::Green("Opened: " + buildDir) << "\n";
    }

    void SparkBuildApp::RunEngine()
    {
        std::string buildDir = m_config.config.buildPath;
        if (buildDir.empty())
            buildDir = "build";
        if (!std::filesystem::path(buildDir).is_absolute() && !m_config.config.enginePath.empty())
        {
            buildDir = (std::filesystem::path(m_config.config.enginePath) / buildDir).string();
        }

        std::string binDir = (std::filesystem::path(buildDir) / "bin").string();
        std::vector<std::filesystem::path> candidates;

        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(binDir))
            {
                if (!entry.is_regular_file())
                    continue;
#ifdef SPARK_PLATFORM_WINDOWS
                if (entry.path().filename() != "SparkEngine.exe")
                    continue;
#else
                if (entry.path().filename() != "SparkEngine")
                    continue;
                auto perms = std::filesystem::status(entry.path()).permissions();
                if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none)
                    continue;
#endif
                candidates.push_back(entry.path());
            }
        }
        catch (...)
        {
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& left, const auto& right) { return left.generic_string() < right.generic_string(); });

        const std::string configuration = BuildTypeToString(m_config.config.buildType);
        std::vector<std::filesystem::path> configuredCandidates;
        for (const auto& candidate : candidates)
        {
            if (std::find(candidate.begin(), candidate.end(), std::filesystem::path(configuration)) != candidate.end())
                configuredCandidates.push_back(candidate);
        }
        if (!configuredCandidates.empty())
            candidates = std::move(configuredCandidates);

        if (candidates.empty())
        {
            std::cout << Term::Red("Engine executable not found. Build the project first.") << "\n";
            return;
        }
        if (candidates.size() != 1)
        {
            std::cout << Term::Red("Multiple SparkEngine executables match the active build configuration; "
                                   "remove stale build outputs or choose a dedicated build directory.")
                      << "\n";
            for (const auto& candidate : candidates)
                std::cout << "  " << candidate.string() << "\n";
            return;
        }

        const std::string exePath = candidates.front().string();

        std::cout << Term::Green("Launching: " + exePath) << "\n";
        std::string runDir = std::filesystem::path(exePath).parent_path().string();

#ifdef SPARK_PLATFORM_WINDOWS
        ShellExecuteA(nullptr, "open", exePath.c_str(), nullptr, runDir.c_str(), SW_SHOW);
#else
        (void)LaunchDetachedProcess(exePath, {}, runDir);
#endif
    }

    // ============================================================================
    // Config summary
    // ============================================================================

    void SparkBuildApp::ShowConfigSummary()
    {
        Term::PrintSectionHeader("Configuration Summary");

        std::cout << "  " << Term::Bold("Platform:     ") << SPARK_PLATFORM_NAME << "\n";
        std::cout << "  " << Term::Bold("Engine Path:  ")
                  << (m_config.config.enginePath.empty() ? "(not set)" : m_config.config.enginePath) << "\n";
        std::cout << "  " << Term::Bold("Build Path:   ") << m_config.config.buildPath << "\n";
        std::cout << "  " << Term::Bold("CMake Path:   ")
                  << (m_config.config.cmakePath.empty() ? "(PATH)" : m_config.config.cmakePath) << "\n";
        std::cout << "  " << Term::Bold("Generator:    ") << GeneratorDisplayName(m_config.config.generator) << "\n";
        std::cout << "  " << Term::Bold("Build Type:   ") << BuildTypeToString(m_config.config.buildType) << "\n";
#ifdef SPARK_PLATFORM_WINDOWS
        std::cout << "  " << Term::Bold("MSVC Toolset: ")
                  << (m_config.config.msvcToolset.empty() ? "(default)" : m_config.config.msvcToolset) << "\n";
#endif
        std::cout << "  " << Term::Bold("Parallel:     ")
                  << (m_config.config.parallelJobs == 0 ? "auto" : std::to_string(m_config.config.parallelJobs))
                  << "\n";
        if (!m_config.config.cmakePreset.empty())
        {
            std::cout << "  " << Term::Bold("CMake Preset: ") << m_config.config.cmakePreset << "\n";
        }

        std::cout << "\n  " << Term::Bold(Term::Yellow("Build Options:")) << "\n";

        OptionCategory catOrder[] = {OptionCategory::Core,      OptionCategory::Graphics,
                                     OptionCategory::Rendering, OptionCategory::EditorTools,
                                     OptionCategory::Scripting, OptionCategory::Gameplay,
                                     OptionCategory::Shipping,  OptionCategory::Experimental};

        for (auto cat : catOrder)
        {
            bool hasAny = false;
            for (const auto& opt : m_config.config.options)
            {
                if (opt.category == cat)
                {
                    hasAny = true;
                    break;
                }
            }
            if (!hasAny)
                continue;

            std::cout << "    " << Term::Bold(CategoryDisplayName(cat)) << ": ";
            bool first = true;
            for (const auto& opt : m_config.config.options)
            {
                if (opt.category != cat)
                    continue;
                if (!first)
                    std::cout << ", ";
                if (opt.currentValue)
                    std::cout << Term::Green(opt.displayName);
                else
                    std::cout << Term::Red(opt.displayName);
                first = false;
            }
            std::cout << "\n";
        }

        try
        {
            std::cout << "\n  " << Term::Bold("CMake Configure Command:") << "\n";
            std::cout << "  " << Term::Dim(m_config.BuildCMakeConfigureCommand()) << "\n";
            std::cout << "\n  " << Term::Bold("CMake Build Command:") << "\n";
            std::cout << "  " << Term::Dim(m_config.BuildCMakeBuildCommand()) << "\n";
        }
        catch (const std::invalid_argument& error)
        {
            std::cout << "\n  " << Term::Red(std::string("Command preview rejected: ") + error.what()) << "\n";
        }
        std::cout << "\n";
    }

} // namespace SparkBuild
