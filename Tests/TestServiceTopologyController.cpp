/** @file TestServiceTopologyController.cpp @brief Deterministic topology command contract tests. */
#include "TestFramework.h"
#include "Panels/ServiceTopologyController.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

using namespace SparkEditor;

TEST(ServiceTopologyController_ConstructsDaemonSecurityBoundary)
{
    const auto arguments = ServiceTopologyController::DaemonArguments("spark-daemon-test", "D:/Projects/Game",
                                                                      "D:/Projects/Game/Temp/orchestrator.state");
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--socket") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--orchestrator-allow-root") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "D:/Projects/Game") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--orchestrator-state-file") != arguments.end());
}

TEST(ServiceTopologyController_ConstructsGatewayHealthAndStopSurfaces)
{
    const auto arguments = ServiceTopologyController::GatewayArguments("Config/gateway.ini", "Temp/gateway-health.json",
                                                                       "Temp/gateway.stop");
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--config") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--health-file") != arguments.end());
    EXPECT_TRUE(std::find(arguments.begin(), arguments.end(), "--stop-file") != arguments.end());
}

TEST(ServiceTopologyController_ConstructsOrchestratorStatusCommand)
{
    const auto arguments = ServiceTopologyController::OrchestratorStatusArguments("spark-daemon-test");
    EXPECT_EQ(arguments.size(), static_cast<size_t>(3));
    EXPECT_EQ(arguments[2], std::string("list"));
}

TEST(ServiceTopologyController_ConstructsOrchestratorServerLifecycleCommands)
{
    const auto define = ServiceTopologyController::OrchestratorDefineArguments(
        "spark-daemon-test", "area-town", "D:/Spark/bin/SparkServer.exe", "D:/Projects/Game",
        {"--config", "D:/Projects/Game/Config/server.ini"});
    EXPECT_EQ(define.size(), static_cast<size_t>(8));
    EXPECT_EQ(define[2], std::string("define"));
    EXPECT_EQ(define[3], std::string("area-town"));
    EXPECT_EQ(define[4], std::string("D:/Spark/bin/SparkServer.exe"));
    EXPECT_EQ(define[5], std::string("D:/Projects/Game"));
    EXPECT_EQ(define[6], std::string("--config"));

    for (const std::string command : {"start", "drain", "restart", "stop", "undefine"})
    {
        const auto mutation =
            ServiceTopologyController::OrchestratorMutationArguments("spark-daemon-test", command, "area-town");
        EXPECT_EQ(mutation.size(), static_cast<size_t>(4));
        EXPECT_EQ(mutation[2], command);
        EXPECT_EQ(mutation[3], std::string("area-town"));
    }
}

TEST(ServiceTopologyController_BoundsRetainedProcessOutput)
{
    TopologyServiceSnapshot snapshot;
    for (size_t index = 0; index < ServiceTopologyController::MaxRetainedLogLines + 3; ++index)
        ServiceTopologyController::AppendLogLine(snapshot, "line-" + std::to_string(index));

    EXPECT_EQ(snapshot.log.size(), ServiceTopologyController::MaxRetainedLogLines);
    EXPECT_EQ(snapshot.log.front(), std::string("line-3"));

    ServiceTopologyController::AppendLogLine(
        snapshot, std::string(ServiceTopologyController::MaxRetainedLogLineBytes + 100, 'x'));
    EXPECT_EQ(snapshot.log.back().size(), ServiceTopologyController::MaxRetainedLogLineBytes);
    EXPECT_TRUE(snapshot.log.back().ends_with(" [truncated]"));
}

TEST(ServiceTopologyController_RemovesStaleControlFilesBeforeLaunch)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-topology-stale-" + std::to_string(stamp));
    fs::create_directories(root);
    const fs::path health = root / "health.json";
    const fs::path stop = root / "gateway.stop";
    std::ofstream(health) << "{\"ready\":true}";
    std::ofstream(stop) << "stop\n";

    ServiceTopologyController controller;
    TopologyServiceSpec spec;
    spec.executable = root / "missing-service";
    spec.healthFile = health;
    spec.stopFile = stop;
    controller.Configure(TopologyService::Gateway, std::move(spec));

    // CreateProcess reports a missing executable synchronously on Windows, while
    // POSIX fork/exec reports a successful fork and the child exits with 127.
    // This test covers stale-control cleanup, so do not make it depend on which
    // launch-failure model the platform exposes.
    (void)controller.Start(TopologyService::Gateway);
    EXPECT_FALSE(fs::exists(health));
    EXPECT_FALSE(fs::exists(stop));

    std::error_code error;
    fs::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(ServiceTopologyController_CapsHealthFileBeforeReading)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-topology-health-" + std::to_string(stamp));
    fs::create_directories(root);
    const fs::path health = root / "health.json";

    std::ofstream(health, std::ios::binary) << std::string(ServiceTopologyController::MaxHealthFileBytes + 1, 'x');
    std::string contents = "stale";
    EXPECT_FALSE(ServiceTopologyController::ReadHealthFile(health, contents));
    EXPECT_TRUE(contents.empty());

    std::ofstream(health, std::ios::binary | std::ios::trunc) << "{\"ready\":true}";
    EXPECT_TRUE(ServiceTopologyController::ReadHealthFile(health, contents));
    EXPECT_EQ(contents, std::string("{\"ready\":true}"));

    std::error_code error;
    fs::remove_all(root, error);
    EXPECT_FALSE(error);
}
