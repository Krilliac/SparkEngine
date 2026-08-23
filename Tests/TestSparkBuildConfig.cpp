/**
 * @file TestSparkBuildConfig.cpp
 * @brief Regression tests for SparkBuild CMake command generation.
 */

#include "TestFramework.h"
#include "Config.h"

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
    EXPECT_TRUE(command.find(" -T v143") != std::string::npos);
    EXPECT_TRUE(command.find(" -A x64") != std::string::npos);
    EXPECT_TRUE(command.find("SPARK_MSVC_TOOLSET") == std::string::npos);
#else
    EXPECT_TRUE(command.find(" -T v143") == std::string::npos);
#endif
}
