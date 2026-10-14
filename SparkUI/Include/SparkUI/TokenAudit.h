/**
 * @file TokenAudit.h
 * @brief Accessibility audit of a token set: WCAG 2.x contrast minimums, colour-vision-deficiency
 *        separation and focus-ring distinctness (spec section 2.5 as amended).
 *
 * ImGui-free. tools/ui/generate_palettes.py implements the same rules and refuses to write a
 * Palettes.inl that would fail them, so every built-in set passes with margin.
 *
 * Thread affinity: pure functions, callable from any thread. Ownership: results are returned by
 * value; AuditFailure::rule points at a string literal with static storage.
 */

#pragma once

#include "SparkUI/Tokens.h"

#include <string_view>
#include <vector>

namespace Spark::Ui
{
    /// @brief Minimum WCAG ratio for body and status text (Dark and Light appearances).
    inline constexpr float kAuditTextMinimum = 4.5f;
    /// @brief Minimum WCAG ratio for body and status text in the High Contrast appearance.
    inline constexpr float kAuditHighContrastTextMinimum = 7.0f;
    /// @brief Minimum WCAG ratio for disabled text, borders, focus, accent fill and graph colours.
    inline constexpr float kAuditNonTextMinimum = 3.0f;
    /// @brief Minimum OKLab distance between status texts (and status vs accent) under simulated CVD.
    inline constexpr float kAuditMinCvdDelta = 0.06f;
    /// @brief Minimum OKLab distance between the focus ring and the accent fill.
    inline constexpr float kAuditMinFocusAccentDelta = 0.08f;
    /// @brief Minimum OKLCH hue distance between the focus ring and the selection surface.
    inline constexpr float kAuditMinFocusHueDegrees = 60.0f;

    /// @brief One failed audit rule.
    /// @details For contrast rules @c ratio and @c required are WCAG ratios; for CVD and focus
    ///          distance rules they are OKLab distances; for the hue rule they are degrees.
    struct AuditFailure
    {
        Role fg = Role::TextPrimary;
        Role bg = Role::SurfaceBase;
        float ratio = 0.0f;
        float required = 0.0f;
        std::string_view rule;
    };

    /// @brief Run every audit rule on @p t.
    /// @return The failures, empty when the set passes. Rule names: "text-contrast",
    ///         "disabled-contrast", "on-accent-contrast", "status-on-fill", "border-strong",
    ///         "border-focus", "accent-on-base", "graph-on-sunken", "cvd-status-protan",
    ///         "cvd-status-deutan", "cvd-status-tritan", "cvd-accent-protan", "cvd-accent-deutan",
    ///         "focus-accent-distance", "focus-hue".
    std::vector<AuditFailure> AuditTokens(const Tokens& t);

    /// @brief TextSecondary colour of the audit's RED fixture (spec section 2.5): too dark on Base.
    inline constexpr Rgba kAuditFailFixtureTextSecondary = Hex(0x5f5a55);

    /// @brief The RED fixture: Ember Dark with TextSecondary replaced by @p textSecondary.
    /// @details With the default colour the audit must report at least one failure; tests assert
    ///          that so a disabled audit cannot pass silently.
    Tokens AuditFailFixture(Rgba textSecondary = kAuditFailFixtureTextSecondary) noexcept;
} // namespace Spark::Ui
