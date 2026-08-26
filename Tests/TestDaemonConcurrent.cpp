/**
 * @file TestDaemonConcurrent.cpp
 * @brief Concurrent-clients and stats-request tests for SparkDaemon.
 *
 * Phase 1/2/3 tests all use a single client per server. These tests verify
 * the multi-client story: two clients connected to the same daemon issuing
 * interleaved requests produce correctly-ordered responses, and the new
 * `Control::StatsRequest` reports accurate uptime + service inventory.
 */

#include "TestFramework.h"

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)

#include "Utils/DaemonClient.h"
#include "Utils/DaemonFraming.h"
#include "Utils/DaemonProtocol.h"
#include "Utils/ShaderServiceClient.h"

#include "ControlService.h"
#include "DaemonServer.h"
#include "ShaderService.h"

#include <atomic>
#include <barrier>
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
#include <vector>

namespace
{
    std::string UniqueConcurrentSockPath(const char* tag)
    {
#if defined(_WIN32)
        return std::string("spark-daemon-concurrent-") + tag + "-" + std::to_string(::GetCurrentProcessId());
#else
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-daemon-concurrent-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
#endif
    }

    bool WaitForConcurrentSocket(const std::string& path, std::chrono::milliseconds timeout)
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

    struct ConcurrentFixture
    {
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;
        std::string sockPath;

        explicit ConcurrentFixture(
            const char* tag,
            size_t maximumWorkers = Spark::Daemon::DaemonServer::kDefaultMaximumClientWorkers)
            : sockPath(UniqueConcurrentSockPath(tag))
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>(maximumWorkers);
            auto statsProvider = [this] { return server->SnapshotStats(); };
            server->AddService(
                std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag(), statsProvider));
            server->AddService(std::make_unique<Spark::Daemon::ShaderService>());
            thread = std::thread([this] { (void)server->Run(sockPath); });
            EXPECT_TRUE(WaitForConcurrentSocket(sockPath, std::chrono::milliseconds(2000)));
        }

        ~ConcurrentFixture()
        {
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
#if !defined(_WIN32)
            ::unlink(sockPath.c_str());
#endif
        }
    };
} // namespace

// =========================================================================
// Concurrent clients
// =========================================================================

TEST(DaemonConcurrent_TwoClientsShareCacheAndDontInterleaveResponses)
{
    ConcurrentFixture fx("two-clients");

    // Client A stores 50 entries; client B, concurrently, issues 50 pings.
    // Both sequences must complete with correct, non-corrupt responses.
    constexpr int kIterations = 50;

    std::atomic<int> storesOk{0};
    std::atomic<int> pingsOk{0};
    std::barrier connectRace(3);

    std::thread writer(
        [&]
        {
            Spark::Daemon::DaemonClient client;
            connectRace.arrive_and_wait();
            const auto connected = client.Connect(fx.sockPath);
            EXPECT_TRUE(connected.has_value());
            if (!connected)
                return;
            Spark::Daemon::ShaderServiceClient shader(client);
            for (int i = 0; i < kIterations; ++i)
            {
                std::vector<uint8_t> blob{static_cast<uint8_t>(i & 0xFFu), 0xAA, 0xBB};
                auto put = shader.PutCacheEntry(/*hash*/ static_cast<uint64_t>(i), /*target*/ 1, /*stage*/ 0, blob);
                if (put)
                    storesOk.fetch_add(1, std::memory_order_relaxed);
            }
            client.Disconnect();
        });

    std::thread pinger(
        [&]
        {
            Spark::Daemon::DaemonClient client;
            connectRace.arrive_and_wait();
            const auto connected = client.Connect(fx.sockPath);
            EXPECT_TRUE(connected.has_value());
            if (!connected)
                return;
            for (int i = 0; i < kIterations; ++i)
            {
                if (client.Ping())
                    pingsOk.fetch_add(1, std::memory_order_relaxed);
            }
            client.Disconnect();
        });

    // Release both clients onto the server's single listening pipe instance at
    // the same time. On Windows one CreateFileW wins; the other must wait for
    // the accept loop to publish the next instance instead of failing busy.
    connectRace.arrive_and_wait();
    writer.join();
    pinger.join();

    EXPECT_EQ(storesOk.load(), kIterations);
    EXPECT_EQ(pingsOk.load(), kIterations);

    // Verify the writer's entries are readable from a third client — every
    // PutCacheEntry must have reached the cache.
    Spark::Daemon::DaemonClient reader;
    EXPECT_TRUE(reader.Connect(fx.sockPath).has_value());
    Spark::Daemon::ShaderServiceClient shader(reader);

    int hits = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        auto get = shader.GetCacheEntry(static_cast<uint64_t>(i), 1, 0);
        if (get && get->found && !get->blob.empty() && get->blob[0] == static_cast<uint8_t>(i & 0xFFu))
            ++hits;
    }
    EXPECT_EQ(hits, kIterations);
}

