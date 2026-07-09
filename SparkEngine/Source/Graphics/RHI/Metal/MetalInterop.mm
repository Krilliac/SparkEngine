/**
 * @file MetalInterop.mm
 * @brief Objective-C++ bridge for Metal objects used by C++ engine code.
 */

#include "MetalInterop.h"

#ifdef SPARK_METAL_SUPPORT

#include "MetalDevice.h"
#include "MetalRayTracing.h"

namespace Spark::RHI::Metal
{
    std::unique_ptr<IRHIDevice> CreateDevice()
    {
        return std::make_unique<MetalDevice>();
    }

    bool InitializeRayTracing(MetalRayTracingSystem& rayTracing, IRHIDevice& device)
    {
        auto* metalDevice = dynamic_cast<MetalDevice*>(&device);
        if (metalDevice == nullptr)
        {
            return false;
        }

        (void)rayTracing.Initialize(metalDevice);
        return true;
    }
} // namespace Spark::RHI::Metal

#endif // SPARK_METAL_SUPPORT
