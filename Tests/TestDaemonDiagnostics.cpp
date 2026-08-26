/**
 * @file TestDaemonDiagnostics.cpp
 * @brief Unit tests for the SparkDaemon stats formatter.
 *
 * `FormatDaemonStats` is a pure function of a `DaemonStatsSnapshot`.
 * The tests build snapshots directly and assert on the rendered output —
 * no daemon, no sockets, no connections.
 */

#include "TestFramework.h"

#include "Utils/DaemonDiagnostics.h"

using namespace Spark::Daemon;

TEST(DaemonDiagnostics_DisconnectedRendersSingleLine)
{
    DaemonStatsSnapshot snap; // socketPath empty
    const std::string out = FormatDaemonStats(snap);
    EXPECT_EQ(out, "daemon: not connected");
}

TEST(DaemonDiagnostics_ConnectedRendersHeader)
{
    DaemonStatsSnapshot snap;
    snap.socketPath = "/tmp/spark.sock";
    snap.control.protocolVersion = "1.0.0";
    snap.control.uptimeSeconds = 42;
    snap.control.registeredIds = {
        static_cast<uint16_t>(ServiceId::Control),
        static_cast<uint16_t>(ServiceId::Shader),
    };
    const std::string out = FormatDaemonStats(snap);

    EXPECT_TRUE(out.find("daemon: connected") == 0);
    EXPECT_TRUE(out.find("socket:   /tmp/spark.sock") != std::string::npos);
    EXPECT_TRUE(out.find("version:  1.0.0") != std::string::npos);
    EXPECT_TRUE(out.find("uptime:   42s") != std::string::npos);
    EXPECT_TRUE(out.find("services: control, shader") != std::string::npos);
}

TEST(DaemonDiagnostics_UptimeFormatsAcrossRanges)
{
    auto run = [](uint64_t seconds)
    {
        DaemonStatsSnapshot snap;
        snap.socketPath = "/tmp/x";
        snap.control.uptimeSeconds = seconds;
        return FormatDaemonStats(snap);
    };

    EXPECT_TRUE(run(30).find("uptime:   30s") != std::string::npos);
    EXPECT_TRUE(run(120).find("uptime:   2.0m") != std::string::npos);
    EXPECT_TRUE(run(7200).find("uptime:   2.0h") != std::string::npos);
    EXPECT_TRUE(run(172800).find("uptime:   2.0d") != std::string::npos);
}

TEST(DaemonDiagnostics_ShaderCacheRendersCounts)
{
    DaemonStatsSnapshot snap;
    snap.socketPath = "/tmp/x";
    snap.control.protocolVersion = "1.0.0";
    ShaderCacheStats shader;
    shader.entryCount = 42;
    shader.totalBytes = 3 * 1024 * 1024; // 3 MiB
    shader.hitCount = 80;
    shader.missCount = 20;
    shader.evictionCount = 5;
    snap.shader = shader;

    const std::string out = FormatDaemonStats(snap);
    EXPECT_TRUE(out.find("shader cache:") != std::string::npos);
    EXPECT_TRUE(out.find("entries:   42") != std::string::npos);
    EXPECT_TRUE(out.find("bytes:     3.0 MiB") != std::string::npos);
    EXPECT_TRUE(out.find("hit/miss:  80/20 (80.0%)") != std::string::npos);
    EXPECT_TRUE(out.find("evictions: 5") != std::string::npos);
    // Asset section must NOT appear when the optional is unset.
    EXPECT_TRUE(out.find("asset cache:") == std::string::npos);
}

