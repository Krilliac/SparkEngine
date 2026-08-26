/** @file TestServiceTopologyController.cpp @brief Deterministic topology command contract tests. */
#include "TestFramework.h"
#include "Panels/ServiceTopologyController.h"

#include <algorithm>

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
