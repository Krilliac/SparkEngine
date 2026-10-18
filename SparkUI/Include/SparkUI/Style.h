/**
 * @file Style.h
 * @brief Map a token set onto an ImGuiStyle (colours, metrics, font scale) at an effective scale.
 *
 * Thread affinity: ApplyStyle writes only the ImGuiStyle it is given and reads no ImGui global
 * state, so it may run on any thread that owns @p out. Ownership: the caller owns @p out.
 */

#pragma once

#include "SparkUI/Tokens.h"

#include <imgui.h>

namespace Spark::Ui
{
    /// @brief Smallest effective UI scale.
    inline constexpr float kMinUiScale = 0.75f;
    /// @brief Largest effective UI scale.
    inline constexpr float kMaxUiScale = 3.0f;

    /// @brief Rebuild @p out from the unscaled tokens at @p effectiveScale.
    /// @details Absolute and idempotent: the result depends only on @p t and @p effectiveScale
    ///          (plus the incoming FontScaleDpi/FontScaleMain, which are preserved), so applying
    ///          twice equals applying once. Metrics are floor(token x scale); hairline borders are
    ///          max(1, round(scale)). FontSizeBase is set to the body size.
    void ApplyStyle(const Tokens& t, float effectiveScale, ImGuiStyle& out);

    /// @brief clamp(dpiScale x userScale, 0.75, 3.0); a non-finite or non-positive factor counts as 1.
    float ClampScale(float dpiScale, float userScale) noexcept;

    /// @brief Token colour converted to an ImGui colour vector (no global alpha applied).
    ImVec4 ToImVec4(Rgba c) noexcept;
} // namespace Spark::Ui
