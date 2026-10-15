/**
 * @file DpiWin32.cpp
 * @brief Win32 DPI helpers over the Dear ImGui Win32 backend.
 */

#include "SparkUI/Dpi.h"

#include <cmath>

#include <imgui.h>
#include <imgui_impl_win32.h>

namespace Spark::Ui
{
    void EnableProcessDpiAwareness() noexcept
    {
        ImGui_ImplWin32_EnableDpiAwareness();
    }

    float WindowDpiScale(void* nativeWindow) noexcept
    {
        if (nativeWindow == nullptr)
        {
            return 1.0f;
        }
        const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(nativeWindow);
        return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }
} // namespace Spark::Ui
