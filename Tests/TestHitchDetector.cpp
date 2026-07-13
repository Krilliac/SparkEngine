/**
 * @file TestHitchDetector.cpp
 * @brief Tests for the HitchDetector frame-spike detection system
 */

#include "TestFramework.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Inline minimal HitchDetector reproduction for unit testing
// (avoids pulling in full engine headers — mirrors the real implementation)
// ============================================================================

namespace TestHitch
{

    enum class HitchSeverity : uint8_t
    {
        None,
        Mild,
        Moderate,
        Severe
    };

    struct HitchDetectorConfig
    {
        float mildMultiplier = 2.0f;
        float moderateMultiplier = 4.0f;
        float severeMultiplier = 8.0f;
        float minFrameTimeMs = 4.0f;
        uint32_t historyWindowSize = 120;
        bool enabled = true;
    };

    struct HitchDetectorStatus
    {
        float currentAvgFrameTimeMs = 0.0f;
        float worstHitchMs = 0.0f;
        HitchSeverity lastHitchSeverity = HitchSeverity::None;
        uint32_t mildCount = 0;
        uint32_t moderateCount = 0;
        uint32_t severeCount = 0;
        uint32_t totalHitches = 0;
        float hitchesPerMinute = 0.0f;
        uint64_t framesAnalyzed = 0;
    };

    class HitchDetector
    {
      public:
        void Initialize()
        {
            if (m_initialized)
                return;
            Reset();
            m_initialized = true;
        }

        void Shutdown() { m_initialized = false; }

        void Configure(const HitchDetectorConfig& config) { m_config = config; }
        const HitchDetectorConfig& GetConfig() const { return m_config; }

        void Update(float dt)
        {
            if (!m_config.enabled || !m_initialized)
                return;

            float frameTimeMs = dt * 1000.0f;
            m_framesAnalyzed++;
            m_uptimeSec += dt;
            m_currentMinuteElapsed += dt;

            uint32_t windowSize = m_config.historyWindowSize;
            if (windowSize > MAX_HISTORY)
                windowSize = MAX_HISTORY;

            // Baseline is snapshotted BEFORE this frame enters the window --
            // mirrors HitchDetector::Update (a spike must not dilute the average
            // it is compared against, or it under-classifies itself).
            const bool haveBaseline = (m_framesFilled >= 10);
            const float avg = haveBaseline ? ComputeRollingAverage() : 0.0f;

            m_frameTimes[m_frameIndex % windowSize] = frameTimeMs;
            m_frameIndex++;
            if (m_framesFilled < windowSize)
                m_framesFilled++;

            if (!haveBaseline)
                return;

            if (avg < m_config.minFrameTimeMs)
                return;

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

            if (m_currentMinuteElapsed >= 60.0f)
            {
                m_hitchesPerMinute = static_cast<float>(m_hitchesInCurrentMinute);
                m_hitchesInCurrentMinute = 0;
                m_currentMinuteElapsed = 0.0f;
            }
        }

        void Reset()
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

        HitchDetectorStatus GetStatus() const
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

      private:
        float ComputeRollingAverage() const
        {
            if (m_framesFilled == 0)
                return 0.0f;
            float sum = 0.0f;
            for (uint32_t i = 0; i < m_framesFilled; i++)
                sum += m_frameTimes[i];
            return sum / static_cast<float>(m_framesFilled);
        }

        HitchDetectorConfig m_config;
        static constexpr uint32_t MAX_HISTORY = 300;
        std::array<float, MAX_HISTORY> m_frameTimes{};
        uint32_t m_frameIndex = 0;
        uint32_t m_framesFilled = 0;
        uint32_t m_mildCount = 0;
        uint32_t m_moderateCount = 0;
        uint32_t m_severeCount = 0;
        float m_worstHitchMs = 0.0f;
        HitchSeverity m_lastHitchSeverity = HitchSeverity::None;
        uint64_t m_framesAnalyzed = 0;
        float m_uptimeSec = 0.0f;
        uint32_t m_hitchesInCurrentMinute = 0;
        float m_currentMinuteElapsed = 0.0f;
        float m_hitchesPerMinute = 0.0f;
        bool m_initialized = false;
    };

} // namespace TestHitch

// ============================================================================
// Tests
// ============================================================================

