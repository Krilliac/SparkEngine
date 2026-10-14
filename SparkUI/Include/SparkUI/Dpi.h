/**
 * @file Dpi.h
 * @brief Platform DPI helpers for Spark UI hosts (Win32 editor/launcher/installer, SDL builds).
 *
 * Thread affinity: call from the thread that owns the native window (the UI thread).
 * Ownership: no state is kept; the native window stays owned by the caller.
 */

#pragma once

namespace Spark::Ui
{
    /// @brief Make the process per-monitor DPI aware (Win32); a no-op on other platforms.
    /// @details Call before the first window class is registered.
    void EnableProcessDpiAwareness() noexcept;

    /// @brief DPI scale (1.0 = 96 DPI) of a native window.
    /// @param nativeWindow An HWND on Windows, an SDL_Window* elsewhere; nullptr yields 1.0.
    float WindowDpiScale(void* nativeWindow) noexcept;
} // namespace Spark::Ui
