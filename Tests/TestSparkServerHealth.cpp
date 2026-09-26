/**
 * @file TestSparkServerHealth.cpp
 * @brief SparkServer operator health surface: build identity, tick percentiles, RSS, and drain ordering.
 */

#include "TestFramework.h"
#include "ServerApplication.h"
#include "ServerHealth.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Spark::Server;
using namespace std::chrono_literals;

namespace
{
    /** Return the raw JSON token for a top-level key: `"x"`, `true`, `12`, or `null`. */
    std::optional<std::string> JsonField(std::string_view json, std::string_view key)
    {
        const std::string needle = "\"" + std::string(key) + "\":";
        const size_t start = json.find(needle);
        if (start == std::string_view::npos)
            return std::nullopt;
        size_t cursor = start + needle.size();
        size_t end = cursor;
        if (cursor < json.size() && json[cursor] == '"')
        {
            end = cursor + 1;
            while (end < json.size() && json[end] != '"')
                end += json[end] == '\\' ? 2 : 1;
            ++end;
        }
        else
        {
            while (end < json.size() && json[end] != ',' && json[end] != '}')
                ++end;
        }
        return std::string(json.substr(cursor, end - cursor));
    }

    class ScopedGameModuleKind
    {
      public:
        ScopedGameModuleKind()
        {
            if (const char* previous = std::getenv("SPARK_MODULE_ABI_KIND_GAME"))
                m_previous = previous;
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_MODULE_ABI_KIND_GAME", "1");
#else
            setenv("SPARK_MODULE_ABI_KIND_GAME", "1", 1);
#endif
        }

        ~ScopedGameModuleKind()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_MODULE_ABI_KIND_GAME", m_previous ? m_previous->c_str() : "");
#else
            if (m_previous)
                setenv("SPARK_MODULE_ABI_KIND_GAME", m_previous->c_str(), 1);
            else
                unsetenv("SPARK_MODULE_ABI_KIND_GAME");
#endif
        }

        ScopedGameModuleKind(const ScopedGameModuleKind&) = delete;
        ScopedGameModuleKind& operator=(const ScopedGameModuleKind&) = delete;

      private:
        std::optional<std::string> m_previous;
    };

    /** Redirect std::cout, where SparkServer publishes each health snapshot, for the scope's lifetime. */
    class ScopedStdoutCapture
    {
      public:
        ScopedStdoutCapture() : m_previous(std::cout.rdbuf(m_buffer.rdbuf())) {}
        ~ScopedStdoutCapture() { std::cout.rdbuf(m_previous); }

        ScopedStdoutCapture(const ScopedStdoutCapture&) = delete;
        ScopedStdoutCapture& operator=(const ScopedStdoutCapture&) = delete;

        [[nodiscard]] std::vector<std::string> Lines() const
        {
            std::vector<std::string> lines;
            std::istringstream stream(m_buffer.str());
            std::string line;
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.front() == '{')
                    lines.push_back(line);
            }
            return lines;
        }

      private:
        std::ostringstream m_buffer;
        std::streambuf* m_previous;
    };

    ServerOptions LoopbackServerOptions()
    {
        ServerOptions options;
        options.modulePath = SPARK_TEST_COMPATIBLE_MODULE_PATH;
        options.server.port = 0;
        options.server.endpointPolicy = Spark::Net::NetworkEndpointPolicy::Loopback();
        options.server.enableLogging = false;
        options.server.tickRate = 120.0f;
        return options;
    }
} // namespace

