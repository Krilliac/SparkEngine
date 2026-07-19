/**
 * @file TFHUDInternal.h
 * @brief Shared internals for the TFHUD*.cpp split parts: the hitmarker
 *        timing constant (feed-in setter + crosshair drawing) and the
 *        faction-color helper used by more than one drawing part. Include
 *        only from the TFHUD translation units.
 */
#pragma once

#include "Core/TFTypes.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

namespace Terrafront
{
    namespace HudDetail
    {

        inline constexpr float kHitmarkerDuration = 0.30f; // seconds

#ifdef SPARK_HAS_IMGUI

        inline ImU32 FactionCol(FactionId f, float alpha = 1.0f)
        {
            float c[4];
            FactionColor(f, c);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
        }

#endif // SPARK_HAS_IMGUI

    } // namespace HudDetail
} // namespace Terrafront