TEST(DaemonConcurrent_WorkerCapRefusesOverloadAndShutdownRemainsPrompt)
{
    ConcurrentFixture fx("worker-cap", 1);

    Spark::Daemon::DaemonClient occupyingClient;
    ASSERT_TRUE(occupyingClient.Connect(fx.sockPath).has_value());
    // Let the accept loop assign the sole worker and publish its next endpoint.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Spark::Daemon::DaemonClient overloadedClient;
    const auto connected = overloadedClient.Connect(fx.sockPath);
    if (connected)
    {
        // A transport connection can win the race just before refusal, but no
        // request may be serviced by an extra worker.
        EXPECT_FALSE(overloadedClient.Ping().has_value());
    }

    const auto stopStarted = std::chrono::steady_clock::now();
    fx.server->Stop();
    if (fx.thread.joinable())
        fx.thread.join();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    EXPECT_TRUE(stopElapsed < std::chrono::seconds(2));
}

// =========================================================================
// Control::StatsRequest
// =========================================================================

TEST(DaemonStats_ReportsVersionAndRegisteredServices)
{
    ConcurrentFixture fx("stats");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());

    auto response = client.Request(Spark::Daemon::ServiceId::Control,
                                   static_cast<uint16_t>(Spark::Daemon::ControlMessage::StatsRequest), /*payload*/ {});
    EXPECT_TRUE(response.has_value());
    EXPECT_EQ(response->messageType, static_cast<uint16_t>(Spark::Daemon::ControlMessage::StatsResponse));

    Spark::Daemon::DaemonStats stats;
    EXPECT_TRUE(Spark::Daemon::DecodeDaemonStats(response->payload, stats));
    EXPECT_EQ(stats.protocolVersion, std::string(Spark::Daemon::kProtocolVersion));

    // Control + Shader were both registered.
    EXPECT_EQ(stats.registeredIds.size(), 2u);
    EXPECT_EQ(stats.registeredIds[0], static_cast<uint16_t>(Spark::Daemon::ServiceId::Control));
    EXPECT_EQ(stats.registeredIds[1], static_cast<uint16_t>(Spark::Daemon::ServiceId::Shader));
}

TEST(DaemonStats_UptimeAdvancesAcrossRequests)
{
    ConcurrentFixture fx("uptime");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());

    auto firstResponse =
        client.Request(Spark::Daemon::ServiceId::Control,
                       static_cast<uint16_t>(Spark::Daemon::ControlMessage::StatsRequest), /*payload*/ {});
    EXPECT_TRUE(firstResponse.has_value());
    Spark::Daemon::DaemonStats firstStats;
    EXPECT_TRUE(Spark::Daemon::DecodeDaemonStats(firstResponse->payload, firstStats));

    // Sleep for a bit over 1 second so the integer-seconds uptime ticks over
    // even on the tightest CI runner.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto secondResponse =
        client.Request(Spark::Daemon::ServiceId::Control,
                       static_cast<uint16_t>(Spark::Daemon::ControlMessage::StatsRequest), /*payload*/ {});
    EXPECT_TRUE(secondResponse.has_value());
    Spark::Daemon::DaemonStats secondStats;
    EXPECT_TRUE(Spark::Daemon::DecodeDaemonStats(secondResponse->payload, secondStats));

    EXPECT_TRUE(secondStats.uptimeSeconds >= firstStats.uptimeSeconds);
    EXPECT_TRUE(secondStats.uptimeSeconds - firstStats.uptimeSeconds >= 1u);
}

// =========================================================================
// Wire-format codec sanity
// =========================================================================

TEST(DaemonStats_CodecRoundTrip)
{
    Spark::Daemon::DaemonStats in;
    in.uptimeSeconds = 12345u;
    in.protocolVersion = "9.9.9-beta";
    in.registeredIds = {0x0000u, 0x0002u, 0x0003u, 0x00FFu};

    auto bytes = Spark::Daemon::EncodeDaemonStats(in);
    Spark::Daemon::DaemonStats out;
    EXPECT_TRUE(Spark::Daemon::DecodeDaemonStats(bytes, out));
    EXPECT_EQ(out.uptimeSeconds, in.uptimeSeconds);
    EXPECT_EQ(out.protocolVersion, in.protocolVersion);
    EXPECT_EQ(out.registeredIds.size(), in.registeredIds.size());
    for (size_t i = 0; i < in.registeredIds.size(); ++i)
        EXPECT_EQ(out.registeredIds[i], in.registeredIds[i]);
}

TEST(DaemonStats_CodecRejectsTruncatedPayload)
{
    Spark::Daemon::DaemonStats in;
    in.uptimeSeconds = 42u;
    in.protocolVersion = "x";
    in.registeredIds = {1u};
    auto bytes = Spark::Daemon::EncodeDaemonStats(in);

    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
    Spark::Daemon::DaemonStats out;
    EXPECT_FALSE(Spark::Daemon::DecodeDaemonStats(truncated, out));
}

#endif // supported local IPC platforms
