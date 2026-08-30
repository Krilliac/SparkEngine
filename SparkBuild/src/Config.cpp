#include "Config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace SparkBuild
{
    namespace
    {
        std::string QuoteCommandArgument(const std::string& argument, std::string_view fieldName)
        {
            for (char character : argument)
            {
                const bool commonUnsafe =
                    character == '\0' || character == '\r' || character == '\n' || character == '"';
#ifdef SPARK_PLATFORM_WINDOWS
                // cmd.exe expands percent variables even inside quotes and may
                // expand exclamation variables when delayed expansion is on.
                const bool shellExpansion = character == '%' || character == '!';
#else
                // ProcessRunner uses /bin/sh -c. These characters retain
                // expansion or escaping semantics inside double quotes.
                const bool shellExpansion = character == '$' || character == '`' || character == '\\';
#endif
                if (commonUnsafe || shellExpansion)
                    throw std::invalid_argument("Unsafe character in SparkBuild " + std::string(fieldName));
            }

            std::string quoted;
            quoted.reserve(argument.size() + 2);
            quoted.push_back('"');
            quoted += argument;
#ifdef SPARK_PLATFORM_WINDOWS
            // Preserve trailing backslashes through CommandLineToArgvW-style
            // parsing: each one must be doubled before the closing quote.
            for (auto it = argument.rbegin(); it != argument.rend() && *it == '\\'; ++it)
                quoted.push_back('\\');
#endif
            quoted.push_back('"');
            return quoted;
        }
    } // namespace

    const char* GeneratorToString(Generator gen)
    {
        switch (gen)
        {
        case Generator::VS2022:
            return "Visual Studio 17 2022";
        case Generator::VS2026:
            return "Visual Studio 18 2026";
        case Generator::Ninja:
            return "Ninja";
        case Generator::NinjaMultiConfig:
            return "Ninja Multi-Config";
        case Generator::UnixMakefiles:
            return "Unix Makefiles";
        case Generator::Xcode:
            return "Xcode";
        }
        return "Ninja";
    }

    const char* GeneratorDisplayName(Generator gen)
    {
        switch (gen)
        {
        case Generator::VS2022:
            return "Visual Studio 2022";
        case Generator::VS2026:
            return "Visual Studio 2026";
        case Generator::Ninja:
            return "Ninja";
        case Generator::NinjaMultiConfig:
            return "Ninja Multi-Config";
        case Generator::UnixMakefiles:
            return "Unix Makefiles (Make)";
        case Generator::Xcode:
            return "Xcode";
        }
        return "Ninja";
    }

    const char* BuildTypeToString(BuildType bt)
    {
        switch (bt)
        {
        case BuildType::Debug:
            return "Debug";
        case BuildType::Release:
            return "Release";
        case BuildType::RelWithDebInfo:
            return "RelWithDebInfo";
        case BuildType::MinSizeRel:
            return "MinSizeRel";
        }
        return "Release";
    }

    const char* CategoryDisplayName(OptionCategory cat)
    {
        switch (cat)
        {
        case OptionCategory::Core:
            return "Core Systems";
        case OptionCategory::Graphics:
            return "Graphics Backends";
        case OptionCategory::Rendering:
            return "Rendering & Effects";
        case OptionCategory::EditorTools:
            return "Editor & Tools";
        case OptionCategory::Scripting:
            return "Scripting";
        case OptionCategory::Gameplay:
            return "Gameplay Systems";
        case OptionCategory::Shipping:
            return "Shipping & Deployment";
        case OptionCategory::Experimental:
            return "Experimental";
        }
        return "Other";
    }

    std::vector<Generator> GetAvailableGenerators()
    {
        std::vector<Generator> gens;
#ifdef SPARK_PLATFORM_WINDOWS
        gens.push_back(Generator::VS2022);
        gens.push_back(Generator::VS2026);
        gens.push_back(Generator::Ninja);
        gens.push_back(Generator::NinjaMultiConfig);
        gens.push_back(Generator::UnixMakefiles);
#elif defined(SPARK_PLATFORM_MACOS)
        gens.push_back(Generator::Ninja);
        gens.push_back(Generator::NinjaMultiConfig);
        gens.push_back(Generator::Xcode);
        gens.push_back(Generator::UnixMakefiles);
#else
        gens.push_back(Generator::Ninja);
        gens.push_back(Generator::NinjaMultiConfig);
        gens.push_back(Generator::UnixMakefiles);
#endif
        return gens;
    }

    Generator GetDefaultGenerator()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return Generator::VS2022;
#else
        return Generator::Ninja;
#endif
    }

    ConfigManager::ConfigManager()
    {
        InitDefaults();
    }

    void ConfigManager::InitDefaults()
    {
        config.options.clear();

        // ========================================================================
        // Build options matching SparkEngine's CMakeLists.txt exactly
        // See: https://github.com/Krilliac/SparkEngine/blob/Working/CMakeLists.txt
        // ========================================================================

        // Core systems
        config.options.push_back(
            {"ENABLE_GRAPHICS", "Graphics Engine", "Build the graphics engine", true, true, OptionCategory::Core});
        config.options.push_back({"ENABLE_RECAST", "Recast Navigation",
                                  "Build the Recast/Detour navigation implementation", true, true,
                                  OptionCategory::Core});
        config.options.push_back({"SPARK_HEADLESS_SUPPORT", "Headless Mode",
                                  "Enable headless/dedicated-server runtime support", true, true,
                                  OptionCategory::Core});
        config.options.push_back({"SPARK_DOUBLE_PRECISION_PHYSICS", "Double-Precision Physics",
                                  "Use double-precision Jolt physics for large worlds", false, false,
                                  OptionCategory::Core});

        // Graphics backends and rendering breadth
        config.options.push_back({"ENABLE_VULKAN", "Vulkan Backend", "Build the experimental Vulkan backend", true,
                                  true, OptionCategory::Graphics});
        config.options.push_back({"ENABLE_OPENGL", "OpenGL Backend", "Build the experimental OpenGL backend", true,
                                  true, OptionCategory::Graphics});
        config.options.push_back({"ENABLE_SDL2", "SDL2 Windowing", "Build SDL2 windowing and input support", false,
                                  false, OptionCategory::Graphics});
        config.options.push_back({"ENABLE_METAL", "Metal Backend", "Build the experimental macOS Metal backend", false,
                                  false, OptionCategory::Graphics});
        config.options.push_back({"ENABLE_DXR", "DirectX Raytracing",
                                  "Build DirectX Raytracing support for the D3D12 path", true, true,
                                  OptionCategory::Graphics});
        config.options.push_back({"ENABLE_HYBRID_RT", "Hybrid Ray Tracing",
                                  "Build hybrid software and hardware ray-tracing support", true, true,
                                  OptionCategory::Rendering});
        config.options.push_back({"ENABLE_NEURAL_RENDERING", "Neural Rendering",
                                  "Build experimental neural-rendering features", true, true,
                                  OptionCategory::Rendering});

        // Editor, build products, and validation tools
        config.options.push_back({"ENABLE_EDITOR", "Editor", "Build the SparkEditor authoring application", true, true,
                                  OptionCategory::EditorTools});
        config.options.push_back({"ENABLE_PROFILING", "Profiling Tools", "Build profiling instrumentation", true, true,
                                  OptionCategory::EditorTools});
        config.options.push_back(
            {"BUILD_TESTS", "Unit Tests", "Build the CTest test suite", true, true, OptionCategory::EditorTools});
        config.options.push_back({"BUILD_GAME_MODULES", "Game Modules", "Build the in-tree game modules", true, true,
                                  OptionCategory::EditorTools});
        config.options.push_back(
            {"ENABLE_LAUNCHER", "Launcher", "Build SparkLauncher", true, true, OptionCategory::EditorTools});
        config.options.push_back(
            {"ENABLE_SPARKBUILD", "Build Configurator", "Build SparkBuild", true, true, OptionCategory::EditorTools});
        config.options.push_back(
            {"ENABLE_INSTALLER", "Installer", "Build SparkInstaller", true, true, OptionCategory::EditorTools});
        config.options.push_back({"ENABLE_ASSET_PIPELINE_TOOLS", "Asset Pipeline Tools",
                                  "Build SparkCooker and SparkWorker", true, true, OptionCategory::EditorTools});
        config.options.push_back({"ENABLE_AUTOMATION_HOST", "Automation Host", "Build SparkAutomation", true, true,
                                  OptionCategory::EditorTools});
        config.options.push_back({"SPARK_SUPPRESS_THIRDPARTY_WARNINGS", "Suppress Third-Party Warnings",
                                  "Treat third-party headers as external/system headers", true, true,
                                  OptionCategory::EditorTools});

        // Scripting
        config.options.push_back({"ENABLE_ANGELSCRIPT", "AngelScript",
                                  "Build the experimental AngelScript gameplay runtime", true, true,
                                  OptionCategory::Scripting});

        // Shipping and deployment
        config.options.push_back({"SPARK_NATIVE_ARCH", "Native CPU Tuning",
                                  "Tune for the build machine instead of distributed CPUs", false, false,
                                  OptionCategory::Shipping});
        config.options.push_back({"SPARK_STRICT_DEPS", "Strict Dependencies",
                                  "Fail configuration when a critical dependency is missing", false, false,
                                  OptionCategory::Shipping});
        config.options.push_back({"ENABLE_CONSOLE_IN_SHIPPING", "Console in Shipping",
                                  "Include SparkConsole in Shipping builds", false, false, OptionCategory::Shipping});
        config.options.push_back({"ENABLE_DEVCOMMANDS_IN_SHIPPING", "Dev Commands in Shipping",
                                  "Include dev commands in Shipping builds", false, false, OptionCategory::Shipping});
        config.options.push_back({"STRIP_DEBUG_SYMBOLS", "Strip Debug Symbols", "Strip debug symbols from binaries",
                                  false, false, OptionCategory::Shipping});

        // Experimental and out-of-profile products
        config.options.push_back({"ENABLE_NETWORKING", "Networking", "Build UDP networking features", true, true,
                                  OptionCategory::Experimental});
        config.options.push_back({"ENABLE_SERVER_PROCESSES", "Server Processes",
                                  "Build dedicated server, gateway, and orchestration processes", true, true,
                                  OptionCategory::Experimental});
        config.options.push_back({"ENABLE_VR", "VR/AR Framework", "Build the experimental OpenXR-ready framework",
                                  false, false, OptionCategory::Experimental});
        config.options.push_back({"ENABLE_MOBILE", "Mobile Framework", "Build experimental mobile platform support",
                                  false, false, OptionCategory::Experimental});

        // Set platform-appropriate defaults
        config.buildPath = "build";
        config.parallelJobs = 0;
        config.generator = GetDefaultGenerator();
        config.buildType = BuildType::Release;

#ifdef SPARK_PLATFORM_LINUX
        // On Linux, enable SDL2 by default (matches engine CMakeLists.txt)
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_SDL2")
            {
                opt.defaultValue = true;
                opt.currentValue = true;
            }
            // DXR is Windows-only
            if (opt.cmakeVar == "ENABLE_DXR")
            {
                opt.defaultValue = false;
                opt.currentValue = false;
            }
        }
