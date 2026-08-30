/**
 * @file TestSparkBuildConfig.cpp
 * @brief Regression tests for SparkBuild CMake command generation.
 */

#include "TestFramework.h"
#include "Config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace SparkBuild;

namespace
{
    std::filesystem::path WriteConfigFixture(std::string_view label, std::string_view contents)
    {
        const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() /
                          ("sparkbuild-config-" + std::string(label) + "-" + std::to_string(nonce) + ".ini");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        output.close();
        return path;
    }

    bool OptionValue(const ConfigManager& manager, std::string_view name)
    {
        for (const auto& option : manager.config.options)
        {
            if (option.cmakeVar == name)
                return option.currentValue;
        }
        throw std::runtime_error("missing SparkBuild option fixture");
    }
} // namespace

TEST(SparkBuildConfig_VisualStudioToolsetIsGeneratorInput)
{
    ConfigManager manager;
    manager.config.enginePath = "engine";
    manager.config.buildPath = "out";
    manager.config.generator = Generator::VS2022;
    manager.config.msvcToolset = "v143";

    const std::string command = manager.BuildCMakeConfigureCommand();
#ifdef SPARK_PLATFORM_WINDOWS
    EXPECT_TRUE(command.find(" -T \"v143\"") != std::string::npos);
    EXPECT_TRUE(command.find(" -A x64") != std::string::npos);
    EXPECT_TRUE(command.find("SPARK_MSVC_TOOLSET") == std::string::npos);
#else
    EXPECT_TRUE(command.find(" -T v143") == std::string::npos);
#endif
}

TEST(SparkBuildConfig_AsyncCommandQuotesMetacharactersAsOneArgument)
{
    ConfigManager manager;
    manager.config.cmakePath = "tools & sdk/cmake";
    manager.config.enginePath = "engine & source";
    manager.config.cmakePreset = "safe & preset";

    const std::string command = manager.BuildCMakeConfigureCommand();
    EXPECT_TRUE(command.find("\"tools & sdk/cmake\" --preset \"safe & preset\" -S \"engine & source\"") !=
                std::string::npos);
}

TEST(SparkBuildConfig_AsyncCommandRejectsQuoteBreakingInput)
{
    ConfigManager manager;
    manager.config.cmakePreset = "safe\" & injected";

    bool rejected = false;
    try
    {
        (void)manager.BuildCMakeConfigureCommand();
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(SparkBuildConfig_AsyncCommandRejectsPlatformExpansionInput)
{
    ConfigManager manager;
#ifdef SPARK_PLATFORM_WINDOWS
    manager.config.buildPath = "%COMSPEC%";
#else
    manager.config.buildPath = "$(touch injected)";
#endif

    bool rejected = false;
    try
    {
        (void)manager.BuildCMakeBuildCommand();
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(SparkBuildConfig_LoadRejectsUnknownOptionTransactionally)
{
    ConfigManager manager;
    const auto path =
        WriteConfigFixture("unknown-option", "[Options]\nENABLE_GRAPHICS=OFF\nREMOVED_OR_UNKNOWN_OPTION=ON\n");

    EXPECT_FALSE(manager.Load(path.string()));
    EXPECT_TRUE(OptionValue(manager, "ENABLE_GRAPHICS"));
    std::filesystem::remove(path);
}

TEST(SparkBuildConfig_LoadRejectsInvalidOptionValue)
{
    ConfigManager manager;
    const auto path = WriteConfigFixture("invalid-value", "[Options]\nENABLE_GRAPHICS=perhaps\n");

    EXPECT_FALSE(manager.Load(path.string()));
    EXPECT_TRUE(OptionValue(manager, "ENABLE_GRAPHICS"));
    std::filesystem::remove(path);
}

TEST(SparkBuildConfig_LoadAcceptsKnownOptionValue)
{
    ConfigManager manager;
    const auto path = WriteConfigFixture("known-option", "[Options]\nENABLE_GRAPHICS=OFF\n");

    EXPECT_TRUE(manager.Load(path.string()));
    EXPECT_FALSE(OptionValue(manager, "ENABLE_GRAPHICS"));
    std::filesystem::remove(path);
}
