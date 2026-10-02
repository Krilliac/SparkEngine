#pragma once

#include "Platform.h"
#include <cstddef>
#include <istream>
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
        std::string cmakeVar;    // e.g. "ENABLE_RECAST"
        std::string displayName; // e.g. "Recast Navigation"
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
        std::string msvcToolset; // e.g. "v143", "v145" (Windows only)
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

        // Largest sparkbuild.ini Load accepts. A saved file is a few KiB; the bound
        // keeps a wrong path (a log, a device, a pipe) from being read into memory.
        static constexpr size_t kMaxConfigBytes = 64 * 1024;

        // Load/save user preferences to INI file. Load refuses a file larger than
        // kMaxConfigBytes, then parses it with LoadFromStream.
        bool Load(const std::string& iniPath);
        bool Save(const std::string& iniPath) const;

        // Parse sparkbuild.ini text. Transactional: an unknown section, key or value,
        // or a repeated key, returns false and leaves config unchanged.
        bool LoadFromStream(std::istream& input);

        // Apply a preset to all options
        void ApplyPresetAllOn();
        void ApplyPresetAllOff();
        void ApplyPresetDefaults();
        void ApplyPresetMinimal();
        void ApplyPresetLinuxFriendly();
        void ApplyPresetShipping();
        void ApplyPresetDevelopment();

        // Build the cmake configure command line. Throws std::invalid_argument
        // when a configured value cannot be represented safely in the shell
        // command consumed by ProcessRunner::RunAsync.
        std::string BuildCMakeConfigureCommand() const;
        // Build the cmake build command line (same validation contract).
        std::string BuildCMakeBuildCommand() const;

        // Get path to the INI file (next to the exe or in home dir)
        static std::string GetDefaultIniPath();

        // Detect available CMake presets from engine directory
        std::vector<std::string> DetectCMakePresets() const;

        BuildConfig config;
    };

} // namespace SparkBuild