TEST(Server_Health_TickHistogramReportsBucketBoundedPercentiles)
{
    TickLatencyHistogram histogram;
    EXPECT_EQ(histogram.Summarize().samples, uint64_t{0});

    // 90 fast ticks (<= 100us), 9 medium ticks (<= 2ms), and one 12ms spike.
    for (int index = 0; index < 90; ++index)
        histogram.Record(80us);
    for (int index = 0; index < 9; ++index)
        histogram.Record(1500us);
    histogram.Record(12ms);

    const TickLatencySummary summary = histogram.Summarize();
    EXPECT_EQ(summary.samples, uint64_t{100});
    EXPECT_EQ(summary.p50Micros, uint64_t{100});
    EXPECT_EQ(summary.p95Micros, uint64_t{2000});
    EXPECT_EQ(summary.p99Micros, uint64_t{2000});
    EXPECT_EQ(summary.maxMicros, uint64_t{12000});

    // A partial microsecond rounds up so a bound never under-reports a sample.
    TickLatencyHistogram rounding;
    rounding.Record(std::chrono::nanoseconds(50001));
    EXPECT_EQ(rounding.Summarize().maxMicros, uint64_t{51});
    EXPECT_EQ(rounding.Summarize().p50Micros, uint64_t{51});

    histogram.Reset();
    EXPECT_EQ(histogram.Summarize().samples, uint64_t{0});
    EXPECT_EQ(histogram.Summarize().maxMicros, uint64_t{0});
}

TEST(Server_Health_TickHistogramOverflowReportsObservedMax)
{
    TickLatencyHistogram histogram;
    histogram.Record(3s);
    histogram.Record(2500ms);
    const TickLatencySummary summary = histogram.Summarize();
    EXPECT_EQ(summary.samples, uint64_t{2});
    EXPECT_EQ(summary.p50Micros, uint64_t{3000000});
    EXPECT_EQ(summary.p99Micros, uint64_t{3000000});
    EXPECT_EQ(summary.maxMicros, uint64_t{3000000});

    // Percentiles are clamped to the observed maximum inside a bucket.
    TickLatencyHistogram clamped;
    clamped.Record(120us);
    EXPECT_EQ(clamped.Summarize().p99Micros, uint64_t{120});
}

TEST(Server_Health_ResidentSetIsMeasured)
{
    const std::optional<uint64_t> rss = QueryResidentSetBytes();
    ASSERT_TRUE(rss.has_value());
    // The test process has at least loaded the engine; anything under 1 MiB is a units bug.
    EXPECT_TRUE(*rss >= uint64_t{1024} * 1024);
}

TEST(Server_Health_VersionRequestNeedsNoModuleAndIdentityDefaultsUnknown)
{
    const std::array<std::string_view, 1> arguments = {"--version"};
    const ParseResult parsed = ParseServerOptions(arguments);
    ASSERT_TRUE(parsed.options.has_value());
    EXPECT_TRUE(parsed.options->showVersion);
    EXPECT_FALSE(parsed.options->showHelp);
    EXPECT_TRUE(ServerHelpText().find("--version") != std::string_view::npos);

    // An embedder that supplies no stamp publishes "unknown", never a guessed identity.
    const BuildIdentity unstamped = parsed.options->build;
    EXPECT_EQ(std::string(unstamped.version), std::string("unknown"));
    EXPECT_EQ(std::string(unstamped.commit), std::string("unknown"));
    EXPECT_EQ(std::string(unstamped.treeState), std::string("unknown"));
    const std::string json = FormatHealthJson(ServerHealth{});
    EXPECT_EQ(JsonField(json, "commit").value_or(""), std::string("\"unknown\""));
}

TEST(Server_Health_JsonCarriesOperatorFields)
{
    ServerHealth health;
    health.live = true;
    health.draining = true;
    health.port = 27015;
    health.gameModule = "Game\"One";
    health.build = {"1.2.3", "0123456789abcdef0123456789abcdef01234567", "dirty"};
    health.tickLatency = {42, 100, 2000, 4000, 3500};
    health.residentSetBytes = 123456789;

    const std::string json = FormatHealthJson(health);
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_EQ(JsonField(json, "ready").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(json, "draining").value_or(""), std::string("true"));
    EXPECT_EQ(JsonField(json, "stopping").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(json, "gameModule").value_or(""), std::string("\"Game\\\"One\""));
    EXPECT_EQ(JsonField(json, "version").value_or(""), std::string("\"1.2.3\""));
    EXPECT_EQ(JsonField(json, "commit").value_or(""), std::string("\"0123456789abcdef0123456789abcdef01234567\""));
    EXPECT_EQ(JsonField(json, "treeState").value_or(""), std::string("\"dirty\""));
    EXPECT_EQ(JsonField(json, "tickSamples").value_or(""), std::string("42"));
    EXPECT_EQ(JsonField(json, "tickP50Us").value_or(""), std::string("100"));
    EXPECT_EQ(JsonField(json, "tickP95Us").value_or(""), std::string("2000"));
    EXPECT_EQ(JsonField(json, "tickP99Us").value_or(""), std::string("4000"));
    EXPECT_EQ(JsonField(json, "tickMaxUs").value_or(""), std::string("3500"));
    EXPECT_EQ(JsonField(json, "rssBytes").value_or(""), std::string("123456789"));

    // An unavailable measurement is explicit, never a fabricated zero.
    health.residentSetBytes.reset();
    EXPECT_EQ(JsonField(FormatHealthJson(health), "rssBytes").value_or(""), std::string("null"));
}

