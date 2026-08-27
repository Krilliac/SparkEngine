/** @file TestLauncherPaths.cpp @brief SparkLauncher path discovery contract tests. */
#include "TestFramework.h"
#include "../SparkLauncher/src/LauncherPaths.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace
{
    std::filesystem::path MakeLauncherPathsTestRoot()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path() / ("spark-launcher-paths-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        return root;
    }
} // namespace

TEST(LauncherPaths_FindsInstalledTemplatesRelativeToExecutable)
{
    const auto root = MakeLauncherPathsTestRoot();
    const auto executable = root / "install" / "bin" / "SparkLauncher";
    const auto installedTemplates = root / "install" / "share" / "SparkEngine" / "templates";
    const auto unrelatedWorkingDirectory = root / "working" / "project";
    std::filesystem::create_directories(installedTemplates);
    std::filesystem::create_directories(unrelatedWorkingDirectory);

    const auto found = SparkLauncher::FindLauncherTemplatesDirectory(executable, unrelatedWorkingDirectory);
    EXPECT_EQ(found, std::filesystem::canonical(installedTemplates));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherPaths_PrefersInstalledTemplatesOverWorkingDirectory)
{
    const auto root = MakeLauncherPathsTestRoot();
    const auto executable = root / "install" / "bin" / "SparkLauncher";
    const auto developmentTemplates = root / "source" / "Templates";
    const auto installedTemplates = root / "install" / "share" / "SparkEngine" / "templates";
    std::filesystem::create_directories(developmentTemplates);
    std::filesystem::create_directories(installedTemplates);

    const auto found = SparkLauncher::FindLauncherTemplatesDirectory(executable, root / "source");
    EXPECT_EQ(found, std::filesystem::canonical(installedTemplates));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherPaths_FallsBackToDevelopmentTemplates)
{
    const auto root = MakeLauncherPathsTestRoot();
    const auto executable = root / "build" / "bin" / "SparkLauncher";
    const auto developmentTemplates = root / "source" / "Templates";
    std::filesystem::create_directories(developmentTemplates);

    const auto found = SparkLauncher::FindLauncherTemplatesDirectory(executable, root / "source");
    EXPECT_EQ(found, std::filesystem::canonical(developmentTemplates));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherPaths_ResolvesRunningExecutable)
{
    const auto executable = SparkLauncher::GetLauncherExecutablePath();
    EXPECT_FALSE(executable.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(executable));
}
