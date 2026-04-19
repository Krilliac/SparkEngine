#include "Config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

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
        // See: https://github.com/Krilliac/SparkEngine/blob/master/CMakeLists.txt
        // ========================================================================

        // Core Systems
        config.options.push_back({"ENABLE_GRAPHICS", "Graphics Engine",
                                  "DirectX 11 renderer (Windows) / OpenGL (Linux/macOS)", true, true,
                                  OptionCategory::Core});
        config.options.push_back(
            {"ENABLE_PHYSX", "Physics (Bullet)", "Bullet Physics 3D physics engine", true, true, OptionCategory::Core});
        config.options.push_back({"ENABLE_AI", "AI & Navigation",
                                  "Behavior trees, NavMesh pathfinding, perception system", true, true,
                                  OptionCategory::Core});
        config.options.push_back({"ENABLE_ANIMATION", "Skeletal Animation",
                                  "Skeletal animation, IK, state machines, blending", true, true,
                                  OptionCategory::Core});
        config.options.push_back({"ENABLE_SAVE_SYSTEM", "Save/Load System",
                                  "JSON serialization with compression (miniz)", true, true, OptionCategory::Core});
        config.options.push_back({"ENABLE_ADVANCED_INPUT", "Advanced Input", "Extended input: keyboard, mouse, gamepad",
                                  true, true, OptionCategory::Core});
        config.options.push_back({"ENABLE_ASSET_STREAMING", "Asset Streaming", "Runtime asset streaming and loading",
                                  true, true, OptionCategory::Core});

        // Graphics Backends
        config.options.push_back({"ENABLE_VULKAN", "Vulkan Backend",
                                  "Vulkan graphics backend (experimental, cross-platform)", true, true,
                                  OptionCategory::Graphics});
        config.options.push_back({"ENABLE_OPENGL", "OpenGL Backend",
                                  "OpenGL 4.5 backend (experimental, cross-platform)", true, true,
                                  OptionCategory::Graphics});
        config.options.push_back({"ENABLE_DXR", "DirectX Raytracing", "DXR support (requires D3D12, Windows only)",
                                  false, false, OptionCategory::Graphics});

        // Rendering & Effects
        config.options.push_back({"ENABLE_POST_PROCESSING", "Post-Processing", "Bloom, tone mapping, FXAA effects",
                                  true, true, OptionCategory::Rendering});
        config.options.push_back({"ENABLE_LIGHTING_SYSTEM", "Advanced Lighting", "PBR lighting, IBL, shadow mapping",
                                  true, true, OptionCategory::Rendering});
        config.options.push_back({"ENABLE_DECALS", "Decal System", "Projected decals for impacts and effects", true,
                                  true, OptionCategory::Rendering});
        config.options.push_back(
            {"ENABLE_MESH_LOD", "Mesh LOD", "Mesh level-of-detail system", true, true, OptionCategory::Rendering});
        config.options.push_back(
            {"ENABLE_FOG_SYSTEM", "Fog System", "Fog rendering system", true, true, OptionCategory::Rendering});
        config.options.push_back({"ENABLE_SCREEN_SPACE", "Screen-Space Effects", "Screen-space effects (SSAO, SSR)",
                                  true, true, OptionCategory::Rendering});

        // Editor & Tools
        config.options.push_back({"ENABLE_EDITOR", "Editor", "ImGui visual editor (Windows/Linux)", true, true,
                                  OptionCategory::EditorTools});
        config.options.push_back({"ENABLE_PROFILING", "Profiling Tools",
                                  "Performance profiling, timers, memory tracking", true, true,
                                  OptionCategory::EditorTools});
        config.options.push_back({"ENABLE_PERF_STATS", "Performance Stats", "Performance statistics overlay", true,
                                  true, OptionCategory::EditorTools});
        config.options.push_back(
            {"BUILD_TESTS", "Unit Tests", "Build CTest unit test suite", true, true, OptionCategory::EditorTools});

        // Scripting
        config.options.push_back({"ENABLE_LUA", "Lua Scripting", "Lua scripting support (Sol2 bindings)", true, true,
                                  OptionCategory::Scripting});
        config.options.push_back({"ENABLE_HOT_RELOAD", "Hot Reload", "Game module hot-reload during development", true,
                                  true, OptionCategory::Scripting});

        // Gameplay Systems
        config.options.push_back({"ENABLE_TERRAIN_SYSTEM", "Terrain System", "Heightmap terrain with LOD and erosion",
                                  true, true, OptionCategory::Gameplay});
        config.options.push_back({"ENABLE_PROCEDURAL", "Procedural Generation",
                                  "Noise, erosion, mesh gen, WFC algorithms", true, true, OptionCategory::Gameplay});
        config.options.push_back({"ENABLE_CINEMATIC", "Cinematic Sequencer", "Cinematic sequence and cutscene system",
                                  true, true, OptionCategory::Gameplay});
        config.options.push_back(
            {"ENABLE_WEATHER", "Weather System", "Dynamic weather system", true, true, OptionCategory::Gameplay});
        config.options.push_back({"ENABLE_INVENTORY", "Inventory System", "Item inventory management system", true,
                                  true, OptionCategory::Gameplay});
        config.options.push_back({"ENABLE_QUEST_SYSTEM", "Quest System", "Quest and objective tracking system", true,
                                  true, OptionCategory::Gameplay});
        config.options.push_back({"ENABLE_EVENT_SYSTEM", "Event System", "Publish/subscribe event bus", true, true,
                                  OptionCategory::Gameplay});
        config.options.push_back({"ENABLE_DAY_NIGHT", "Day/Night Cycle", "Dynamic day/night cycle system", true, true,
                                  OptionCategory::Gameplay});

        // Shipping & Deployment
        config.options.push_back({"SPARK_HEADLESS_SUPPORT", "Headless Mode", "Headless/dedicated server mode support",
                                  true, true, OptionCategory::Shipping});
        config.options.push_back({"ENABLE_CONSOLE_IN_SHIPPING", "Console in Shipping",
                                  "Include SparkConsole in Shipping builds", false, false, OptionCategory::Shipping});
        config.options.push_back({"ENABLE_DEVCOMMANDS_IN_SHIPPING", "Dev Commands in Shipping",
                                  "Include dev commands in Shipping builds", false, false, OptionCategory::Shipping});
        config.options.push_back({"STRIP_DEBUG_SYMBOLS", "Strip Debug Symbols", "Strip debug symbols from binaries",
                                  false, false, OptionCategory::Shipping});

        // Experimental
        config.options.push_back({"ENABLE_NETWORKING", "Networking",
                                  "UDP multiplayer, lag compensation (requires curl)", false, false,
                                  OptionCategory::Experimental});
        config.options.push_back({"ENABLE_SDL2", "SDL2 Windowing", "SDL2 cross-platform windowing and input", false,
                                  false, OptionCategory::Experimental});
        config.options.push_back({"ENABLE_COLLABORATIVE", "Collaborative Editing", "Collaborative editing features",
                                  true, true, OptionCategory::Experimental});

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
            if (opt.cmakeVar == "ENABLE_AI" || opt.cmakeVar == "ENABLE_ANIMATION" ||
                opt.cmakeVar == "ENABLE_NETWORKING" || opt.cmakeVar == "ENABLE_SAVE_SYSTEM" ||
                opt.cmakeVar == "ENABLE_PROCEDURAL" || opt.cmakeVar == "ENABLE_CINEMATIC" ||
                opt.cmakeVar == "ENABLE_DECALS" || opt.cmakeVar == "ENABLE_MESH_LOD" || opt.cmakeVar == "ENABLE_DXR")
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
            if (opt.cmakeVar == "ENABLE_EDITOR" || opt.cmakeVar == "ENABLE_PROFILING" ||
                opt.cmakeVar == "ENABLE_HOT_RELOAD" || opt.cmakeVar == "ENABLE_CONSOLE_IN_SHIPPING" ||
                opt.cmakeVar == "ENABLE_DEVCOMMANDS_IN_SHIPPING" || opt.cmakeVar == "BUILD_TESTS")
            {
                opt.currentValue = false;
            }
            if (opt.cmakeVar == "STRIP_DEBUG_SYMBOLS")
            {
                opt.currentValue = true;
            }
        }
        config.buildType = BuildType::Release;
    }

    void ConfigManager::ApplyPresetDevelopment()
    {
        // Full-featured development build with all tools enabled
        ApplyPresetDefaults();
        for (auto& opt : config.options)
        {
            if (opt.cmakeVar == "ENABLE_EDITOR" || opt.cmakeVar == "ENABLE_PROFILING" ||
                opt.cmakeVar == "ENABLE_HOT_RELOAD" || opt.cmakeVar == "ENABLE_PERF_STATS" ||
                opt.cmakeVar == "BUILD_TESTS")
            {
                opt.currentValue = true;
            }
            if (opt.cmakeVar == "STRIP_DEBUG_SYMBOLS" || opt.cmakeVar == "ENABLE_CONSOLE_IN_SHIPPING" ||
                opt.cmakeVar == "ENABLE_DEVCOMMANDS_IN_SHIPPING")
            {
                opt.currentValue = false;
            }
        }
        config.buildType = BuildType::Debug;
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

        std::string line, currentSection;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;

            if (line[0] == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.size() - 2);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = Trim(line.substr(0, eq));
            std::string val = Trim(line.substr(eq + 1));

            if (currentSection == "Paths")
            {
                if (key == "EnginePath")
                    config.enginePath = val;
                else if (key == "BuildPath")
                    config.buildPath = val;
                else if (key == "CMakePath")
                    config.cmakePath = val;
            }
            else if (currentSection == "Build")
            {
                if (key == "Generator")
                {
                    if (val == "VS2022")
                        config.generator = Generator::VS2022;
                    else if (val == "VS2026")
                        config.generator = Generator::VS2026;
                    else if (val == "Ninja")
                        config.generator = Generator::Ninja;
                    else if (val == "NinjaMultiConfig")
                        config.generator = Generator::NinjaMultiConfig;
                    else if (val == "UnixMakefiles")
                        config.generator = Generator::UnixMakefiles;
                    else if (val == "Xcode")
                        config.generator = Generator::Xcode;
                }
                else if (key == "BuildType")
                {
                    if (val == "Debug")
                        config.buildType = BuildType::Debug;
                    else if (val == "Release")
                        config.buildType = BuildType::Release;
                    else if (val == "RelWithDebInfo")
                        config.buildType = BuildType::RelWithDebInfo;
                    else if (val == "MinSizeRel")
                        config.buildType = BuildType::MinSizeRel;
                }
                else if (key == "MSVCToolset")
                {
                    config.msvcToolset = val;
                }
                else if (key == "ParallelJobs")
                {
                    config.parallelJobs = std::atoi(val.c_str());
                }
                else if (key == "CMakePreset")
                {
                    config.cmakePreset = val;
                }
            }
            else if (currentSection == "Options")
            {
                for (auto& opt : config.options)
                {
                    if (opt.cmakeVar == key)
                    {
                        opt.currentValue = (val == "1" || val == "ON" || val == "true");
                        break;
                    }
                }
            }
        }
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
        std::string cmake = config.cmakePath.empty() ? "cmake" : ("\"" + config.cmakePath + "\"");

        // If using a preset, the command is much simpler
        if (!config.cmakePreset.empty())
        {
            std::string srcDir = config.enginePath.empty() ? "." : ("\"" + config.enginePath + "\"");
            return cmake + " --preset " + config.cmakePreset + " -S " + srcDir;
        }

        std::string srcDir = config.enginePath.empty() ? "." : ("\"" + config.enginePath + "\"");
        std::string buildDir = config.buildPath.empty() ? "build" : config.buildPath;

        std::string cmd = cmake + " -S " + srcDir + " -B \"" + buildDir + "\"";
        cmd += " -G \"" + std::string(GeneratorToString(config.generator)) + "\"";

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
            cmd += " -DSPARK_MSVC_TOOLSET=" + config.msvcToolset;
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
            cmd += " -D" + opt.cmakeVar + "=" + (opt.currentValue ? "ON" : "OFF");
        }

        return cmd;
    }

    std::string ConfigManager::BuildCMakeBuildCommand() const
    {
        std::string cmake = config.cmakePath.empty() ? "cmake" : ("\"" + config.cmakePath + "\"");
        std::string buildDir = config.buildPath.empty() ? "build" : config.buildPath;

        std::string cmd = cmake + " --build \"" + buildDir + "\"";
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