TEST(Server_Health_RunPublishesDrainingBeforeStopping)
{
    const ScopedGameModuleKind gameKind;
    ServerOptions options = LoopbackServerOptions();
    options.runFor = 150ms;
    options.statusInterval = 100ms;
    options.build = {"4.5.6", "fedcba9876543210fedcba9876543210fedcba98", "clean"};

    std::vector<std::string> lines;
    int exitCode = -1;
    {
        const ScopedStdoutCapture capture;
        ServerApplication application(std::move(options));
        ASSERT_TRUE(application.Start());
        exitCode = application.Run();
        lines = capture.Lines();
    }
    EXPECT_EQ(exitCode, 0);
    ASSERT_TRUE(lines.size() >= 3);

    // Startup publishes a ready snapshot carrying the build identity.
    EXPECT_EQ(JsonField(lines.front(), "ready").value_or(""), std::string("true"));
    EXPECT_EQ(JsonField(lines.front(), "draining").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(lines.front(), "version").value_or(""), std::string("\"4.5.6\""));
    EXPECT_EQ(JsonField(lines.front(), "commit").value_or(""),
              std::string("\"fedcba9876543210fedcba9876543210fedcba98\""));
    EXPECT_EQ(JsonField(lines.front(), "treeState").value_or(""), std::string("\"clean\""));

    size_t firstDraining = lines.size();
    size_t firstStopping = lines.size();
    for (size_t index = 0; index < lines.size(); ++index)
    {
        if (firstDraining == lines.size() && JsonField(lines[index], "draining") == "true")
            firstDraining = index;
        if (firstStopping == lines.size() && JsonField(lines[index], "stopping") == "true")
            firstStopping = index;
    }
    ASSERT_TRUE(firstDraining < lines.size());
    ASSERT_TRUE(firstStopping < lines.size());
    // A supervisor sees ready=false while the process is still live and before teardown begins.
    EXPECT_TRUE(firstDraining < firstStopping);
    const std::string& draining = lines[firstDraining];
    EXPECT_EQ(JsonField(draining, "live").value_or(""), std::string("true"));
    EXPECT_EQ(JsonField(draining, "ready").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(draining, "stopping").value_or(""), std::string("false"));
    for (size_t index = 0; index < firstDraining; ++index)
        EXPECT_EQ(JsonField(lines[index], "stopping").value_or(""), std::string("false"));

    // The drain snapshot carries the loop's measured tick latency and memory.
    const uint64_t samples = std::stoull(JsonField(draining, "tickSamples").value_or("0"));
    EXPECT_TRUE(samples > 0);
    const uint64_t p50 = std::stoull(JsonField(draining, "tickP50Us").value_or("0"));
    const uint64_t p99 = std::stoull(JsonField(draining, "tickP99Us").value_or("0"));
    const uint64_t maximum = std::stoull(JsonField(draining, "tickMaxUs").value_or("0"));
    EXPECT_TRUE(p50 <= p99);
    EXPECT_TRUE(p99 <= maximum);
    const std::string rss = JsonField(draining, "rssBytes").value_or("null");
    ASSERT_TRUE(rss != "null");
    EXPECT_TRUE(std::stoull(rss) > 0);

    // The final snapshot reports a fully stopped, non-draining process.
    const std::string& last = lines.back();
    EXPECT_EQ(JsonField(last, "live").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(last, "ready").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(last, "draining").value_or(""), std::string("false"));
    EXPECT_EQ(JsonField(last, "stopping").value_or(""), std::string("false"));
}
