/**
 * @file TestSparkBuildConfig.cpp
 * @brief Regression tests for SparkBuild CMake command generation.
 */

#include "TestFramework.h"
#include "Config.h"

#include <stdexcept>
#include <string>

using namespace SparkBuild;

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
