#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "TestFramework.h"
#include "GatewayApplication.h"
#include "GatewayAreaControl.h"

#include <chrono>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <future>
#include <memory>

using namespace Spark::Gateway;

namespace
{
    uint64_t ProcessId()
    {
#ifdef _WIN32
        return static_cast<uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<uint64_t>(::getpid());
#endif
    }

    std::string UniqueName(std::string_view prefix)
    {
        static std::atomic<uint64_t> counter{0};
        return std::string(prefix) + "-" + std::to_string(ProcessId()) + "-" +
               std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    }

    uint16_t UniquePort(uint16_t offset)
    {
        return static_cast<uint16_t>(20000 + ((ProcessId() * 17 + offset) % 30000));
    }
} // namespace

TEST(GatewayAreaControl_LiveLoopbackIsIdempotentAndPersistsEpochFence)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-area-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x3c);
    const uint16_t controlPort = UniquePort(1);

    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.interServerPort = controlPort;
    LocalAreaControlPlane client(key);
    client.RegisterEndpoint(7, area);
    // Match the deterministic endpoint derived by RegisterEndpoint.
    LocalAreaControlService service("spark-area-control-" + std::to_string(controlPort), key, state);
    EXPECT_TRUE(service.Start());
    EXPECT_TRUE(client.IsReady());
    LocalAreaControlPlane wrongKey(std::vector<uint8_t>(32, 0x4d));
    wrongKey.RegisterEndpoint(7, area);
    EXPECT_FALSE(wrongKey.IsReady());

    LocalAreaControlPlane secondClient(key);
    secondClient.RegisterEndpoint(7, area);
    EXPECT_EQ(static_cast<int>(secondClient.Prepare({"second-client", 1, 7, 7})),
              static_cast<int>(HandoffOperationResult::Applied));

    HandoffCommand command{"session-live", 4, 7, 7};
    EXPECT_EQ(static_cast<int>(client.Prepare(command)), static_cast<int>(HandoffOperationResult::Applied));
    EXPECT_EQ(static_cast<int>(client.Prepare(command)), static_cast<int>(HandoffOperationResult::Duplicate));
    EXPECT_EQ(static_cast<int>(client.Transfer(command)), static_cast<int>(HandoffOperationResult::Applied));
    EXPECT_EQ(static_cast<int>(client.Commit(command)), static_cast<int>(HandoffOperationResult::Applied));
    EXPECT_EQ(static_cast<int>(client.Acknowledge(command)), static_cast<int>(HandoffOperationResult::Applied));
    service.Stop();
    EXPECT_FALSE(client.IsReady());

    LocalAreaControlService restarted("spark-area-control-" + std::to_string(controlPort), key, state);
    EXPECT_TRUE(restarted.Start());
    HandoffCommand stale{"session-live", 3, 7, 7};
    EXPECT_TRUE(client.Prepare(stale) == HandoffOperationResult::Rejected);
    HandoffCommand next{"session-live", 5, 7, 7};
    EXPECT_TRUE(client.Prepare(next) == HandoffOperationResult::Applied);
    restarted.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RemoteEndpointIsExplicitlyUnsupported)
{
    LocalAreaControlPlane control(std::vector<uint8_t>(32, 0x65));
    AreaEndpoint area;
    area.host = "203.0.113.10";
    area.area.interServerPort = UniquePort(31);
    control.RegisterEndpoint(31, area);
    EXPECT_FALSE(control.IsReady());
    EXPECT_FALSE(control.IsEndpointReady(31));
}

