/**
 * @file TestDedicatedServerProcessController.cpp
 * @brief Focused editor-to-SparkServer argument and packaging tests.
 */

#include "TestFramework.h"
#include "Panels/DedicatedServerProcessController.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace SparkEditor;

TEST(DedicatedServerProcessController_BuildsManifestLaunchArguments)
{
    DedicatedServerLaunchRequest request;
    request.executable = "SparkServer.exe";
    request.manifest = "Project/spark.modules.json";
    request.healthFile = "Project/Temp/health.json";
    request.stopFile = "Project/Temp/stop";
    request.serverName = "Local test";
    request.map = "arena";
    request.port = 28015;
    request.maxClients = 24;
    request.tickRate = 30.0f;
    request.lanOnly = true;
    request.lanBroadcast = false;

    std::string error;
    const auto arguments = DedicatedServerProcessController::CreateArguments(request, error);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--manifest") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--health-file") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--stop-file") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--lan-only") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--no-lan-broadcast") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "28015") != arguments.end());
}

TEST(DedicatedServerProcessController_RejectsAmbiguousGameSelection)
{
    DedicatedServerLaunchRequest request;
    request.executable = "SparkServer.exe";
    request.module = "Game.dll";
    request.manifest = "spark.modules.json";
    request.stopFile = "stop";
    std::string error;
    EXPECT_TRUE(DedicatedServerProcessController::CreateArguments(request, error).empty());
    EXPECT_TRUE(error.find("exactly one") != std::string::npos);
}

TEST(DedicatedServerProcessController_PackagesAndRenamesBuiltServer)
{
    const auto root = std::filesystem::temp_directory_path() / "spark-dedicated-package-test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::filesystem::create_directories(root / "build");
    {
        std::ofstream source(root / "build" / "SparkServer.exe", std::ios::binary);
        source << "server-binary";
    }
    std::string error;
    EXPECT_TRUE(DedicatedServerProcessController::PackageExecutable(root / "build" / "SparkServer.exe",
                                                                    root / "package", "ArenaServer", error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "package" / "ArenaServer.exe"));
    std::filesystem::remove_all(root, cleanupError);
}
