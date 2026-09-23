/**
 * @file GraphicsBenchmarkStats.h
 * @brief Bounded statistics for real renderer benchmark samples.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace Spark::Graphics
{

/** Collects a bounded sequence of finite millisecond samples. */
class GraphicsBenchmarkStats
{
  public:
    // The benchmark accepts up to 300 seconds. 90,000 samples covers that
    // duration at 300 Hz while remaining a small bounded diagnostic buffer.
    static constexpr std::size_t kDefaultCapacity = 90000;

    explicit GraphicsBenchmarkStats(std::size_t capacity = kDefaultCapacity)
        : m_capacity(std::min(capacity, kDefaultCapacity))
    {}

    void Reset()
    {
        m_samples.clear();
        m_truncated = false;
    }

    void Add(double sampleMs)
    {
        if (!std::isfinite(sampleMs) || sampleMs < 0.0)
            return;
        if (m_samples.size() >= m_capacity)
        {
            m_truncated = true;
            return;
        }
        if (m_samples.empty())
            m_samples.reserve(m_capacity);
        m_samples.push_back(sampleMs);
    }

    std::size_t Count() const { return m_samples.size(); }
    std::size_t Capacity() const { return m_capacity; }
    bool IsTruncated() const { return m_truncated; }

    double Mean() const
    {
        if (m_samples.empty())
            return std::numeric_limits<double>::quiet_NaN();
        double total = 0.0;
        for (const double sample : m_samples)
            total += sample;
        return total / static_cast<double>(m_samples.size());
    }

    double Min() const
    {
        return m_samples.empty() ? std::numeric_limits<double>::quiet_NaN()
                                  : *std::min_element(m_samples.begin(), m_samples.end());
    }

    double Max() const
    {
        return m_samples.empty() ? std::numeric_limits<double>::quiet_NaN()
                                  : *std::max_element(m_samples.begin(), m_samples.end());
    }

    /** Linear-interpolated percentile in [0, 1], or NaN when empty. */
    double Percentile(double percentile) const
    {
        if (m_samples.empty() || !std::isfinite(percentile))
            return std::numeric_limits<double>::quiet_NaN();
        percentile = std::clamp(percentile, 0.0, 1.0);
        std::vector<double> sorted = m_samples;
        std::sort(sorted.begin(), sorted.end());
        const double position = percentile * static_cast<double>(sorted.size() - 1);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, sorted.size() - 1);
        const double fraction = position - static_cast<double>(lower);
        return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
    }

  private:
    std::size_t m_capacity;
    std::vector<double> m_samples;
    bool m_truncated = false;
};

} // namespace Spark::Graphics
