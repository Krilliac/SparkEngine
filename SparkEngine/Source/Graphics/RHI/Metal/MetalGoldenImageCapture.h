/**
 * @file MetalGoldenImageCapture.h
 * @brief `IGoldenImageCapture` that reads back a Metal texture.
 *
 * Thin wrapper around `MetalTextureReadback::ReadbackTextureRGBA8` so RT
 * dispatches (and anything else that writes to an `MTLTexture`) can flow
 * into the existing `GoldenImageTestRunner` pixel-diff pipeline without
 * the runner ever seeing Objective-C types.
 *
 * Usage:
 *
 * @code
 *   id<MTLTexture> hdrOutput = hybridRT.GetOutputTexture();
 *   auto capture = std::make_unique<Spark::RHI::Metal::MetalGoldenImageCapture>(
 *       (__bridge void*)hdrOutput);
 *   Spark::GoldenImageTestRunner::GetInstance().SetCapture(std::move(capture));
 *
 *   // Later:
 *   auto result = Spark::GoldenImageTestRunner::GetInstance()
 *                     .CompareWithGolden("HybridRT_SimpleScene");
 * @endcode
 *
 * Design notes:
 *   - Source texture pointer is stored opaque (`void*`) — same contract
 *     as `MetalTextureReadback`. Plain C++ consumers that only see an
 *     `MTLTexture` via `void*` accessor (e.g. an RHI-side `GetNative()`)
 *     can construct and drop this capture without dragging Objective-C
 *     into their translation unit.
 *   - The capture does **not** retain the texture. The owning RT system
 *     must keep the texture alive for the lifetime of the capture — same
 *     lifetime contract as `RHIBridge::RegisterRenderTarget`.
 *   - `CaptureFramebuffer(width, height)` ignores its arguments when
 *     the underlying texture's intrinsic size differs. The readback helper
 *     returns the texture's actual dimensions; the test runner only needs
 *     a width/height for its file-naming, it doesn't rescale.
 *   - Off-macOS this header is an empty namespace — the class only
 *     materializes inside `SPARK_PLATFORM_MACOS` so it never pollutes
 *     Linux/Windows builds even if accidentally included.
 */

#pragma once

#include "../../../Core/Platform.h"

#ifdef SPARK_PLATFORM_MACOS

#include "../../../Utils/GoldenImageTest.h"

#include <cstdint>
#include <vector>

namespace Spark::RHI::Metal
{
    /**
     * @brief `IGoldenImageCapture` that pulls pixels from an `MTLTexture`.
     *
     * Readback is synchronous — same behavior as the underlying
     * `ReadbackTextureRGBA8`. One command buffer submit + waitUntilCompleted
     * per capture. Callers running at interactive rates should either cache
     * the capture instance or move off the golden-image path entirely.
     */
    class MetalGoldenImageCapture final : public Spark::IGoldenImageCapture
    {
      public:
        /**
         * @brief Construct with an opaque `id<MTLTexture>` pointer.
         * @param mtlTexture Non-owning reference. Must outlive the capture.
         */
        explicit MetalGoldenImageCapture(void* mtlTexture) noexcept : m_texture(mtlTexture) {}

        /** @brief Swap the source texture (e.g. after a swapchain rotation). */
        void SetTexture(void* mtlTexture) noexcept { m_texture = mtlTexture; }

        /** @brief Current source texture pointer, may be null. */
        void* GetTexture() const noexcept { return m_texture; }

        /**
         * @brief Read the source texture back into an RGBA8 byte buffer.
         *
         * The `width` / `height` arguments are informational only — the
         * readback reports the texture's true dimensions through the helper.
         * When the source texture is null or its format isn't convertible to
         * RGBA8, returns an empty vector (contract matches `IGoldenImageCapture`).
         */
        [[nodiscard]] std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) override;

      private:
        void* m_texture = nullptr;
    };

} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
