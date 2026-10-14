/**
 * @file Color.h
 * @brief Colour science for the token audit: sRGB transfer, WCAG 2.x contrast, OKLab/OKLCH and
 *        Machado 2009 colour-vision-deficiency simulation.
 *
 * ImGui-free. All functions are pure and thread-safe; they allocate nothing.
 */

#pragma once

#include "SparkUI/Tokens.h"

#include <cstdint>
#include <span>

namespace Spark::Ui
{
    /// @brief OKLab coordinates (Ottosson 2020). L is in [0, 1].
    struct OkLab
    {
        float L = 0.0f;
        float a = 0.0f;
        float b = 0.0f;
    };

    /// @brief Colour vision deficiency simulated at severity 1.0.
    enum class Cvd : std::uint8_t
    {
        Protan,
        Deutan,
        Tritan
    };

    /// @brief sRGB-encoded component to linear light.
    float SrgbToLinear(float c) noexcept;

    /// @brief Linear-light component to sRGB encoding (clamped to [0, 1]).
    float LinearToSrgb(float c) noexcept;

    /// @brief WCAG 2.x relative luminance of the colour's RGB (alpha ignored).
    float RelativeLuminance(Rgba c) noexcept;

    /// @brief WCAG 2.x contrast ratio; a translucent @p fg is first composited over @p bg.
    float ContrastRatio(Rgba fg, Rgba bg) noexcept;

    /// @brief Source-over composite of @p top onto @p bottom; the result is opaque when @p bottom is.
    Rgba Composite(Rgba top, Rgba bottom) noexcept;

    /// @brief sRGB colour to OKLab.
    OkLab ToOkLab(Rgba c) noexcept;

    /// @brief OKLab to sRGB, clamped into the sRGB gamut, with alpha @p a.
    Rgba FromOkLab(OkLab lab, float a = 1.0f) noexcept;

    /// @brief OKLCH hue of @p c in degrees [0, 360).
    float HueDegrees(Rgba c) noexcept;

    /// @brief Shortest angular distance between the OKLCH hues of two colours, in degrees.
    float HueDistanceDegrees(Rgba x, Rgba y) noexcept;

    /// @brief Simulate @p kind (Machado, Oliveira and Fernandes 2009, severity 1.0) and return
    ///        the perceived colour in OKLab.
    OkLab SimulateCvdLab(Rgba c, Cvd kind) noexcept;

    /// @brief Simulate @p kind and return the perceived colour as sRGB.
    Rgba SimulateCvd(Rgba c, Cvd kind) noexcept;

    /// @brief Euclidean OKLab distance.
    float DeltaE(OkLab x, OkLab y) noexcept;

    /// @brief Linear sRGB-space interpolation from @p x to @p y (alpha of the result is 1).
    Rgba Mix(Rgba x, Rgba y, float t) noexcept;

    /// @brief @p c with its alpha replaced.
    constexpr Rgba WithAlpha(Rgba c, float a) noexcept
    {
        return Rgba{c.r, c.g, c.b, a};
    }

    /// @brief @p c with its OKLab lightness moved by @p amount (hue and chroma kept).
    Rgba ShiftLightness(Rgba c, float amount) noexcept;

    /// @brief Move @p fg's OKLab lightness (hue and chroma fixed) by the smallest amount that
    ///        reaches @p minRatio against every colour in @p backgrounds.
    /// @param converged Set to true when the returned colour meets the ratio on all backgrounds.
    /// @return The adjusted colour, or the best-contrast extreme when no lightness suffices.
    /// @details Bisects lightness towards white and towards black (at most 64 evaluations) and
    ///          keeps the smaller move that passes.
    Rgba EnsureContrast(Rgba fg, std::span<const Rgba> backgrounds, float minRatio, bool* converged = nullptr) noexcept;
} // namespace Spark::Ui
