/**
 * @file MetalTextureReadback.h
 * @brief CPU readback of an MTLTexture into an RGBA byte vector.
 *
 * Small helper used by Metal-side golden-image tests and capture
 * tools. Given an `id<MTLTexture>`, blits it into a staging buffer
 * with shared storage and returns the bytes as `std::vector<uint8_t>`
 * in tightly-packed RGBA order (4 bytes per pixel, row-major).
 *
 * Design notes:
 *   - Declared in a plain C++ header (no Objective-C types) so it can
 *     be included from `.cpp` translation units. The implementation
 *     lives in `.mm` where MTL types are visible.
 *   - The texture pointer is opaque `void*` here — callers in `.mm`
 *     land pass `(__bridge void*)mtlTexture`, plain C++ callers
 *     receive a `void*` from an accessor and pass it straight
 *     through without touching the ObjC runtime.
 *   - Readback is synchronous (commits + waits for the blit). Tests
 *     use this sparingly; production code should prefer the async
 *     capture path in `GoldenImageTestRunner`.
 *   - `kReadbackMaxBytes` caps the allocation defensively — a bad
 *     pointer shouldn't cause a multi-GB malloc. 64 MB covers
 *     4096×4096 RGBA8 with margin.
 */

#pragma once

#include "../../../Core/Platform.h"

#ifdef SPARK_PLATFORM_MACOS

#include <cstdint>
#include <vector>

namespace Spark::RHI::Metal
{
    constexpr std::size_t kReadbackMaxBytes = 64u * 1024u * 1024u;

    /**
     * @brief Copy `mtlTexture` to host memory as tightly-packed RGBA8.
     *
     * @param mtlTexture  Opaque `id<MTLTexture>` pointer. `nullptr` → empty.
     * @param width       [out] Texture width in pixels.
     * @param height      [out] Texture height in pixels.
     * @return RGBA8 bytes (`width * height * 4` length) or empty on
     *         failure (nil input, non-color format, oversize allocation).
     *         The first failure mode in CI runs is usually "texture
     *         format not convertible to RGBA8"; the function returns
     *         empty rather than attempting a silent format coercion.
     */
    std::vector<uint8_t> ReadbackTextureRGBA8(void* mtlTexture, uint32_t& width, uint32_t& height);

} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
