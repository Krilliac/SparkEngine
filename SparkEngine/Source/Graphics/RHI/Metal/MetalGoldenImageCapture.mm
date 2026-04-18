/**
 * @file MetalGoldenImageCapture.mm
 * @brief Implementation of the `MetalGoldenImageCapture` bridge.
 *
 * Just delegates to `ReadbackTextureRGBA8`. The reason the class exists
 * (instead of the tests calling the readback helper directly) is so
 * `GoldenImageTestRunner` — which is plain C++ — can own a capture
 * without ever seeing an Objective-C type.
 */

#include "MetalGoldenImageCapture.h"

#ifdef SPARK_PLATFORM_MACOS

#include "MetalTextureReadback.h"

namespace Spark::RHI::Metal
{
    std::vector<uint8_t> MetalGoldenImageCapture::CaptureFramebuffer(uint32_t /*width*/, uint32_t /*height*/)
    {
        uint32_t actualW = 0;
        uint32_t actualH = 0;
        return ReadbackTextureRGBA8(m_texture, actualW, actualH);
    }
} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
