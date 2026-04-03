/**
 * @file HitchDetector.cpp
 * @brief Frame hitch detection — rolling average analysis and spike classification
 */

#include "../Core/Platform.h"
#include "Utils/HitchDetector.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <format>

namespace Spark
{

    HitchDetector& HitchDetector::GetInstance()
    {
        static HitchDetector instance;
        return instance;
    }

    void HitchDetector::Initialize()
    {
        if (m_initialized)
            return;

        Reset();
        m_initialized = true;
    }

    void HitchDetector::Shutdown()
    {
        m_initialized = false;
    }

    void HitchDetector::Configure(const HitchDetectorConfig& config)
    {
        m_config = config;
    }

    void HitchDetector::Reset()
    {
        m_frameTimes.fill(0.0f);
        m_frameIndex = 0;
        m_framesFilled = 0;
        m_mildCount = 0;
        m_moderateCount = 0;
        m_severeCount = 0;
        m_worstHitchMs = 0.0f;
        m_lastHitchSeverity = HitchSeverity::None;
        m_framesAnalyzed = 0;
        m_uptimeSec = 0.0f;
        m_hitchesInCurrentMinute = 0;
        m_currentMinuteElapsed = 0.0f;
        m_hitchesPerMinute = 0.0f;
    }

    void HitchDetector::Update(float dt)
    {
        if (!m_config.enabled || !m_initialized)
            return;

        float frameTimeMs = dt * 1000.0f;
        m_framesAnalyzed++;
        m_uptimeSec += dt;
        m_currentMinuteElapsed += dt;

        // Store in ring buffer
        uint32_t windowSize = std::min(m_config.historyWindowSize, MAX_HISTORY);
        m_frameTimes[m_frameIndex % windowSize] = frameTimeMs;
        m_frameIndex++;
        if (m_framesFilled < windowSize)
            m_framesFilled++;

        // Need enough samples before detecting hitches
        if (m_framesFilled < 10)
            return;

        float avg = ComputeRollingAverage();

        // Skip if average is below minimum threshold
        if (avg < m_config.minFrameTimeMs)
            return;

        // Classify hitch severity (check highest first)
        HitchSeverity severity = HitchSeverity::None;
        if (frameTimeMs >= avg * m_config.severeMultiplier)
            severity = HitchSeverity::Severe;
        else if (frameTimeMs >= avg * m_config.moderateMultiplier)
            severity = HitchSeverity::Moderate;
        else if (frameTimeMs >= avg * m_config.mildMultiplier)
            severity = HitchSeverity::Mild;

        if (severity != HitchSeverity::None)
        {
            m_lastHitchSeverity = severity;
            m_hitchesInCurrentMinute++;

            if (frameTimeMs > m_worstHitchMs)
                m_worstHitchMs = frameTimeMs;

            switch (severity)
            {
            case HitchSeverity::Severe:
                m_severeCount++;
                break;
            case HitchSeverity::Moderate:
                m_moderateCount++;
                break;
            case HitchSeverity::Mild:
                m_mildCount++;
                break;
            default:
                break;
            }
        }

        // Update hitches-per-minute every 60 seconds
        if (m_currentMinuteElapsed >= 60.0f)
        {
            m_hitchesPerMinute = static_cast<float>(m_hitchesInCurrentMinute);
            m_hitchesInCurrentMinute = 0;
            m_currentMinuteElapsed = 0.0f;
        }
    }

    float HitchDetector::ComputeRollingAverage() const
    {
        if (m_framesFilled == 0)
            return 0.0f;

        float sum = 0.0f;
        for (uint32_t i = 0; i < m_framesFilled; i++)
            sum += m_frameTimes[i];

        return sum / static_cast<float>(m_framesFilled);
    }

    HitchDetectorStatus HitchDetector::GetStatus() const
    {
        HitchDetectorStatus status;
        status.currentAvgFrameTimeMs = ComputeRollingAverage();
        status.worstHitchMs = m_worstHitchMs;
        status.lastHitchSeverity = m_lastHitchSeverity;
        status.mildCount = m_mildCount;
        status.moderateCount = m_moderateCount;
        status.severeCount = m_severeCount;
        status.totalHitches = m_mildCount + m_moderateCount + m_severeCount;
        status.hitchesPerMinute = m_hitchesPerMinute;
        status.framesAnalyzed = m_framesAnalyzed;
        return status;
    }

    // ========================================================================
    // Console commands
    // ========================================================================

    static const char* SeverityToString(HitchSeverity s)
    {
        switch (s)
        {
        case HitchSeverity::None:
            return "None";
        case HitchSeverity::Mild:
            return "Mild";
        case HitchSeverity::Moderate:
            return "Moderate";
        case HitchSeverity::Severe:
            return "Severe";
        }
        return "Unknown";
    }

    void HitchDetector::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "hitch.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto status = HitchDetector::GetInstance().GetStatus();
                return std::format("Avg frame: {:.2f}ms | Worst hitch: {:.2f}ms | Last: {}\n"
                                   "Mild: {} | Moderate: {} | Severe: {} | Total: {}\n"
                                   "Hitches/min: {:.1f} | Frames analyzed: {}",
                                   status.currentAvgFrameTimeMs, status.worstHitchMs,
                                   SeverityToString(status.lastHitchSeverity), status.mildCount, status.moderateCount,
                                   status.severeCount, status.totalHitches, status.hitchesPerMinute,
                                   status.framesAnalyzed);
            },
            "Show hitch detector status", "HitchDetector");

        console.RegisterCommand(
            "hitch.config",
            [](const std::vector<std::string>&) -> std::string
            {
                const auto& cfg = HitchDetector::GetInstance().GetConfig();
                return std::format("Enabled: {} | Mild: {:.1f}x | Moderate: {:.1f}x | Severe: {:.1f}x\n"
                                   "Min frame time: {:.1f}ms | History window: {} frames",
                                   cfg.enabled ? "yes" : "no", cfg.mildMultiplier, cfg.moderateMultiplier,
                                   cfg.severeMultiplier, cfg.minFrameTimeMs, cfg.historyWindowSize);
            },
            "Show hitch detector configuration", "HitchDetector");

        console.RegisterCommand(
            "hitch.reset",
            [](const std::vector<std::string>&) -> std::string
            {
                HitchDetector::GetInstance().Reset();
                return "Hitch detector statistics reset.";
            },
            "Reset hitch detector statistics", "HitchDetector");
    }

} // namespace Spark