TEST(GatewayIngress_LiveLoopbackAuthenticatesAndRoutes)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-ingress-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x71);

    const uint16_t controlPort = UniquePort(2);
    const uint16_t gamePort = UniquePort(3);
    const std::string ingressEndpoint = UniqueName("spark-gateway-ingress");
    LocalAreaControlService areaService("spark-area-control-" + std::to_string(controlPort), key, state);
    EXPECT_TRUE(areaService.Start());
    LocalAreaControlPlane control(key);
    KeyFileAuthenticator authenticator(key);
    Spark::Net::WorldServer world;
    Spark::Net::WorldServerConfig worldConfig;
    EXPECT_TRUE(world.Start(worldConfig));
    GatewayCoordinator coordinator(world, authenticator, control);
    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.areaName = "Loopback";
    area.area.port = gamePort;
    area.area.interServerPort = controlPort;
    area.area.maxClients = 8;
    EXPECT_TRUE(coordinator.RegisterAreas({area}));

    LocalGatewayIngressService ingress(ingressEndpoint, coordinator);
    EXPECT_TRUE(ingress.Start());
    AdmissionRequest request;
    request.clientId = 88;
    request.sessionId = "live-ingress-session";
    request.playerName = "Loopback Player";
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    request.credential = authenticator.CreateCredential(request, now, 9911);
    const RouteResult routed = LocalGatewayIngressClient(ingressEndpoint).Admit(request);
    EXPECT_TRUE(routed.accepted);
    EXPECT_EQ(routed.port, gamePort);

    ingress.Stop();
    areaService.Stop();
    world.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_LiveTwoAreaHandoffReachesSourceAndTarget)
{
    const auto temp = std::filesystem::temp_directory_path();
    const auto sourceState = temp / (UniqueName("spark-gateway-source-state") + ".txt");
    const auto targetState = temp / (UniqueName("spark-gateway-target-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(sourceState, error);
    std::filesystem::remove(targetState, error);
    const std::vector<uint8_t> key(32, 0x19);
    const uint16_t sourcePort = UniquePort(4);
    const uint16_t targetPort = UniquePort(5);
    LocalAreaControlService source("spark-area-control-" + std::to_string(sourcePort), key, sourceState);
    LocalAreaControlService target("spark-area-control-" + std::to_string(targetPort), key, targetState);
    EXPECT_TRUE(source.Start());
    EXPECT_TRUE(target.Start());

    LocalAreaControlPlane control(key);
    AreaEndpoint sourceEndpoint;
    sourceEndpoint.host = "127.0.0.1";
    sourceEndpoint.area.interServerPort = sourcePort;
    AreaEndpoint targetEndpoint;
    targetEndpoint.host = "127.0.0.1";
    targetEndpoint.area.interServerPort = targetPort;
    control.RegisterEndpoint(1, sourceEndpoint);
    control.RegisterEndpoint(2, targetEndpoint);
    HandoffCommand command{"two-area-session", 1, 1, 2};
    EXPECT_TRUE(control.Prepare(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Transfer(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Commit(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Acknowledge(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Acknowledge(command) == HandoffOperationResult::Duplicate);

    source.Stop();
    target.Stop();
    std::filesystem::remove(sourceState, error);
    std::filesystem::remove(targetState, error);
}

TEST(GatewayAreaControl_StopCancelsPartialClientFrame)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-partial-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x2a);
    const std::string endpoint = UniqueName("spark-area-control-partial-frame");
    LocalAreaControlService service(endpoint, key, state);
    EXPECT_TRUE(service.Start());

#ifdef _WIN32
    const std::wstring pipe = L"\\\\.\\pipe\\" + std::wstring(endpoint.begin(), endpoint.end());
    HANDLE client = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT_TRUE(client != INVALID_HANDLE_VALUE);
    if (client != INVALID_HANDLE_VALUE)
    {
        const uint8_t partial = 0x01;
        DWORD written = 0;
        EXPECT_TRUE(WriteFile(client, &partial, 1, &written, nullptr) != 0);
    }
#else
    const std::string socketPath = "/tmp/" + endpoint + ".sock";
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_TRUE(client >= 0);
    if (client >= 0)
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socketPath.data(), socketPath.size());
        EXPECT_TRUE(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        const uint8_t partial = 0x01;
        EXPECT_TRUE(::send(client, &partial, 1, 0) == 1);
    }
#endif

    auto stopping = std::async(std::launch::async, [&service] { service.Stop(); });
    const bool stoppedPromptly = stopping.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
#ifdef _WIN32
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
#else
    if (client >= 0)
        ::close(client);
#endif
    stopping.wait();
    EXPECT_TRUE(stoppedPromptly);
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_SecondServiceCannotStealLiveEndpoint)
{
    const std::string endpoint = UniqueName("spark-area-control-exclusive");
    const auto firstState = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-exclusive-a") + ".txt");
    const auto secondState =
        std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-exclusive-b") + ".txt");
    const std::vector<uint8_t> key(32, 0x7d);
    LocalAreaControlService first(endpoint, key, firstState);
    LocalAreaControlService second(endpoint, key, secondState);
    EXPECT_TRUE(first.Start());
    EXPECT_FALSE(second.Start());
    EXPECT_TRUE(first.IsReady());
    first.Stop();
    std::error_code error;
    std::filesystem::remove(firstState, error);
    std::filesystem::remove(secondState, error);
}

TEST(GatewayApplication_IngressStartupFailureUnwindsAWorkingCoordinator)
{
    // Start() only reaches the ingress step after the WorldServer, the area
    // registrations and the authenticated area-control readiness probe already
    // succeeded, so this pins the one startup-failure path that has live state
    // to unwind. The ingress borrows a GatewayCoordinator reference and must not
    // survive it, which is only observable if the teardown order is preserved.
    const auto areaState = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-app-area") + ".txt");
    const auto squatterState =
        std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-app-squat") + ".txt");
    std::error_code error;
    std::filesystem::remove(areaState, error);
    std::filesystem::remove(squatterState, error);
    const std::vector<uint8_t> key(32, 0x3c);

    const uint16_t controlPort = UniquePort(41);
    const std::string ingressEndpoint = UniqueName("spark-gateway-app-ingress");

    LocalAreaControlService areaService("spark-area-control-" + std::to_string(controlPort), key, areaState);
    EXPECT_TRUE(areaService.Start());
    // Own the ingress endpoint first so the application's own listener cannot
    // be created on either a named pipe or a unix socket.
    LocalAreaControlService ingressSquatter(ingressEndpoint, key, squatterState);
    EXPECT_TRUE(ingressSquatter.Start());

    GatewayOptions options;
    options.world.worldName = "GatewayApplicationIngressFailure";
    options.world.port = UniquePort(42);
    options.world.interServerPort = UniquePort(43);
    options.ingressEndpoint = ingressEndpoint;
    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.areaName = "Loopback";
    area.area.port = UniquePort(44);
    area.area.interServerPort = controlPort;
    area.area.maxClients = 8;
    options.areas = {area};

    {
        GatewayApplication application(std::move(options), std::make_unique<KeyFileAuthenticator>(key),
                                       std::make_unique<LocalAreaControlPlane>(key));
        EXPECT_FALSE(application.Start());
        const GatewayHealth health = application.GetHealth();
        EXPECT_FALSE(health.live);
        EXPECT_FALSE(health.ready);
        EXPECT_FALSE(health.ingressReady);
        EXPECT_TRUE(health.lastError.find("ingress") != std::string::npos);
    }

    // The failed startup released the endpoint it could not claim, so the
    // owning service is still the only listener and is still serving.
    EXPECT_TRUE(ingressSquatter.IsReady());
    ingressSquatter.Stop();
    areaService.Stop();
    std::filesystem::remove(areaState, error);
    std::filesystem::remove(squatterState, error);
}
