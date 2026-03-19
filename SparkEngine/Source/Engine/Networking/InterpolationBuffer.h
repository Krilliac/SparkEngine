/**
 * @file InterpolationBuffer.h
 * @brief Generic ring-buffer for timestamped network sample interpolation
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a templated circular buffer that stores timestamped network samples
 * and interpolates between them for smooth client-side rendering. Unlike the
 * entity-specific NetworkInterpolationBuffer, this is a general-purpose
 * template suitable for any interpolatable value type (floats, vectors, etc.).
 *
 * Requires a Lerp() overload for the template parameter type.
 */

#pragma once
#include "../../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace Spark::Net
{

    // ========================================================================
    // Lerp Overloads
    // ========================================================================

    /**
     * @brief Linear interpolation for scalar floats.
     * @param a  Start value
     * @param b  End value
     * @param t  Interpolation factor [0, 1]
     * @return Interpolated value
     */
    inline float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    /**
     * @brief Component-wise linear interpolation for 3D vectors.
     * @param a  Start vector
     * @param b  End vector
     * @param t  Interpolation factor [0, 1]
     * @return Interpolated vector
     */
    inline XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t)
    {
        return XMFLOAT3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    /**
     * @brief Component-wise linear interpolation for 4D vectors / quaternions.
     *
     * Note: for true quaternion interpolation, use Slerp. This provides a fast
     * approximation suitable for small angular differences between samples.
     *
     * @param a  Start vector
     * @param b  End vector
     * @param t  Interpolation factor [0, 1]
     * @return Interpolated vector
     */
    inline XMFLOAT4 Lerp(const XMFLOAT4& a, const XMFLOAT4& b, float t)
    {
        return XMFLOAT4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }

    // ========================================================================
    // InterpolationBuffer
    // ========================================================================

    /**
     * @brief Ring-buffer of timestamped samples with interpolation support.
     *
     * Stores up to BUFFER_SIZE samples in chronological order. When Evaluate()
     * is called with a render timestamp, the buffer finds the two bracketing
     * samples and interpolates between them using Lerp().
     *
     * @tparam T  Value type. Must have a corresponding Lerp(T, T, float) overload.
     */
    template <typename T> class InterpolationBuffer
    {
      public:
        static constexpr size_t BUFFER_SIZE = 16;

        /**
         * @brief Add a new timestamped sample to the buffer.
         *
         * Samples should be added in chronological order. If the buffer is full,
         * the oldest sample is overwritten.
         *
         * @param value      The sample value
         * @param timestamp  Server time for this sample (seconds)
         */
        void AddSample(const T& value, float timestamp)
        {
            m_samples[m_head] = Sample{value, timestamp};
            m_head = (m_head + 1) % BUFFER_SIZE;

            if (m_count < BUFFER_SIZE)
            {
                ++m_count;
            }
        }

        /**
         * @brief Evaluate the interpolated value at the given render time.
         *
         * Finds the two samples that bracket renderTime and linearly interpolates
         * between them. Edge cases:
         * - No samples: returns default-constructed T
         * - One sample: returns that sample's value
         * - renderTime before all samples: returns oldest value
         * - renderTime after all samples: returns newest value (no extrapolation)
         *
         * @param renderTime  The time to evaluate at (seconds)
         * @return Interpolated value
         */
        T Evaluate(float renderTime) const
        {
            if (m_count == 0)
            {
                return T{};
            }

            if (m_count == 1)
            {
                return GetSample(0).value;
            }

            auto [before, after] = FindBracketingSamples(renderTime);

            if (before == nullptr || after == nullptr)
            {
                // renderTime is outside the sample range — clamp to nearest
                if (before == nullptr)
                {
                    return GetSample(0).value;
                }
                return GetSample(m_count - 1).value;
            }

            // Compute interpolation factor
            float span = after->timestamp - before->timestamp;
            if (span <= 0.0f)
            {
                return before->value;
            }

            float t = (renderTime - before->timestamp) / span;
            t = std::clamp(t, 0.0f, 1.0f);

            return Lerp(before->value, after->value, t);
        }

        /// @brief Remove all samples from the buffer
        void Clear()
        {
            m_head = 0;
            m_count = 0;
        }

        /// @brief Check if the buffer contains any samples
        bool HasSamples() const { return m_count > 0; }

        /// @brief Get the number of samples currently stored
        size_t GetSampleCount() const { return m_count; }

        /// @brief Get the timestamp of the newest sample, or 0 if empty
        float GetLatestTimestamp() const
        {
            if (m_count == 0)
            {
                return 0.0f;
            }
            return GetSample(m_count - 1).timestamp;
        }

        /// @brief Get the timestamp of the oldest sample, or 0 if empty
        float GetOldestTimestamp() const
        {
            if (m_count == 0)
            {
                return 0.0f;
            }
            return GetSample(0).timestamp;
        }

      private:
        struct Sample
        {
            T value{};
            float timestamp = 0.0f;
        };

        std::array<Sample, BUFFER_SIZE> m_samples{};
        size_t m_head = 0;
        size_t m_count = 0;

        /// @brief Get a sample by logical index (0 = oldest)
        const Sample& GetSample(size_t logicalIndex) const
        {
            size_t startIndex = (m_head + BUFFER_SIZE - m_count) % BUFFER_SIZE;
            return m_samples[(startIndex + logicalIndex) % BUFFER_SIZE];
        }

        /**
         * @brief Find the two samples that bracket the given time.
         *
         * @param time  Target time to bracket
         * @return Pair of pointers: (before, after). Either may be null if time
         *         is outside the sample range.
         */
        std::pair<const Sample*, const Sample*> FindBracketingSamples(float time) const
        {
            // Samples are stored in chronological order (oldest at logical index 0)
            // Find the first sample with timestamp > time
            for (size_t i = 0; i < m_count; ++i)
            {
                const auto& sample = GetSample(i);
                if (sample.timestamp > time)
                {
                    if (i == 0)
                    {
                        // time is before all samples
                        return {nullptr, &sample};
                    }
                    return {&GetSample(i - 1), &sample};
                }
            }

            // time is after all samples
            return {&GetSample(m_count - 1), nullptr};
        }
    };

} // namespace Spark::Net
