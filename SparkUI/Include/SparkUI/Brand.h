/**
 * @file Brand.h
 * @brief Spark brand drawing: the logomark, the ember gradient, soft shadows and the brand header.
 *
 * The logomark geometry constants are shared verbatim with
 * Assets/Engine/Branding/generate_logomark.py so the vector asset and the in-app mark match.
 *
 * Thread affinity: UI thread only; draws into the given draw list (BrandHeader into the current
 * window). Ownership: nothing is retained.
 */

#pragma once

#include <cstdint>

#include <imgui.h>

namespace Spark::Ui
{
    /// @brief Logomark geometry in a unit square (y down).
    namespace Logomark
    {
        inline constexpr float kCenterX = 0.5f;
        inline constexpr float kCenterY = 0.5f;
        inline constexpr float kOuterRadius = 0.46f;
        inline constexpr float kInnerRadius = 0.13f;
        inline constexpr float kRotationDegrees = 0.0f;
        inline constexpr float kDotCenterX = 0.78f;
        inline constexpr float kDotCenterY = 0.22f;
        inline constexpr float kDotRadius = 0.075f;
    } // namespace Logomark

    /// @brief Brand gradient stops: ember-600, ember, spark (0xRRGGBB).
    inline constexpr std::uint32_t kBrandGradientStops[3] = {0xe8611a, 0xf1823a, 0xffc857};

    /// @brief Draw the four-point star logomark with its ember dot.
    /// @param center Center of the mark's square in screen pixels.
    /// @param size Side of the square in screen pixels.
    /// @param glow Add a soft halo behind the mark.
    /// @param tint Solid colour instead of the gradient; 0 keeps the gradient.
    void DrawLogomark(ImDrawList* drawList, ImVec2 center, float size, bool glow = false, ImU32 tint = 0);

    /// @brief Fill a rectangle with the brand gradient (diagonal, or top-to-bottom when @p vertical).
    void DrawBrandGradient(ImDrawList* drawList, ImVec2 min, ImVec2 max, float alpha = 1.0f, bool vertical = false);

    /// @brief Logomark plus Display-face title and optional subtitle in the current window.
    void BrandHeader(const char* title, const char* subtitle = nullptr);
} // namespace Spark::Ui
