/**
 * @file GraphicsEngineRHI.h
 * @brief Internal header for Linux RHI state shared across GraphicsEngine translation units
 *
 * This header is only included on non-Windows platforms. It provides the shared
 * LinuxRHIState singleton used by all GraphicsEngine .cpp files that were split
 * from the original monolithic GraphicsEngine.cpp.
 */
#pragma once

#ifndef SPARK_PLATFORM_WINDOWS

#include "RHI/RHI.h"
#include <chrono>
#include <cstdint>

namespace Spark::Graphics::Detail
{

    struct LinuxRHIState
    {
        Spark::RHI::RHIBridge bridge;
        bool initialized = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::chrono::high_resolution_clock::time_point frameStart;
        uint32_t frameCount = 0;
        float accumulatedTime = 0.0f;
        uint32_t measuredFps = 0;
    };

    inline LinuxRHIState& GetRHI()
    {
        static LinuxRHIState s;
        return s;
    }

} // namespace Spark::Graphics::Detail

#endif // !SPARK_PLATFORM_WINDOWS
