/** @file TestServiceTopologyController.cpp @brief Deterministic topology command contract tests. */
#include "TestFramework.h"
#include "Panels/ServiceTopologyController.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
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
    spec.workingDirectory = root;
    spec.healthFile = health;
    spec.stopFile = stop;
    spec.privateKeyFile = root / "Config/gateway.key";
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

TEST(ServiceTopologyController_LaunchesProcessInsideConfiguredWorkingDirectory)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("spark-topology-working-dir-" + std::to_string(stamp));
#ifdef _WIN32
    root /= L"Caf\u00e9 Project";
#endif
    fs::create_directories(root);

    TopologyServiceSpec spec;
#ifdef _WIN32
    spec.executable = "cmd.exe";
    std::ofstream(root / "cwd-marker.txt") << "ready\n";
    spec.arguments = {"/d", "/c", "if exist cwd-marker.txt (echo cwd-ok) else (exit /b 9)"};
#else
    spec.executable = "/bin/pwd";
#endif
    spec.workingDirectory = root;

    ServiceTopologyController controller;
    controller.Configure(TopologyService::Orchestrator, std::move(spec));
    ASSERT_TRUE(controller.Start(TopologyService::Orchestrator));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (controller.Snapshot(TopologyService::Orchestrator).running && std::chrono::steady_clock::now() < deadline)
    {
        controller.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    controller.Update();

    const auto& snapshot = controller.Snapshot(TopologyService::Orchestrator);
    EXPECT_FALSE(snapshot.running);
    EXPECT_TRUE(snapshot.exitCode.has_value());
    if (snapshot.exitCode)
        EXPECT_EQ(*snapshot.exitCode, 0);

    bool reportedConfiguredDirectory = false;
    for (const std::string& line : snapshot.log)
    {
#ifdef _WIN32
        if (line == "cwd-ok")
        {
            reportedConfiguredDirectory = true;
            break;
        }
#else
        std::error_code equivalentError;
        if (fs::equivalent(fs::path(line), root, equivalentError) && !equivalentError)
        {
            reportedConfiguredDirectory = true;
            break;
        }
#endif
    }
    EXPECT_TRUE(reportedConfiguredDirectory);

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

TEST(ServiceTopologyController_ProvisionsProjectPrivateKeyOnce)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-topology-key-" + std::to_string(stamp));
    fs::create_directories(root);
    const fs::path keyFile = root / "Config/gateway.key";
    std::string error;

    EXPECT_TRUE(ServiceTopologyController::EnsureProjectPrivateKey(root, keyFile, error));
    EXPECT_TRUE(fs::is_regular_file(keyFile));
    std::ifstream first(keyFile, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(first)), std::istreambuf_iterator<char>());
    EXPECT_EQ(original.size(), size_t{65});

    EXPECT_TRUE(ServiceTopologyController::EnsureProjectPrivateKey(root, keyFile, error));
    std::ifstream second(keyFile, std::ios::binary);
    const std::string unchanged((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>());
    EXPECT_EQ(unchanged, original);
    first.close();
    second.close();

    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ServiceTopologyController_RejectsPrivateKeyOutsideProject)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-topology-key-root-" + std::to_string(stamp));
    const fs::path outside = root.parent_path() / ("spark-topology-key-outside-" + std::to_string(stamp));
    fs::create_directories(root);
    std::string error;

    EXPECT_FALSE(ServiceTopologyController::EnsureProjectPrivateKey(root, outside / "gateway.key", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(outside / "gateway.key"));

    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    fs::remove_all(outside, cleanupError);
}
