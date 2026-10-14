/**
 * @file DpiSdl.cpp
 * @brief SDL2 DPI helpers: drawable-to-window ratio times display DPI / 96 (ratio only on macOS).
 */

#include "SparkUI/Dpi.h"

#include <cmath>

#include <SDL.h>

namespace Spark::Ui
{
    void EnableProcessDpiAwareness() noexcept {}

    float WindowDpiScale(void* nativeWindow) noexcept
    {
        auto* window = static_cast<SDL_Window*>(nativeWindow);
        if (window == nullptr)
        {
            return 1.0f;
        }
#ifdef __APPLE__
        // macOS reports logical points; the SDL2 ImGui backend already applies the Retina backing ratio through
        // DisplayFramebufferScale, so multiplying it in here would draw the UI at double size (spec section 2.8).
        const float scale = 1.0f;
#else
        int windowWidth = 0;
        int windowHeight = 0;
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        float scale = windowWidth > 0 && drawableWidth > 0
                          ? static_cast<float>(drawableWidth) / static_cast<float>(windowWidth)
                          : 1.0f;
        float diagonalDpi = 0.0f;
        float horizontalDpi = 0.0f;
        float verticalDpi = 0.0f;
        const int display = SDL_GetWindowDisplayIndex(window);
        if (display >= 0 && SDL_GetDisplayDPI(display, &diagonalDpi, &horizontalDpi, &verticalDpi) == 0 &&
            horizontalDpi > 0.0f)
        {
            scale *= horizontalDpi / 96.0f;
        }
#endif
        return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }
} // namespace Spark::Ui
