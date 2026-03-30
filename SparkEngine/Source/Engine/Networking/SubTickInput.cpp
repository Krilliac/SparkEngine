/**
 * @file SubTickInput.cpp
 * @brief Implementation of sub-tick input recording and processing
 */

#include "SubTickInput.h"

#include "../../Utils/LogMacros.h"
#include <algorithm>
#include <cmath>

namespace Spark::Net
{

    // ========================================================================
    // RecordInput
    // ========================================================================

    void SubTickInputBuffer::RecordInput(const SubTickInputSample& sample)
    {
        SubTickInputSample recorded = sample;

        // Assign sequence number if not already set
        if (recorded.sequenceNumber == 0)
        {
            recorded.sequenceNumber = ++m_nextSequence;
        }
        SPARK_LOG_TRACE(Spark::LogCategory::Network, "RecordInput: seq=%u tickFraction=%.4f bufferSize=%zu",
                        recorded.sequenceNumber, recorded.tickFraction, m_samples.size() + 1);

        // Clamp tickFraction to valid range [0, 1)
        recorded.tickFraction = std::clamp(recorded.tickFraction, 0.0f, 0.9999f);

        // Insert sorted by tickFraction to maintain temporal order within the tick.
        // Most inputs arrive in order, so search from the back for efficiency.
        auto insertPos = m_samples.end();
        for (auto it = m_samples.rbegin(); it != m_samples.rend(); ++it)
        {
            if (it->tickFraction <= recorded.tickFraction)
            {
                insertPos = it.base();
                break;
            }
        }

        if (insertPos == m_samples.end() && !m_samples.empty() && m_samples.back().tickFraction > recorded.tickFraction)
        {
            // Insert at the beginning
            insertPos = m_samples.begin();
        }

        m_samples.insert(insertPos, std::move(recorded));
    }

    // ========================================================================
    // Clear
    // ========================================================================

    void SubTickInputBuffer::Clear()
    {
        if (!m_samples.empty())
        {
            SPARK_LOG_TRACE(Spark::LogCategory::Network, "SubTickInputBuffer cleared (%zu samples)", m_samples.size());
        }
        m_samples.clear();
    }

    // ========================================================================
    // GetSamples
    // ========================================================================

    const std::vector<SubTickInputSample>& SubTickInputBuffer::GetSamples() const
    {
        return m_samples;
    }

    // ========================================================================
    // ProcessInputsForTick
    // ========================================================================

    void SubTickInputBuffer::ProcessInputsForTick(
        uint32_t /*tickNumber*/, float tickDuration,
        std::function<void(const SubTickInputSample&, float exactTime)> handler) const
    {
        if (!handler || m_samples.empty())
        {
            return;
        }

        SPARK_LOG_TRACE(Spark::LogCategory::Network, "ProcessInputsForTick: %zu samples, tickDuration=%.4f",
                        m_samples.size(), tickDuration);

        // Ensure tick duration is valid
        float safeDuration = std::max(tickDuration, 0.001f);

        // Process each sample in temporal order (already sorted by tickFraction).
        // Compute the exact time within the tick for each sample.
        for (const auto& sample : m_samples)
        {
            float exactTime = sample.tickFraction * safeDuration;
            handler(sample, exactTime);
        }
    }

    // ========================================================================
    // GetSampleCount
    // ========================================================================

    size_t SubTickInputBuffer::GetSampleCount() const
    {
        return m_samples.size();
    }

} // namespace Spark::Net
