/**
 * @file DeltaSmoother.h
 * @brief Frame time smoothing to reduce jitter in game systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Averages recent frame times to produce a stable delta for systems
 * sensitive to per-frame timing spikes (camera, animation, physics
 * interpolation). Uses a ring buffer of the last N samples.
 *
 * ## Usage
 * @code
 *   Spark::DeltaSmoother smoother(10); // average over 10 frames
 *
 *   // Each frame
 *   float rawDt = timer.GetDeltaTime();
 *   float smoothDt = smoother.Smooth(rawDt);
 *   camera.Update(smoothDt);  // no micro-stutters
 *
 *   // Query statistics
 *   float avg = smoother.GetAverage();
 *   float fps = smoother.GetSmoothedFPS();
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

namespace Spark
{

    /**
     * @class DeltaSmoother
     * @brief Running average filter for frame delta times.
     *
     * Maintains a ring buffer of the most recent delta time samples
     * and computes their average to smooth out frame-to-frame jitter.
     */
    class DeltaSmoother
    {
      public:
        /**
         * @brief Construct a smoother with the given sample window.
         * @param windowSize  Number of frames to average over. Minimum 1.
         */
        explicit DeltaSmoother(size_t windowSize = 10) : m_samples(windowSize > 0 ? windowSize : 1, 0.0f), m_index(0) {}

        /**
         * @brief Add a new delta time sample and return the smoothed value.
         * @param deltaTime  Raw frame delta time in seconds.
         * @return           Smoothed delta time (average of recent samples).
         */
        float Smooth(float deltaTime)
        {
            // Clamp absurd values (e.g. after breakpoints or alt-tab)
            constexpr float MAX_DT = 0.25f; // 4 FPS minimum
            deltaTime = (std::min)(deltaTime, MAX_DT);

            m_samples[m_index] = deltaTime;
            m_index = (m_index + 1) % m_samples.size();

            return GetAverage();
        }

        /**
         * @brief Get the current smoothed average without adding a sample.
         * @return  Average of all samples in the window.
         */
        float GetAverage() const
        {
            float sum = std::accumulate(m_samples.begin(), m_samples.end(), 0.0f);
            return sum / static_cast<float>(m_samples.size());
        }

        /**
         * @brief Get smoothed FPS based on the current average delta.
         * @return  Frames per second estimate, or 0 if average is zero.
         */
        float GetSmoothedFPS() const
        {
            float avg = GetAverage();
            return (avg > 0.0f) ? (1.0f / avg) : 0.0f;
        }

        /**
         * @brief Get the min and max delta times in the current window.
         * @return  Pair of (min, max) delta times.
         */
        std::pair<float, float> GetRange() const
        {
            auto [minIt, maxIt] = std::minmax_element(m_samples.begin(), m_samples.end());
            return {*minIt, *maxIt};
        }

        /**
         * @brief Reset all samples to zero.
         */
        void Reset() { std::fill(m_samples.begin(), m_samples.end(), 0.0f); }

        /**
         * @brief Get the window size.
         * @return  Number of samples in the averaging window.
         */
        size_t GetWindowSize() const { return m_samples.size(); }

      private:
        std::vector<float> m_samples;
        size_t m_index;
    };

} // namespace Spark
