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

#if defined(__linux__) || defined(__APPLE__)

#include "Utils/DaemonClient.h"
#include "Utils/DaemonProtocol.h"
#include "Utils/ShaderServiceClient.h"

#include "../SparkDaemon/src/ControlService.h"
#include "../SparkDaemon/src/DaemonServer.h"
#include "../SparkDaemon/src/ShaderService.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace
{
    std::string UniqueConcurrentSockPath(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-daemon-concurrent-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
    }

    bool WaitForConcurrentSocket(const std::string& path, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            struct stat st;
            if (::stat(path.c_str(), &st) == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    struct ConcurrentFixture
    {
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;
        std::string sockPath;

        explicit ConcurrentFixture(const char* tag) : sockPath(UniqueConcurrentSockPath(tag))
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
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
            ::unlink(sockPath.c_str());
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

    std::thread writer(
        [&]
        {
            Spark::Daemon::DaemonClient client;
            EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
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
            EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
            for (int i = 0; i < kIterations; ++i)
            {
                if (client.Ping())
                    pingsOk.fetch_add(1, std::memory_order_relaxed);
            }
            client.Disconnect();
        });

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

#endif // POSIX