#elif defined(SPARK_PLATFORM_MACOS)
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_SDL2")
            {
                opt.defaultValue = true;
                opt.currentValue = true;
            }
            if (opt.cmakeVar == "ENABLE_DXR")
            {
                opt.defaultValue = false;
                opt.currentValue = false;
            }
            if (opt.cmakeVar == "ENABLE_METAL")
            {
                opt.defaultValue = true;
                opt.currentValue = true;
            }
        }
#endif
    }

    void ConfigManager::ApplyPresetAllOn()
    {
        for (auto& opt : config.options)
            opt.currentValue = true;
    }

    void ConfigManager::ApplyPresetAllOff()
    {
        for (auto& opt : config.options)
            opt.currentValue = false;
    }

    void ConfigManager::ApplyPresetDefaults()
    {
        for (auto& opt : config.options)
            opt.currentValue = opt.defaultValue;
    }

    void ConfigManager::ApplyPresetMinimal()
    {
        // Matches the "minimal" preset from SparkEngine's CMakePresets.json
        ApplyPresetDefaults();
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_DXR" || opt.cmakeVar == "ENABLE_HYBRID_RT" ||
                opt.cmakeVar == "ENABLE_NEURAL_RENDERING" || opt.cmakeVar == "ENABLE_ANGELSCRIPT" ||
                opt.cmakeVar == "ENABLE_NETWORKING" || opt.cmakeVar == "ENABLE_SERVER_PROCESSES" ||
                opt.cmakeVar == "ENABLE_EDITOR" || opt.cmakeVar == "ENABLE_LAUNCHER" ||
                opt.cmakeVar == "ENABLE_SPARKBUILD" || opt.cmakeVar == "ENABLE_INSTALLER" ||
                opt.cmakeVar == "ENABLE_ASSET_PIPELINE_TOOLS" || opt.cmakeVar == "ENABLE_AUTOMATION_HOST" ||
                opt.cmakeVar == "ENABLE_PROFILING" || opt.cmakeVar == "BUILD_GAME_MODULES" ||
                opt.cmakeVar == "BUILD_TESTS")
            {
                opt.currentValue = false;
            }
        }
    }

    void ConfigManager::ApplyPresetLinuxFriendly()
    {
        ApplyPresetDefaults();
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_SDL2" || opt.cmakeVar == "ENABLE_OPENGL")
                opt.currentValue = true;
            if (opt.cmakeVar == "ENABLE_DXR")
                opt.currentValue = false;
        }
        config.generator = Generator::Ninja;
    }

    void ConfigManager::ApplyPresetShipping()
    {
        // Matches the "shipping" presets from SparkEngine's CMakePresets.json
        ApplyPresetDefaults();
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_PROFILING" || opt.cmakeVar == "ENABLE_CONSOLE_IN_SHIPPING" ||
                opt.cmakeVar == "ENABLE_DEVCOMMANDS_IN_SHIPPING" || opt.cmakeVar == "BUILD_TESTS" ||
                opt.cmakeVar == "ENABLE_DXR" || opt.cmakeVar == "ENABLE_HYBRID_RT" ||
                opt.cmakeVar == "ENABLE_NEURAL_RENDERING" || opt.cmakeVar == "ENABLE_ANGELSCRIPT" ||
                opt.cmakeVar == "ENABLE_NETWORKING" || opt.cmakeVar == "ENABLE_SERVER_PROCESSES" ||
                opt.cmakeVar == "ENABLE_VULKAN" || opt.cmakeVar == "ENABLE_OPENGL" || opt.cmakeVar == "ENABLE_SDL2" ||
                opt.cmakeVar == "ENABLE_METAL" || opt.cmakeVar == "ENABLE_VR" || opt.cmakeVar == "ENABLE_MOBILE" ||
                opt.cmakeVar == "SPARK_NATIVE_ARCH")
            {
                opt.currentValue = false;
            }
            if (opt.cmakeVar == "STRIP_DEBUG_SYMBOLS" || opt.cmakeVar == "SPARK_STRICT_DEPS" ||
                opt.cmakeVar == "ENABLE_EDITOR" || opt.cmakeVar == "BUILD_GAME_MODULES" ||
                opt.cmakeVar == "ENABLE_LAUNCHER" || opt.cmakeVar == "ENABLE_SPARKBUILD" ||
                opt.cmakeVar == "ENABLE_INSTALLER" || opt.cmakeVar == "ENABLE_ASSET_PIPELINE_TOOLS" ||
                opt.cmakeVar == "ENABLE_AUTOMATION_HOST")
            {
                opt.currentValue = true;
            }
        }
        config.buildType = BuildType::MinSizeRel;
    }

    void ConfigManager::ApplyPresetDevelopment()
    {
        // Full-featured development build with all tools enabled
        ApplyPresetDefaults();
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_EDITOR" || opt.cmakeVar == "ENABLE_PROFILING" ||
                opt.cmakeVar == "BUILD_TESTS" || opt.cmakeVar == "BUILD_GAME_MODULES" ||
                opt.cmakeVar == "ENABLE_LAUNCHER" || opt.cmakeVar == "ENABLE_SPARKBUILD" ||
                opt.cmakeVar == "ENABLE_INSTALLER" || opt.cmakeVar == "ENABLE_ASSET_PIPELINE_TOOLS" ||
                opt.cmakeVar == "ENABLE_AUTOMATION_HOST" || opt.cmakeVar == "SPARK_NATIVE_ARCH")
            {
                opt.currentValue = true;
            }
            if (opt.cmakeVar == "STRIP_DEBUG_SYMBOLS" || opt.cmakeVar == "ENABLE_CONSOLE_IN_SHIPPING" ||
                opt.cmakeVar == "ENABLE_DEVCOMMANDS_IN_SHIPPING")
            {
                opt.currentValue = false;
            }
        }
        config.buildType = BuildType::RelWithDebInfo;
    }

    static std::string Trim(const std::string& s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    bool ConfigManager::Load(const std::string& iniPath)
    {
        std::ifstream file(iniPath);
        if (!file.is_open())
            return false;

        BuildConfig parsed = config;
        std::string line, currentSection;
        std::unordered_set<std::string> seenKeys;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;

            if (line[0] == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.size() - 2);
                if (currentSection != "Paths" && currentSection != "Build" && currentSection != "Options")
                    return false;
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos || currentSection.empty())
                return false;
            std::string key = Trim(line.substr(0, eq));
            std::string val = Trim(line.substr(eq + 1));
            if (key.empty() || !seenKeys.insert(currentSection + "\n" + key).second)
                return false;

            if (currentSection == "Paths")
            {
                if (key == "EnginePath")
                    parsed.enginePath = val;
                else if (key == "BuildPath")
                    parsed.buildPath = val;
                else if (key == "CMakePath")
                    parsed.cmakePath = val;
                else
                    return false;
            }
            else if (currentSection == "Build")
            {
                if (key == "Generator")
                {
                    if (val == "VS2022")
                        parsed.generator = Generator::VS2022;
                    else if (val == "VS2026")
                        parsed.generator = Generator::VS2026;
                    else if (val == "Ninja")
                        parsed.generator = Generator::Ninja;
                    else if (val == "NinjaMultiConfig")
                        parsed.generator = Generator::NinjaMultiConfig;
                    else if (val == "UnixMakefiles")
                        parsed.generator = Generator::UnixMakefiles;
                    else if (val == "Xcode")
                        parsed.generator = Generator::Xcode;
                    else
                        return false;
                }
                else if (key == "BuildType")
                {
                    if (val == "Debug")
                        parsed.buildType = BuildType::Debug;
                    else if (val == "Release")
                        parsed.buildType = BuildType::Release;
                    else if (val == "RelWithDebInfo")
                        parsed.buildType = BuildType::RelWithDebInfo;
                    else if (val == "MinSizeRel")
                        parsed.buildType = BuildType::MinSizeRel;
                    else
                        return false;
                }
                else if (key == "MSVCToolset")
                {
                    parsed.msvcToolset = val;
                }
                else if (key == "ParallelJobs")
                {
                    try
                    {
                        size_t consumed = 0;
                        const int jobs = std::stoi(val, &consumed);
                        if (consumed != val.size() || jobs < 0)
                            return false;
                        parsed.parallelJobs = jobs;
                    }
                    catch (const std::exception&)
                    {
                        return false;
                    }
                }
                else if (key == "CMakePreset")
                {
                    parsed.cmakePreset = val;
                }
                else
                    return false;
            }
            else if (currentSection == "Options")
            {
                auto option = std::find_if(parsed.options.begin(), parsed.options.end(),
                                           [&](const BuildOption& item) { return item.cmakeVar == key; });
                if (option == parsed.options.end())
                    return false;
                if (val == "1" || val == "ON" || val == "true")
                    option->currentValue = true;
                else if (val == "0" || val == "OFF" || val == "false")
                    option->currentValue = false;
                else
                {
                    return false;
                }
            }
        }
        config = std::move(parsed);
        return true;
    }

    bool ConfigManager::Save(const std::string& iniPath) const
    {
        // Create parent directory if needed
        auto parent = std::filesystem::path(iniPath).parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(iniPath);
        if (!file.is_open())
            return false;

        file << "; SparkBuild Configuration\n";
        file << "; Auto-generated by SparkBuild\n\n";

        file << "[Paths]\n";
        file << "EnginePath=" << config.enginePath << "\n";
        file << "BuildPath=" << config.buildPath << "\n";
        file << "CMakePath=" << config.cmakePath << "\n\n";

        file << "[Build]\n";
        switch (config.generator)
        {
        case Generator::VS2022:
            file << "Generator=VS2022\n";
            break;
        case Generator::VS2026:
            file << "Generator=VS2026\n";
            break;
        case Generator::Ninja:
            file << "Generator=Ninja\n";
            break;
        case Generator::NinjaMultiConfig:
            file << "Generator=NinjaMultiConfig\n";
            break;
        case Generator::UnixMakefiles:
            file << "Generator=UnixMakefiles\n";
            break;
        case Generator::Xcode:
            file << "Generator=Xcode\n";
            break;
        }
        file << "BuildType=" << BuildTypeToString(config.buildType) << "\n";
        file << "MSVCToolset=" << config.msvcToolset << "\n";
        file << "ParallelJobs=" << config.parallelJobs << "\n";
        file << "CMakePreset=" << config.cmakePreset << "\n\n";

        file << "[Options]\n";
        for (const auto& opt : config.options)
        {
            file << opt.cmakeVar << "=" << (opt.currentValue ? "ON" : "OFF") << "\n";
        }

        return true;
    }

    std::string ConfigManager::BuildCMakeConfigureCommand() const
    {
        std::string cmake = config.cmakePath.empty() ? "cmake" : QuoteCommandArgument(config.cmakePath, "CMakePath");
        const std::string sourcePath = config.enginePath.empty() ? "." : config.enginePath;

        // If using a preset, the command is much simpler
        if (!config.cmakePreset.empty())
        {
            return cmake + " --preset " + QuoteCommandArgument(config.cmakePreset, "CMakePreset") + " -S " +
                   QuoteCommandArgument(sourcePath, "EnginePath");
        }

        std::string buildDir = config.buildPath.empty() ? "build" : config.buildPath;

        std::string cmd = cmake + " -S " + QuoteCommandArgument(sourcePath, "EnginePath") + " -B " +
                          QuoteCommandArgument(buildDir, "BuildPath");
        cmd += " -G " + QuoteCommandArgument(GeneratorToString(config.generator), "Generator");

        // Build type (for single-config generators like Ninja/Makefiles)
        bool singleConfig = (config.generator == Generator::Ninja || config.generator == Generator::UnixMakefiles);
        if (singleConfig)
        {
            cmd += " -DCMAKE_BUILD_TYPE=" + std::string(BuildTypeToString(config.buildType));
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // MSVC toolset override
        if (!config.msvcToolset.empty() &&
            (config.generator == Generator::VS2022 || config.generator == Generator::VS2026))
        {
            // CMake selects a Visual Studio toolset when the build tree is
            // created. A cache variable cannot change that generator input.
            cmd += " -T " + QuoteCommandArgument(config.msvcToolset, "MSVCToolset");
        }

        // Platform for VS generators
        if (config.generator == Generator::VS2022 || config.generator == Generator::VS2026)
        {
            cmd += " -A x64";
        }
#endif

        // All build options
        for (const auto& opt : config.options)
        {
            cmd += " " +
                   QuoteCommandArgument("-D" + opt.cmakeVar + "=" + (opt.currentValue ? "ON" : "OFF"), "CMake option");
        }

        return cmd;
    }

    std::string ConfigManager::BuildCMakeBuildCommand() const
    {
        std::string cmake = config.cmakePath.empty() ? "cmake" : QuoteCommandArgument(config.cmakePath, "CMakePath");
        std::string buildDir = config.buildPath.empty() ? "build" : config.buildPath;

        std::string cmd = cmake + " --build " + QuoteCommandArgument(buildDir, "BuildPath");
        cmd += " --config " + std::string(BuildTypeToString(config.buildType));

        if (config.parallelJobs > 0)
        {
            cmd += " --parallel " + std::to_string(config.parallelJobs);
        }
        else
        {
            cmd += " --parallel";
        }

        return cmd;
    }

    std::string ConfigManager::GetDefaultIniPath()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // Try next to the exe first
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        auto pos = path.find_last_of("\\/");
        if (pos != std::string::npos)
        {
            path = path.substr(0, pos + 1);
        }
        return path + "sparkbuild.ini";
#else
        // Use XDG config dir on Unix, falling back to ~/.config
        const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        std::string configDir;
        if (xdgConfig && xdgConfig[0] != '\0')
        {
            configDir = std::string(xdgConfig) + "/sparkbuild";
        }
        else
        {
            const char* home = std::getenv("HOME");
            if (home)
            {
                configDir = std::string(home) + "/.config/sparkbuild";
            }
            else
            {
                configDir = ".";
            }
        }
        return configDir + "/sparkbuild.ini";
#endif
    }

    std::vector<std::string> ConfigManager::DetectCMakePresets() const
    {
        std::vector<std::string> presets;
        if (config.enginePath.empty())
            return presets;

        std::string presetsFile = config.enginePath + SPARK_PATH_SEP + "CMakePresets.json";
        if (!std::filesystem::exists(presetsFile))
            return presets;

        // Simple JSON parsing to extract preset names
        std::ifstream file(presetsFile);
        if (!file.is_open())
            return presets;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Look for "name": "value" patterns in configurePresets
        size_t pos = content.find("\"configurePresets\"");
        if (pos == std::string::npos)
            return presets;

        while ((pos = content.find("\"name\"", pos)) != std::string::npos)
        {
            pos = content.find("\"", pos + 6);
            if (pos == std::string::npos)
                break;
            pos++;
            size_t end = content.find("\"", pos);
            if (end == std::string::npos)
                break;
            presets.push_back(content.substr(pos, end - pos));
            pos = end + 1;
        }

        return presets;
    }

} // namespace SparkBuild
