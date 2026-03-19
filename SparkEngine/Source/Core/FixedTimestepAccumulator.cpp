/**
 * @file FixedTimestepAccumulator.cpp
 * @brief Implementation of the fixed-timestep accumulator
 */

#include "FixedTimestepAccumulator.h"

#include <algorithm>
#include <format>

namespace Spark
{

    FixedTimestepAccumulator& FixedTimestepAccumulator::GetInstance()
    {
        static FixedTimestepAccumulator instance;
        return instance;
    }

    bool FixedTimestepAccumulator::Initialize(float fixedDt)
    {
        if (fixedDt <= 0.0f)
        {
            return false;
        }

        m_fixedDt = fixedDt;
        m_accumulator = 0.0f;
        m_fixedStepCount = 0;
        m_initialized = true;
        return true;
    }

    void FixedTimestepAccumulator::SetFixedTimestep(float dt)
    {
        if (dt > 0.0f)
        {
            m_fixedDt = dt;
        }
    }

    float FixedTimestepAccumulator::GetFixedTimestep() const
    {
        return m_fixedDt;
    }

    void FixedTimestepAccumulator::Advance(float frameDt)
    {
        // Clamp to prevent spiral-of-death on long frames (e.g. breakpoint, loading)
        float clampedDt = std::min(frameDt, MAX_FRAME_DT);
        m_accumulator += clampedDt;

        // Count how many fixed steps we can take
        m_fixedStepCount = 0;
        while (m_accumulator >= m_fixedDt && m_fixedStepCount < MAX_STEPS_PER_FRAME)
        {
            m_accumulator -= m_fixedDt;
            ++m_fixedStepCount;
        }

        // If we hit the step cap, drain the excess to avoid permanent lag
        if (m_fixedStepCount >= MAX_STEPS_PER_FRAME && m_accumulator >= m_fixedDt)
        {
            m_accumulator = std::fmod(m_accumulator, m_fixedDt);
        }
    }

    uint32_t FixedTimestepAccumulator::GetFixedStepCount() const
    {
        return m_fixedStepCount;
    }

    float FixedTimestepAccumulator::GetAccumulator() const
    {
        return m_accumulator;
    }

    float FixedTimestepAccumulator::GetInterpolationAlpha() const
    {
        if (m_fixedDt <= 0.0f)
        {
            return 0.0f;
        }
        return m_accumulator / m_fixedDt;
    }

    std::string FixedTimestepAccumulator::Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return "FixedTimestepAccumulator: not initialized";
        }

        return std::format("FixedTimestepAccumulator:\n"
                           "  Fixed dt:      {:.4f}s ({:.1f} Hz)\n"
                           "  Accumulator:   {:.4f}s\n"
                           "  Steps (last):  {}\n"
                           "  Interp alpha:  {:.3f}",
                           m_fixedDt, 1.0f / m_fixedDt, m_accumulator, m_fixedStepCount, GetInterpolationAlpha());
    }

    void FixedTimestepAccumulator::Shutdown()
    {
        m_accumulator = 0.0f;
        m_fixedStepCount = 0;
        m_initialized = false;
    }

} // namespace Spark