TEST(HitchDetector_InitialState)
{
    TestHitch::HitchDetector detector;
    detector.Initialize();
    auto status = detector.GetStatus();
    EXPECT_TRUE(status.totalHitches == 0);
    EXPECT_TRUE(status.framesAnalyzed == 0);
    EXPECT_TRUE(status.worstHitchMs == 0.0f);
    EXPECT_TRUE(status.lastHitchSeverity == TestHitch::HitchSeverity::None);
}

TEST(HitchDetector_SteadyFramesNoHitches)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    // Feed 60 steady frames at 16.6ms (60 FPS)
    for (int i = 0; i < 60; i++)
        detector.Update(0.0166f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.totalHitches == 0);
    EXPECT_TRUE(status.framesAnalyzed == 60);
}

TEST(HitchDetector_DetectsMildHitch)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    // Build up a baseline of 16.6ms frames
    for (int i = 0; i < 30; i++)
        detector.Update(0.0166f);

    // Inject a 2.5x spike (mild)
    detector.Update(0.0166f * 2.5f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.mildCount >= 1);
    EXPECT_TRUE(status.lastHitchSeverity == TestHitch::HitchSeverity::Mild);
}

TEST(HitchDetector_DetectsModerateHitch)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    for (int i = 0; i < 30; i++)
        detector.Update(0.0166f);

    // Inject a 5x spike (moderate)
    detector.Update(0.0166f * 5.0f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.moderateCount >= 1);
    EXPECT_TRUE(status.lastHitchSeverity == TestHitch::HitchSeverity::Moderate);
}

TEST(HitchDetector_DetectsSevereHitch)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    for (int i = 0; i < 30; i++)
        detector.Update(0.0166f);

    // Inject a 10x spike (severe)
    detector.Update(0.0166f * 10.0f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.severeCount >= 1);
    EXPECT_TRUE(status.lastHitchSeverity == TestHitch::HitchSeverity::Severe);
}

TEST(HitchDetector_TracksWorstHitch)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    for (int i = 0; i < 30; i++)
        detector.Update(0.0166f);

    // Two spikes — second is larger
    detector.Update(0.0166f * 3.0f); // ~49.8ms
    for (int i = 0; i < 10; i++)
        detector.Update(0.0166f);
    detector.Update(0.0166f * 6.0f); // ~99.6ms

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.worstHitchMs > 90.0f);
}

TEST(HitchDetector_ResetClearsStats)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    for (int i = 0; i < 30; i++)
        detector.Update(0.0166f);
    detector.Update(0.0166f * 5.0f);

    detector.Reset();
    auto status = detector.GetStatus();
    EXPECT_TRUE(status.totalHitches == 0);
    EXPECT_TRUE(status.worstHitchMs == 0.0f);
    EXPECT_TRUE(status.framesAnalyzed == 0);
}

TEST(HitchDetector_DisabledIgnoresFrames)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.enabled = false;
    detector.Configure(cfg);
    detector.Initialize();

    for (int i = 0; i < 60; i++)
        detector.Update(0.0166f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.framesAnalyzed == 0);
}

TEST(HitchDetector_MinFrameTimeFilter)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 10.0f; // High threshold — avg ~1.66ms won't qualify
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    // Very fast frames (~1.66ms = 600 FPS)
    for (int i = 0; i < 30; i++)
        detector.Update(0.00166f);

    // Spike that's 5x but avg is below minFrameTimeMs
    detector.Update(0.00166f * 5.0f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.totalHitches == 0);
}

TEST(HitchDetector_StatusSnapshot)
{
    TestHitch::HitchDetector detector;
    TestHitch::HitchDetectorConfig cfg;
    cfg.minFrameTimeMs = 1.0f;
    cfg.historyWindowSize = 30;
    detector.Configure(cfg);
    detector.Initialize();

    for (int i = 0; i < 30; i++)
        detector.Update(0.0166f);

    auto status = detector.GetStatus();
    // Average should be close to 16.6ms
    EXPECT_TRUE(status.currentAvgFrameTimeMs > 15.0f);
    EXPECT_TRUE(status.currentAvgFrameTimeMs < 18.0f);
    EXPECT_TRUE(status.framesAnalyzed == 30);
}
