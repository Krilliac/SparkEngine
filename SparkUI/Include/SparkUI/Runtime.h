/**
 * @file Runtime.h
 * @brief Per-ImGuiContext Spark UI state: active tokens, DPI/user scale, pending scale changes,
 *        reduce-motion, and the role colour and status helpers built on them.
 *
 * Thread affinity: UI thread only; every function acts on the current ImGui context (Install
 * acts on the context it is given). Ownership: the state is heap-allocated on Install and owned
 * by the context; a Shutdown context hook frees it in ImGui::DestroyContext. Nothing is kept in
 * statics, so each module DLL that links its own imgui gets independent state per context.
 * Before Install every accessor answers with Ember Dark tokens at scale 1.
 */

#pragma once

#include "SparkUI/Enums.h"
#include "SparkUI/Tokens.h"

#include <imgui.h>

namespace Spark::Ui
{
    namespace Sdk
    {
        struct SparkUiRuntimeBlock;
    } // namespace Sdk

    /// @brief Create (or update) the Spark UI state of @p ctx and apply its style immediately.
    /// @details Sets io.ConfigDpiScaleFonts = false, style.FontScaleDpi = dpi scale and
    ///          style.FontScaleMain = effective scale / dpi scale.
    void Install(ImGuiContext* ctx, const Tokens& tokens, float dpiScale, float userScale, bool reduceMotion);

    /// @brief Store a scale change; it is applied by the next BeginFrameScale(), never mid-frame.
    void RequestScale(float dpiScale, float userScale);

    /// @brief Apply a pending scale change. Call before ImGui::NewFrame().
    /// @return True when a pending change was applied.
    bool BeginFrameScale();

    /// @brief Switch tokens (theme change) and re-apply the style at the current scale; the font
    ///        atlas is not reloaded.
    void SetTokens(const Tokens& tokens);

    /// @brief Tokens of the current context (Ember Dark before Install).
    const Tokens& ActiveTokens();

    /// @brief Effective scale, clamp(dpi x user, 0.75, 3).
    float Scale();

    /// @brief DPI part of the effective scale.
    float DpiScale();

    /// @brief User part of the scale as requested (before clamping).
    float UserScale();

    /// @brief Logical pixels to device pixels: floor(logical x Scale()), at least 1 for positive input.
    float Px(float logical);

    /// @brief Px() for both components.
    ImVec2 Px2(float x, float y);

    /// @brief @p ems times the current font size.
    float Em(float ems);

    /// @brief True when animations must be replaced by static presentation.
    bool ReduceMotion();

    /// @brief Set the reduce-motion preference of the current context.
    void SetReduceMotion(bool reduceMotion);

    /// @brief Role colour packed for draw lists; includes style.Alpha and @p alphaMul.
    ImU32 Col(Role r, float alphaMul = 1.0f);

    /// @brief Role colour as a vector for PushStyleColor; @p alphaMul scales its alpha.
    ImVec4 ColV(Role r, float alphaMul = 1.0f);

    /// @brief Text role of a status (Neutral maps to TextSecondary).
    Role StatusTextRole(Status s) noexcept;

    /// @brief Fill role of a status (Neutral maps to SurfaceRaised).
    Role StatusFillRole(Status s) noexcept;

    /// @brief Icon glyph of a status (UTF-8, Font Awesome).
    const char* StatusGlyph(Status s) noexcept;

    /// @brief English word of a status: "Note", "Info", "Success", "Warning", "Error".
    const char* StatusWord(Status s) noexcept;

    /// @brief Categorical colour (Okabe-Ito) for @p index; wraps every eight entries.
    ImU32 CategoryColor(int index);

    /// @brief Fill the SDK runtime block a host exposes to game modules through io.UserData.
    /// @details Defined only in builds that compile SparkSDK's UiTheme.h with SPARK_HAS_IMGUI
    ///          (SparkUIKit always does).
    void FillSdkRuntimeBlock(Sdk::SparkUiRuntimeBlock& out);
} // namespace Spark::Ui
