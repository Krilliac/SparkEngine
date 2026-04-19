#pragma once

#include "Platform.h"
#include <string>
#include <vector>
#include <map>

namespace SparkBuild
{

    // Categories for organizing build options in the UI
    enum class OptionCategory
    {
        Core,
        Graphics,
        Rendering,
        EditorTools,
        Scripting,
        Gameplay,
        Shipping,
        Experimental
    };

    struct BuildOption
    {
        std::string cmakeVar;    // e.g. "ENABLE_GRAPHICS"
        std::string displayName; // e.g. "Graphics Engine"
        std::string description; // Description text
        bool defaultValue;
        bool currentValue;
        OptionCategory category;
    };

    enum class BuildType
    {
        Debug,
        Release,
        RelWithDebInfo,
        MinSizeRel
    };

    enum class Generator
    {
        VS2022,
        VS2026,
        Ninja,
        NinjaMultiConfig,
        UnixMakefiles,
        Xcode
    };

    struct BuildConfig
    {
        // Paths
        std::string enginePath; // Path to SparkEngine repo root
        std::string buildPath;  // Path to build output directory
        std::string cmakePath;  // Path to cmake executable (empty = use PATH)

        // Build settings
        Generator generator = Generator::Ninja;
        BuildType buildType = BuildType::Release;
        std::string msvcToolset; // e.g. "v143", "v144" (Windows only)
        std::string cmakePreset; // CMake preset name (empty = manual config)

        // All build options
        std::vector<BuildOption> options;

        // Parallel jobs
        int parallelJobs = 0; // 0 = auto-detect
    };

    // Returns the string representation for CMake -G flag
    const char* GeneratorToString(Generator gen);
    const char* GeneratorDisplayName(Generator gen);
    const char* BuildTypeToString(BuildType bt);
    const char* CategoryDisplayName(OptionCategory cat);

    // Get available generators for the current platform
    std::vector<Generator> GetAvailableGenerators();

    // Get the default generator for the current platform
    Generator GetDefaultGenerator();

    class ConfigManager
    {
      public:
        ConfigManager();

        // Initialize default build options based on SparkEngine's CMakeLists.txt
        void InitDefaults();

        // Load/save user preferences to INI file
        bool Load(const std::string& iniPath);
        bool Save(const std::string& iniPath) const;

        // Apply a preset to all options
        void ApplyPresetAllOn();
        void ApplyPresetAllOff();
        void ApplyPresetDefaults();
        void ApplyPresetMinimal();
        void ApplyPresetLinuxFriendly();
        void ApplyPresetShipping();
        void ApplyPresetDevelopment();

        // Build the cmake configure command line
        std::string BuildCMakeConfigureCommand() const;
        // Build the cmake build command line
        std::string BuildCMakeBuildCommand() const;

        // Get path to the INI file (next to the exe or in home dir)
        static std::string GetDefaultIniPath();

        // Detect available CMake presets from engine directory
        std::vector<std::string> DetectCMakePresets() const;

        BuildConfig config;
    };

} // namespace SparkBuild
