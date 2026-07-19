/**
 * @file UpscalingSystemWindowsInternal.h
 * @brief Shared Windows-only UpscalingUtils declarations for the UpscalingSystemWindows*.cpp split parts
 */
#pragma once
#include "UpscalingSystem.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include <d3d11.h>

#include <cstdint>
#include <utility>

namespace Spark
{
    namespace Graphics
    {
        namespace UpscalingUtils
        {
            FSR1EASUConstants CalculateEASUConstants(uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth,
                                                     uint32_t outputHeight);

            FSR1RCASConstants CalculateRCASConstants(float sharpness);

            std::pair<uint32_t, uint32_t> CalculateDispatchGroups(uint32_t width, uint32_t height);
        } // namespace UpscalingUtils
    } // namespace Graphics
} // namespace Spark

#endif // SPARK_PLATFORM_WINDOWS
