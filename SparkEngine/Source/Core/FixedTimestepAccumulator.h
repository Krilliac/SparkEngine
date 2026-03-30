#pragma once

/**
 * @file FixedTimestepAccumulator.h
 * @brief Centralized fixed-timestep accumulator for the main loop
 *
 * Provides a singleton accumulator that tracks elapsed time and produces
 * a deterministic number of fixed-timestep ticks per frame.  Systems can
 * query GetFixedStepCount() to know how many FixedUpdate ticks to run,
 * and GetInterpolationAlpha() to blend between physics states for smooth
 * rendering.
 */

#include "Platform.h"

#include <cstdint>
#include <string>

namespace Spark
{

    /// @brief Update stage a system registers for
    enum class UpdateStage : uint8_t
    {
        FixedUpdate, ///< Runs at a fixed rate (e.g. 60 Hz)
        Update,      ///< Runs once per frame with variable dt
        LateUpdate   ///< Runs once per frame, after Update
    };

    /// @brief Singleton fixed-timestep accumulator for the main loop
    class FixedTimestepAccumulator
    {
      public:
        /// @brief Get the singleton instance
        [[deprecated("Use EngineContext::Get()->GetSystem<FixedTimestepAccumulator>() instead")]]
        static FixedTimestepAccumulator& GetInstance();

        /// @brief Initialize with the desired fixed timestep
        /// @param fixedDt Fixed timestep in seconds (default 1/60)
        /// @return true on success
        bool Initialize(float fixedDt = 1.0f / 60.0f);

        /// @brief Change the fixed timestep at runtime
        void SetFixedTimestep(float dt);

        /// @brief Get the current fixed timestep
        float GetFixedTimestep() const;

        /// @brief Advance the accumulator by the frame's delta time
        /// @param frameDt Elapsed time since last frame in seconds
        void Advance(float frameDt);

        /// @brief Number of fixed-timestep ticks that should run this frame
        uint32_t GetFixedStepCount() const;

        /// @brief Remaining accumulated time (for interpolation)
        float GetAccumulator() const;

        /// @brief Interpolation alpha (accumulator / fixedDt), in the range [0..1)
        float GetInterpolationAlpha() const;

        /// @brief Console command: return a human-readable status string
        std::string Console_GetStatus() const;

        /// @brief Release resources
        void Shutdown();

      private:
        FixedTimestepAccumulator() = default;
        ~FixedTimestepAccumulator() = default;

        FixedTimestepAccumulator(const FixedTimestepAccumulator&) = delete;
        FixedTimestepAccumulator& operator=(const FixedTimestepAccumulator&) = delete;

        float m_fixedDt = 1.0f / 60.0f;
        float m_accumulator = 0.0f;
        uint32_t m_fixedStepCount = 0;

        /// @brief Cap to prevent spiral-of-death when frameDt is very large
        static constexpr float MAX_FRAME_DT = 0.25f;

        /// @brief Maximum fixed steps per frame to prevent runaway ticking
        static constexpr uint32_t MAX_STEPS_PER_FRAME = 8;

        bool m_initialized = false;
    };

} // namespace Spark
