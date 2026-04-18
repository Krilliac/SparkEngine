/**
 * @file MetalTextureReadback.mm
 * @brief Implementation of synchronous MTLTexture → RGBA8 readback.
 *
 * Allocates a transient MTLBuffer with `MTLResourceStorageModeShared`
 * so the CPU can read directly after the blit completes. Commits a
 * single command buffer and waits for completion — callers doing this
 * at >1 Hz should batch or switch to the async GoldenImageTestRunner
 * path instead.
 */

#include "MetalTextureReadback.h"

#ifdef SPARK_PLATFORM_MACOS

#import <Metal/Metal.h>

namespace Spark::RHI::Metal
{
    std::vector<uint8_t> ReadbackTextureRGBA8(void* mtlTexture, uint32_t& width, uint32_t& height)
    {
        width = 0;
        height = 0;
        if (!mtlTexture)
            return {};

        id<MTLTexture> tex = (__bridge id<MTLTexture>)mtlTexture;
        if (!tex)
            return {};

        // Supported formats. Anything else returns empty — the caller
        // is responsible for converting via a shader pass before
        // handing the output to us. Avoiding in-helper format coercion
        // keeps the contract predictable for golden-image tests.
        const MTLPixelFormat fmt = tex.pixelFormat;
        if (fmt != MTLPixelFormatRGBA8Unorm && fmt != MTLPixelFormatBGRA8Unorm)
            return {};

        width = static_cast<uint32_t>(tex.width);
        height = static_cast<uint32_t>(tex.height);
        const std::size_t bytesPerPixel = 4;
        const std::size_t bytesPerRow = bytesPerPixel * width;
        const std::size_t totalBytes = bytesPerRow * height;

        if (totalBytes == 0 || totalBytes > kReadbackMaxBytes)
            return {};

        id<MTLDevice> device = tex.device;
        if (!device)
            return {};

        id<MTLBuffer> staging = [device newBufferWithLength:totalBytes options:MTLResourceStorageModeShared];
        if (!staging)
            return {};

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
        [blit copyFromTexture:tex
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(width, height, 1)
                             toBuffer:staging
                    destinationOffset:0
               destinationBytesPerRow:bytesPerRow
             destinationBytesPerImage:totalBytes];
        [blit endEncoding];
        [cmdBuf commit];
        [cmdBuf waitUntilCompleted];

        std::vector<uint8_t> out(totalBytes);
        std::memcpy(out.data(), [staging contents], totalBytes);

        // Engine convention is RGBA8; swap red/blue on BGRA sources.
        if (fmt == MTLPixelFormatBGRA8Unorm)
        {
            for (std::size_t i = 0; i < totalBytes; i += 4)
                std::swap(out[i + 0], out[i + 2]);
        }
        return out;
    }
} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
