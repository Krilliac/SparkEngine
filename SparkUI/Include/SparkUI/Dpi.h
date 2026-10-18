/**
 * @file Dpi.h
 * @brief Platform DPI helpers for Spark UI hosts (Win32 editor/launcher/installer, SDL builds).
 *
 * Thread affinity: call from the thread that owns the native window (the UI thread).
 * Ownership: no state is kept; the native window stays owned by the caller.
 */

#pragma once

#include <cmath>

namespace Spark::Ui
{
    /// @brief Make the process per-monitor DPI aware (Win32); a no-op on other platforms.
    /// @details Call before the first window class is registered.
    void EnableProcessDpiAwareness() noexcept;

    /// @brief DPI scale (1.0 = 96 DPI) of a native window.
    /// @param nativeWindow An HWND on Windows, an SDL_Window* elsewhere; nullptr yields 1.0.
    /// @details Win32 asks the ImGui backend for the monitor DPI. SDL builds apply SdlDpiScale()
    ///          to the drawable/window ratio and the display DPI (macOS always answers 1.0).
    float WindowDpiScale(void* nativeWindow) noexcept;

    /// @brief The SDL DPI rule (spec section 2.8 as amended): the drawable/window ratio OR the
    ///        display DPI / 96, never both.
    /// @details A drawable larger than the window means the SDL2 ImGui backend already renders at
    ///          that ratio through io.DisplayFramebufferScale, so the UI must not scale again and
    ///          the answer is 1.0. Otherwise the window is unscaled and the display DPI / 96 is the
    ///          scale. Non-finite or non-positive inputs count as "no information".
    /// @param framebufferRatio Drawable width divided by window width.
    /// @param horizontalDpi Horizontal display DPI from SDL_GetDisplayDPI, or 0 when unknown.
    /// @return The UI DPI scale; always finite and positive.
    inline float SdlDpiScale(float framebufferRatio, float horizontalDpi) noexcept
    {
        constexpr float kRatioTolerance = 1.01f;
        if (std::isfinite(framebufferRatio) && framebufferRatio > kRatioTolerance)
        {
            return 1.0f;
        }
        if (std::isfinite(horizontalDpi) && horizontalDpi > 0.0f)
        {
            return horizontalDpi / 96.0f;
        }
        return 1.0f;
    }
} // namespace Spark::Ui
