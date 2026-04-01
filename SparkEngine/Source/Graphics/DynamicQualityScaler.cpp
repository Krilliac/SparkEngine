/**
 * @file DynamicQualityScaler.cpp
 * @brief Implementation of dynamic resolution scaling based on frame performance
 */

#include "DynamicQualityScaler.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <numeric>

namespace Spark::Graphics
{

    void DynamicQualityScaler::Initialize(const DynamicQualityThresholds& thresholds)
    {
        m_thresholds = thresholds;

        // Validate thresholds to prevent divide-by-zero or nonsensical behavior
        if (m_thresholds.targetFPS <= 0.0f)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "DynamicQualityScaler: invalid targetFPS %.1f, defaulting to 60", m_thresholds.targetFPS);
            m_thresholds.targetFPS = 60.0f;
        }

        if (m_thresholds.minScale >= m_thresholds.maxScale)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "DynamicQualityScaler: minScale (%.2f) >= maxScale (%.2f), swapping", m_thresholds.minScale,
                           m_thresholds.maxScale);
            std::swap(m_thresholds.minScale, m_thresholds.maxScale);
        }

        m_thresholds.minScale = std::max(m_thresholds.minScale, 0.1f);
        m_thresholds.maxScale = std::min(m_thresholds.maxScale, 2.0f);

        m_currentScale = m_thresholds.maxScale;
        Reset();
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "DynamicQualityScaler initialized (targetFPS=%.0f, scale=[%.2f, %.2f])", m_thresholds.targetFPS,
                       m_thresholds.minScale, m_thresholds.maxScale);
    }

    void DynamicQualityScaler::RecordFrameTime(float deltaTimeSec)
    {
        if (!m_initialized || deltaTimeSec <= 0.0f)
            return;

        // Clamp to reasonable range to avoid outliers (e.g., breakpoints, alt-tab)
        deltaTimeSec = std::clamp(deltaTimeSec, 0.0001f, 1.0f);

        m_frameTimes[m_writeIndex] = deltaTimeSec;
        m_writeIndex = (m_writeIndex + 1) % DQS_WINDOW_SIZE;
        m_sampleCount = std::min(m_sampleCount + 1, DQS_WINDOW_SIZE);
        m_timeSinceLastAdjust += deltaTimeSec;

        // Need at least 8 samples before adjusting to avoid reacting to noise
        if (m_sampleCount < 8)
            return;

        // Respect cooldown between adjustments
        if (m_timeSinceLastAdjust < m_thresholds.adjustCooldownSec)
            return;

        float avgFPS = GetAverageFPS();
        float targetFPS = m_thresholds.targetFPS;
        float dropThreshold = targetFPS * (1.0f - m_thresholds.dropThresholdPercent / 100.0f);
        float headroomThreshold = targetFPS * (1.0f + m_thresholds.headroomPercent / 100.0f);

        if (avgFPS < dropThreshold)
        {
            // Below target: reduce resolution
            float prevScale = m_currentScale;
            m_currentScale -= m_thresholds.scaleStepDown;
            m_currentScale = std::max(m_currentScale, m_thresholds.minScale);
            m_timeSinceLastAdjust = 0.0f;
            SPARK_LOG_DEBUG(Spark::LogCategory::Graphics,
                            "DynamicQualityScaler: scale down %.2f -> %.2f (avgFPS=%.1f, target=%.0f)", prevScale,
                            m_currentScale, avgFPS, targetFPS);
        }
        else if (avgFPS > headroomThreshold)
        {
            // Above target with headroom: increase resolution
            float prevScale = m_currentScale;
            m_currentScale += m_thresholds.scaleStepUp;
            m_currentScale = std::min(m_currentScale, m_thresholds.maxScale);
            m_timeSinceLastAdjust = 0.0f;
            SPARK_LOG_DEBUG(Spark::LogCategory::Graphics,
                            "DynamicQualityScaler: scale up %.2f -> %.2f (avgFPS=%.1f, target=%.0f)", prevScale,
                            m_currentScale, avgFPS, targetFPS);
        }
    }

    float DynamicQualityScaler::GetCurrentResolutionScale() const
    {
        return m_currentScale;
    }

    UpscalingQuality DynamicQualityScaler::GetRecommendedQuality() const
    {
        // Map the continuous scale to the nearest UpscalingQuality preset
        if (m_currentScale >= 0.90f)
            return UpscalingQuality::Native;
        if (m_currentScale >= 0.72f)
            return UpscalingQuality::UltraQuality;
        if (m_currentScale >= 0.62f)
            return UpscalingQuality::Quality;
        if (m_currentScale >= 0.54f)
            return UpscalingQuality::Balanced;
        if (m_currentScale >= 0.42f)
            return UpscalingQuality::Performance;

        return UpscalingQuality::UltraPerformance;
    }

    float DynamicQualityScaler::GetAverageFPS() const
    {
        float avgFrameTime = GetAverageFrameTime();
        if (avgFrameTime <= 0.0f)
            return 0.0f;

        return 1.0f / avgFrameTime;
    }

    float DynamicQualityScaler::GetAverageFrameTime() const
    {
        if (m_sampleCount <= 0)
            return 0.0f;

        float sum = 0.0f;
        for (int i = 0; i < m_sampleCount; ++i)
        {
            sum += m_frameTimes[i];
        }

        return sum / static_cast<float>(m_sampleCount);
    }

    void DynamicQualityScaler::Reset()
    {
        m_frameTimes.fill(0.0f);
        m_writeIndex = 0;
        m_sampleCount = 0;
        m_timeSinceLastAdjust = 0.0f;

        if (m_initialized)
        {
            m_currentScale = m_thresholds.maxScale;
        }
    }

    bool DynamicQualityScaler::HasValidData() const
    {
        return m_initialized && m_sampleCount >= 8;
    }

    const DynamicQualityThresholds& DynamicQualityScaler::GetThresholds() const
    {
        return m_thresholds;
    }

    void DynamicQualityScaler::GetRenderDimensions(uint32_t nativeWidth, uint32_t nativeHeight, uint32_t& outWidth,
                                                   uint32_t& outHeight) const
    {
        outWidth = std::max(1u, static_cast<uint32_t>(nativeWidth * m_currentScale));
        outHeight = std::max(1u, static_cast<uint32_t>(nativeHeight * m_currentScale));
    }

} // namespace Spark::Graphics
