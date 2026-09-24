/**
 * @file ServerHealth.cpp
 * @brief SparkServer operator health metrics, JSON serialization, and atomic health-file publication.
 */

#include "ServerHealth.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h depends on the Windows types above, so it must follow windows.h.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace Spark::Server
{
    namespace
    {
        std::string EscapeJson(std::string_view value)
        {
            std::ostringstream stream;
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '"':
                    stream << "\\\"";
                    break;
                case '\\':
                    stream << "\\\\";
                    break;
                case '\n':
                    stream << "\\n";
                    break;
                case '\r':
                    stream << "\\r";
                    break;
                case '\t':
                    stream << "\\t";
                    break;
                default:
                    if (character < 0x20)
                        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<unsigned int>(character) << std::dec;
                    else
                        stream << static_cast<char>(character);
                }
            }
            return stream.str();
        }

        const char* JsonBool(bool value)
        {
            return value ? "true" : "false";
        }
    } // namespace

    void TickLatencyHistogram::Record(std::chrono::nanoseconds duration) noexcept
    {
        const int64_t nanoseconds = std::max<int64_t>(duration.count(), 0);
        // Round up so a percentile derived from a bucket bound never under-reports.
        const uint64_t micros = (static_cast<uint64_t>(nanoseconds) + 999) / 1000;
        const auto bucket = std::lower_bound(BucketUpperBoundsMicros.begin(), BucketUpperBoundsMicros.end(), micros);
        const size_t index = static_cast<size_t>(bucket - BucketUpperBoundsMicros.begin());

        // Publish the maximum before the count. A reader that observes the new
        // count (acquire) is then guaranteed to observe a maximum at least this
        // large, so the percentile clamp below can never hide this sample.
        if (micros > m_maxMicros.load(std::memory_order_relaxed))
            m_maxMicros.store(micros, std::memory_order_release);
        m_counts[index].fetch_add(1, std::memory_order_release);
    }

    TickLatencySummary TickLatencyHistogram::Summarize() const noexcept
    {
        std::array<uint64_t, BucketUpperBoundsMicros.size() + 1> counts{};
        TickLatencySummary summary;
        for (size_t index = 0; index < counts.size(); ++index)
        {
            counts[index] = m_counts[index].load(std::memory_order_acquire);
            summary.samples += counts[index];
        }
        if (summary.samples == 0)
            return summary;
        summary.maxMicros = m_maxMicros.load(std::memory_order_acquire);

        auto percentile = [&](uint64_t percent)
        {
            // Nearest-rank definition: the smallest sample with at least
            // percent% of samples at or below it.
            const uint64_t rank = std::max<uint64_t>(1, (summary.samples * percent + 99) / 100);
            uint64_t cumulative = 0;
            for (size_t index = 0; index < BucketUpperBoundsMicros.size(); ++index)
            {
                cumulative += counts[index];
                if (cumulative >= rank)
                    return std::min(BucketUpperBoundsMicros[index], summary.maxMicros);
            }
            return summary.maxMicros;
        };
        summary.p50Micros = percentile(50);
        summary.p95Micros = percentile(95);
        summary.p99Micros = percentile(99);
        return summary;
    }

    void TickLatencyHistogram::Reset() noexcept
    {
        for (auto& count : m_counts)
            count.store(0, std::memory_order_relaxed);
        m_maxMicros.store(0, std::memory_order_relaxed);
    }

    std::optional<uint64_t> QueryResidentSetBytes() noexcept
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
            return std::nullopt;
        return static_cast<uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
            KERN_SUCCESS)
            return std::nullopt;
        return static_cast<uint64_t>(info.resident_size);
#elif defined(__linux__)
        // statm reports sizes in pages: total program size, then resident set.
        std::FILE* statm = std::fopen("/proc/self/statm", "r");
        if (!statm)
            return std::nullopt;
        unsigned long long totalPages = 0;
        unsigned long long residentPages = 0;
        const int fields = std::fscanf(statm, "%llu %llu", &totalPages, &residentPages);
        std::fclose(statm);
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (fields != 2 || pageSize <= 0)
            return std::nullopt;
        return static_cast<uint64_t>(residentPages) * static_cast<uint64_t>(pageSize);
#else
        return std::nullopt;
#endif
    }

    std::string FormatHealthJson(const ServerHealth& health)
    {
        std::ostringstream stream;
        stream << "{\"live\":" << JsonBool(health.live) << ",\"ready\":" << JsonBool(health.ready)
               << ",\"draining\":" << JsonBool(health.draining) << ",\"stopping\":" << JsonBool(health.stopping)
               << ",\"port\":" << health.port << ",\"players\":" << health.players << ",\"ticks\":" << health.ticks
               << ",\"loadedModules\":" << health.loadedModules << ",\"gameModule\":\"" << EscapeJson(health.gameModule)
               << "\",\"map\":\"" << EscapeJson(health.currentMap) << "\",\"error\":\"" << EscapeJson(health.lastError)
               << "\",\"version\":\"" << EscapeJson(health.build.version) << "\",\"commit\":\""
               << EscapeJson(health.build.commit) << "\",\"treeState\":\"" << EscapeJson(health.build.treeState)
               << "\",\"tickSamples\":" << health.tickLatency.samples
               << ",\"tickP50Us\":" << health.tickLatency.p50Micros << ",\"tickP95Us\":" << health.tickLatency.p95Micros
               << ",\"tickP99Us\":" << health.tickLatency.p99Micros << ",\"tickMaxUs\":" << health.tickLatency.maxMicros
               << ",\"rssBytes\":";
        if (health.residentSetBytes)
            stream << *health.residentSetBytes;
        else
            stream << "null";
        stream << '}';
        return stream.str();
    }

    void WriteHealthFile(const std::filesystem::path& path, std::string_view json)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        const std::filesystem::path temporary = path.string() + ".tmp";
        bool wrote = false;
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << json << '\n';
            output.flush();
            wrote = output.good();
        }
        if (!wrote)
        {
            std::filesystem::remove(temporary, error);
            return;
        }
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            error.clear();
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
            if (error)
                std::filesystem::remove(temporary, error);
        }
    }
} // namespace Spark::Server