TEST(DaemonDiagnostics_BothCachesBothRender)
{
    DaemonStatsSnapshot snap;
    snap.socketPath = "/tmp/x";
    snap.control.protocolVersion = "1.0.0";

    ShaderCacheStats shader;
    shader.entryCount = 1;
    snap.shader = shader;

    AssetCacheStats asset;
    asset.entryCount = 7;
    asset.totalBytes = 512; // < 1 KiB → byte suffix
    asset.hitCount = 0;
    asset.missCount = 0; // hit/miss rate reported as "—"
    asset.evictionCount = 0;
    snap.asset = asset;

    const std::string out = FormatDaemonStats(snap);
    EXPECT_TRUE(out.find("shader cache:") != std::string::npos);
    EXPECT_TRUE(out.find("asset cache:") != std::string::npos);
    EXPECT_TRUE(out.find("bytes:     512 B") != std::string::npos);
    EXPECT_TRUE(out.find("hit/miss:  0/0 (—)") != std::string::npos);
}

TEST(DaemonDiagnostics_EmptyRegisteredServicesListRendersNone)
{
    DaemonStatsSnapshot snap;
    snap.socketPath = "/tmp/x";
    snap.control.protocolVersion = "1.0.0";
    // control.registeredIds left empty — daemon reported zero services.
    const std::string out = FormatDaemonStats(snap);
    EXPECT_TRUE(out.find("services: (none)") != std::string::npos);
}

TEST(DaemonDiagnostics_UnknownServiceIdRendersHex)
{
    DaemonStatsSnapshot snap;
    snap.socketPath = "/tmp/x";
    snap.control.registeredIds = {0xABCD};
    const std::string out = FormatDaemonStats(snap);
    EXPECT_TRUE(out.find("services: unknown(0xabcd)") != std::string::npos);
}

TEST(DaemonDiagnostics_ServiceIdNameMapping)
{
    EXPECT_EQ(ServiceIdName(static_cast<uint16_t>(ServiceId::Control)), "control");
    EXPECT_EQ(ServiceIdName(static_cast<uint16_t>(ServiceId::Asset)), "asset");
    EXPECT_EQ(ServiceIdName(static_cast<uint16_t>(ServiceId::Shader)), "shader");
    EXPECT_EQ(ServiceIdName(static_cast<uint16_t>(ServiceId::Collab)), "collab");
    EXPECT_EQ(ServiceIdName(static_cast<uint16_t>(ServiceId::Build)), "build");
    EXPECT_EQ(ServiceIdName(static_cast<uint16_t>(ServiceId::Orchestration)), "orchestration");
    EXPECT_EQ(ServiceIdName(0x4242), "unknown(0x4242)");
}

TEST(DaemonDiagnostics_ParseCacheScopeKnownValues)
{
    EXPECT_TRUE(ParseDaemonCacheScope("shader") == DaemonCacheScope::Shader);
    EXPECT_TRUE(ParseDaemonCacheScope("asset") == DaemonCacheScope::Asset);
    EXPECT_TRUE(ParseDaemonCacheScope("all") == DaemonCacheScope::All);
}

TEST(DaemonDiagnostics_ParseCacheScopeInvalidReturnsNone)
{
    EXPECT_TRUE(ParseDaemonCacheScope("") == DaemonCacheScope::None);
    EXPECT_TRUE(ParseDaemonCacheScope("Shader") == DaemonCacheScope::None); // case-sensitive
    EXPECT_TRUE(ParseDaemonCacheScope("assets") == DaemonCacheScope::None); // plural rejected
    EXPECT_TRUE(ParseDaemonCacheScope("wombat") == DaemonCacheScope::None); // unknown token
}

TEST(DaemonDiagnostics_CacheScopeAllIsBitwiseUnion)
{
    // `All` is declared as `Shader | Asset`; verify the bit math holds so
    // command runners can do `(scope & Shader) != 0` reliably.
    const auto allBits = static_cast<uint8_t>(DaemonCacheScope::All);
    const auto sh = static_cast<uint8_t>(DaemonCacheScope::Shader);
    const auto as = static_cast<uint8_t>(DaemonCacheScope::Asset);
    EXPECT_EQ(allBits & sh, sh);
    EXPECT_EQ(allBits & as, as);
    EXPECT_EQ(sh & as, 0u);
}
