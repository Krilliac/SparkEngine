/**
 * @file TFHUDInternal.h
 * @brief Shared internals for the TFHUD*.cpp split parts: the hitmarker
 *        timing constant (feed-in setter + crosshair drawing) and the
 *        faction-color helper used by more than one drawing part (it
 *        forwards to TFUi::FactionCol so the colour mapping exists once). Include
 *        only from the TFHUD translation units.
 */
#pragma once

#include "Core/TFTypes.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h" // TFUi::FactionCol + Spark::Ui::Sdk (HudFont, PushSuccessButton)

#include <imgui.h>
#endif

namespace Terrafront
{
    namespace HudDetail
    {

        inline constexpr float kHitmarkerDuration = 0.30f; // seconds

#ifdef SPARK_HAS_IMGUI

        /// Faction colour for the HUD parts; forwards to the one shared implementation in TFUiCommon.h.
        inline ImU32 FactionCol(FactionId f, float alpha = 1.0f)
        {
            return TFUi::FactionCol(f, alpha);
        }

#endif // SPARK_HAS_IMGUI

    } // namespace HudDetail
} // namespace Terrafront
