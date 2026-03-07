/**
 * @file ChromeTracing.h
 * @brief Chrome tracing (chrome://tracing) JSON exporter
 * @author Spark Engine Team
 * @date 2026
 *
 * Records profiling events and exports them in Chrome Trace Event Format.
 * Visualize output in Chrome (chrome://tracing) or Perfetto (https://ui.perfetto.dev/).
 *
 * ## Usage
 * @code
 *   // Enable tracing
 *   Spark::ChromeTracing::GetInstance().Start();
 *
 *   // In your code
 *   {
 *       SPARK_TRACE_SCOPE("Render");
 *       // ... rendering code ...
 *   }
 *
 *   // Save results
 *   Spark::ChromeTracing::GetInstance().Stop();
 *   Spark::ChromeTracing::GetInstance().SaveToFile("trace.json");
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <fstream>
#include <sstream>
#include <thread>
#include <cstdint>

namespace Spark {

class ChromeTracing {
public:
    static ChromeTracing& GetInstance() {
        static ChromeTracing instance;
        return instance;
    }

    void Start() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.clear();
        m_events.reserve(100000);
        m_active = true;
        m_startTime = Clock::now();
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
    }

    bool IsActive() const { return m_active; }

    /// Record the beginning of a duration event
    void BeginEvent(const char* name, const char* category = "default") {
        if (!m_active) return;
        TraceEvent ev;
        ev.name = name;
        ev.category = category;
        ev.phase = 'B'; // Begin
        ev.timestamp = GetTimestampUs();
        ev.threadId = GetThreadId();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(ev);
        }
    }

    /// Record the end of a duration event
    void EndEvent(const char* name, const char* category = "default") {
        if (!m_active) return;
        TraceEvent ev;
        ev.name = name;
        ev.category = category;
        ev.phase = 'E'; // End
        ev.timestamp = GetTimestampUs();
        ev.threadId = GetThreadId();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(ev);
        }
    }

    /// Record an instant event
    void InstantEvent(const char* name, const char* category = "default") {
        if (!m_active) return;
        TraceEvent ev;
        ev.name = name;
        ev.category = category;
        ev.phase = 'i'; // Instant
        ev.timestamp = GetTimestampUs();
        ev.threadId = GetThreadId();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(ev);
        }
    }

    /// Save all recorded events to a JSON file
    bool SaveToFile(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream file(filepath);
        if (!file.is_open()) return false;

        file << "{\"traceEvents\":[\n";
        for (size_t i = 0; i < m_events.size(); ++i) {
            const auto& ev = m_events[i];
            file << "  {\"name\":\"" << ev.name
                 << "\",\"cat\":\"" << ev.category
                 << "\",\"ph\":\"" << ev.phase
                 << "\",\"ts\":" << ev.timestamp
                 << ",\"pid\":1"
                 << ",\"tid\":" << ev.threadId
                 << "}";
            if (i + 1 < m_events.size()) file << ",";
            file << "\n";
        }
        file << "]}\n";

        return file.good();
    }

    /// Get the number of recorded events
    size_t EventCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events.size();
    }

    /// Clear all recorded events
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.clear();
    }

private:
    ChromeTracing() = default;
    ChromeTracing(const ChromeTracing&) = delete;
    ChromeTracing& operator=(const ChromeTracing&) = delete;

    using Clock = std::chrono::high_resolution_clock;

    struct TraceEvent {
        const char* name = "";
        const char* category = "";
        char phase = 'B';
        int64_t timestamp = 0; // microseconds
        uint64_t threadId = 0;
    };

    int64_t GetTimestampUs() const {
        auto now = Clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_startTime).count();
    }

    static uint64_t GetThreadId() {
        return static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    mutable std::mutex m_mutex;
    std::vector<TraceEvent> m_events;
    Clock::time_point m_startTime;
    bool m_active = false;
};

/// RAII scope guard for chrome tracing
class ScopedTraceEvent {
public:
    ScopedTraceEvent(const char* name, const char* category = "default")
        : m_name(name), m_category(category)
    {
        ChromeTracing::GetInstance().BeginEvent(name, category);
    }
    ~ScopedTraceEvent() {
        ChromeTracing::GetInstance().EndEvent(m_name, m_category);
    }
    ScopedTraceEvent(const ScopedTraceEvent&) = delete;
    ScopedTraceEvent& operator=(const ScopedTraceEvent&) = delete;

private:
    const char* m_name;
    const char* m_category;
};

} // namespace Spark

// Convenience macros
#define SPARK_TRACE_CONCAT_INNER(a, b) a##b
#define SPARK_TRACE_CONCAT(a, b) SPARK_TRACE_CONCAT_INNER(a, b)
#define SPARK_TRACE_SCOPE(name) \
    ::Spark::ScopedTraceEvent SPARK_TRACE_CONCAT(_traceScope_, __LINE__)(name)
#define SPARK_TRACE_SCOPE_CAT(name, cat) \
    ::Spark::ScopedTraceEvent SPARK_TRACE_CONCAT(_traceScope_, __LINE__)(name, cat)
