/** @file TestLauncherProcess.cpp @brief SparkLauncher project-action command contract tests. */
#include "TestFramework.h"
#include "../SparkLauncher/src/LauncherProcess.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
    std::filesystem::path MakeLauncherTestRoot()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path() / ("spark-launcher-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "bin");
        std::filesystem::create_directories(root / "project" / "Config");
        return root;
    }

    std::filesystem::path Executable(const std::filesystem::path& directory, const char* name)
    {
#ifdef _WIN32
        return directory / (std::string(name) + ".exe");
#else
        return directory / name;
#endif
    }

    void Touch(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "fixture";
    }
} // namespace

TEST(LauncherProcess_BuildsEditorGameAndServiceTopologyRequests)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto project = root / "project" / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkEditor"));
    Touch(Executable(binaries, "SparkEngine"));

    auto editor = BuildLaunchRequest(binaries, project, LaunchTarget::Editor);
    EXPECT_TRUE(editor.has_value());
    EXPECT_EQ(editor->workingDirectory, project.parent_path());
    EXPECT_EQ(editor->arguments.size(), static_cast<size_t>(2));
    EXPECT_EQ(editor->arguments[0], std::string("--project"));

    auto game = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_TRUE(game.has_value());
    EXPECT_EQ(game->executable, Executable(binaries, "SparkEngine"));

    auto services = BuildLaunchRequest(binaries, project, LaunchTarget::ServiceTopology);
    EXPECT_TRUE(services.has_value());
    EXPECT_EQ(services->arguments.size(), static_cast<size_t>(4));
    EXPECT_EQ(services->arguments[2], std::string("--open-panel"));
    EXPECT_EQ(services->arguments[3], std::string("ServiceTopology"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherProcess_RequiresServerConfigAndExecutable)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto project = root / "project" / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkServer"));

    auto missingConfig = BuildLaunchRequest(binaries, project, LaunchTarget::DedicatedServer);
    EXPECT_FALSE(missingConfig.has_value());
    EXPECT_TRUE(missingConfig.error().find("server.ini") != std::string::npos);

    const auto config = root / "project" / "Config" / "server.ini";
    Touch(config);
    auto server = BuildLaunchRequest(binaries, project, LaunchTarget::DedicatedServer);
    EXPECT_TRUE(server.has_value());
    EXPECT_EQ(server->arguments.size(), static_cast<size_t>(2));
    EXPECT_EQ(server->arguments[0], std::string("--config"));
    EXPECT_EQ(server->arguments[1], config.string());

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}
