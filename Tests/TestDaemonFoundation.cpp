/**
 * @file TestDaemonFoundation.cpp
 * @brief End-to-end loopback test for the Phase 1 daemon foundation.
 *
 * Spins up a DaemonServer on an ephemeral socket path in a background thread,
 * connects with DaemonClient, exercises the built-in Control service (ping,
 * version, shutdown), and joins the server thread on clean exit.
 *
 * Runs against AF_UNIX on POSIX and local named pipes on Windows.
 */

#include "TestFramework.h"

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)

#include "Utils/DaemonClient.h"
#include "Utils/DaemonFraming.h"
#include "Utils/DaemonProtocol.h"

// SparkDaemon sources are pulled into SparkTests directly.
#include "ControlService.h"
#include "DaemonServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
    std::string UniqueSocketPath(const char* tag)
    {
#if defined(_WIN32)
        return std::string("spark-daemon-test-") + tag + "-" + std::to_string(::GetCurrentProcessId());
#else
        // Keep it short enough to fit in sockaddr_un::sun_path (108 bytes on Linux).
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-daemon-test-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
#endif
    }

    bool WaitForSocketFile(const std::string& path, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
#if defined(_WIN32)
            const std::wstring pipeName = Spark::Daemon::NormalizePipeName(path);
            if (!pipeName.empty() && ::WaitNamedPipeW(pipeName.c_str(), 20))
                return true;
#else
            struct stat st;
            if (::stat(path.c_str(), &st) == 0)
                return true;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
} // namespace

TEST(DaemonFoundation_ConnectPingShutdown)
{
    const std::string sockPath = UniqueSocketPath("ping");

    auto server = std::make_unique<Spark::Daemon::DaemonServer>();
    server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));

    std::atomic<bool> serverStarted{false};
    std::atomic<bool> serverExited{false};
    std::thread serverThread(
        [&]
        {
            serverStarted.store(true, std::memory_order_release);
            auto result = server->Run(sockPath);
            serverExited.store(true, std::memory_order_release);
            EXPECT_TRUE(result.has_value());
        });

    // Wait for the socket file to appear so connect() doesn't race the bind.
    EXPECT_TRUE(WaitForSocketFile(sockPath, std::chrono::milliseconds(2000)));

    Spark::Daemon::DaemonClient client;
    auto connectResult = client.Connect(sockPath);
    EXPECT_TRUE(connectResult.has_value());
    EXPECT_TRUE(client.IsConnected());

    // Ping round-trip.
    auto pingResult = client.Ping();
    EXPECT_TRUE(pingResult.has_value());

    // Version request via the lower-level Request API.
    auto versionResult =
        client.Request(Spark::Daemon::ServiceId::Control,
                       static_cast<uint16_t>(Spark::Daemon::ControlMessage::VersionRequest), /*payload*/ {});
    EXPECT_TRUE(versionResult.has_value());
    EXPECT_EQ(versionResult->messageType, static_cast<uint16_t>(Spark::Daemon::ControlMessage::VersionResponse));
    std::string version(versionResult->payload.begin(), versionResult->payload.end());
    EXPECT_EQ(version, std::string(Spark::Daemon::kProtocolVersion));

    // Shutdown flips the server's should-stop flag; ack comes back, then
    // server's accept loop exits next cycle.
    auto shutdownResult =
        client.Request(Spark::Daemon::ServiceId::Control,
                       static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownRequest), /*payload*/ {});
    EXPECT_TRUE(shutdownResult.has_value());
    EXPECT_EQ(shutdownResult->messageType, static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownAck));

    client.Disconnect();

    // Nudge the accept loop out of its blocking wait.
    server->Stop();

    if (serverThread.joinable())
        serverThread.join();
    EXPECT_TRUE(serverExited.load(std::memory_order_acquire));
    EXPECT_FALSE(client.IsConnected());

#if !defined(_WIN32)
    ::unlink(sockPath.c_str());
#endif
}

TEST(DaemonFoundation_UnknownServiceReturnsError)
{
    const std::string sockPath = UniqueSocketPath("unk");

    auto server = std::make_unique<Spark::Daemon::DaemonServer>();
    server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));

    std::thread serverThread([&] { (void)server->Run(sockPath); });

    EXPECT_TRUE(WaitForSocketFile(sockPath, std::chrono::milliseconds(2000)));

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(sockPath).has_value());

    auto result = client.Request(Spark::Daemon::ServiceId::Asset, 0x0001u, /*payload*/ {});
    // Server replies with a Control::ErrorResponse — DaemonClient surfaces
    // that as an unexpected() with "server error:" prefix.
    EXPECT_FALSE(result.has_value());

    client.Disconnect();
    server->Stop();
    if (serverThread.joinable())
        serverThread.join();

#if !defined(_WIN32)
    ::unlink(sockPath.c_str());
#endif
}

TEST(DaemonFoundation_SecondServerCannotStealActiveEndpoint)
{
    const std::string sockPath = UniqueSocketPath("exclusive");
    auto first = std::make_unique<Spark::Daemon::DaemonServer>();
    first->AddService(std::make_unique<Spark::Daemon::ControlService>(first->GetShouldStopFlag()));
    std::thread firstThread([&] { EXPECT_TRUE(first->Run(sockPath).has_value()); });
    EXPECT_TRUE(WaitForSocketFile(sockPath, std::chrono::milliseconds(2000)));

    Spark::Daemon::DaemonServer second;
    auto secondResult = second.Run(sockPath);
    EXPECT_FALSE(secondResult.has_value());

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(sockPath).has_value());
    EXPECT_TRUE(client.Ping().has_value());
    client.Disconnect();

    first->Stop();
    if (firstThread.joinable())
        firstThread.join();
#if !defined(_WIN32)
    ::unlink(sockPath.c_str());
#endif
}

TEST(DaemonFoundation_ClientReportsNotConnected)
{
    Spark::Daemon::DaemonClient client;
    EXPECT_FALSE(client.IsConnected());

    auto result = client.Ping();
    EXPECT_FALSE(result.has_value());
}

TEST(DaemonFoundation_ConnectToMissingSocketFails)
{
    Spark::Daemon::DaemonClient client;
    auto result = client.Connect(UniqueSocketPath("does-not-exist"));
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(client.IsConnected());
}

#endif // supported local IPC platforms
