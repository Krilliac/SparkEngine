/**
 * @file MetalInterop.h
 * @brief C++-safe entry points for Objective-C++ Metal implementation details.
 */

#pragma once

#ifdef SPARK_METAL_SUPPORT

#include "../RHIDevice.h"

#include <memory>

namespace Spark::RHI::Metal
{
    class MetalRayTracingSystem;

    std::unique_ptr<IRHIDevice> CreateDevice();
    bool InitializeRayTracing(MetalRayTracingSystem& rayTracing, IRHIDevice& device);
} // namespace Spark::RHI::Metal

#endif // SPARK_METAL_SUPPORT
