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
#include <memory>

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

        // GBuffer + HDR + Depth textures owned by the Linux/macOS rendering
        // layer. Created after bridge init, registered into the bridge's
        // render-target registry so cross-platform code (HybridRT dispatch,
        // golden-image capture, debug viewers) can look them up by slot.
        //
        // Layout matches Windows: GBuffer[0]=Albedo (RGBA8),
        // [1]=Normals (R16G16B16A16F), [2]=Material (RGBA8), [3]=Motion (RG16F).
        // Depth is D24_UNORM_S8_UINT, HDR is R16G16B16A16F with UAV so the
        // RT pass can write to it.
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferAlbedo;
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferNormals;
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferMaterial;
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferMotion;
        std::unique_ptr<Spark::RHI::IRHITexture> depthStencil;
        std::unique_ptr<Spark::RHI::IRHITexture> hdrLighting;
    };

    inline LinuxRHIState& GetRHI()
    {
        static LinuxRHIState s;
        return s;
    }

} // namespace Spark::Graphics::Detail

#endif // !SPARK_PLATFORM_WINDOWS
