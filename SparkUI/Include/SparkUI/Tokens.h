/**
 * @file Tokens.h
 * @brief Ember Signal design tokens: semantic colour roles, metrics and typography generated
 *        from a palette seed and an appearance.
 *
 * ImGui-free. Every Spark UI surface (editor, launcher, installer GUI, in-game overlay) styles
 * itself from one Tokens value; SparkUI/Source/Palettes.inl holds the nine audited built-in sets.
 *
 * Thread affinity: every function here is pure (no shared state) and may be called from any
 * thread. Ownership: Tokens is a plain value type; copies are independent.
 */

#pragma once

#include "SparkUI/Enums.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Spark::Ui
{
    /// @brief Straight (non-premultiplied) sRGB colour with components in [0, 1].
    struct Rgba
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;

        friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
    };

    /// @brief Build a colour from a 0xRRGGBB literal and an alpha.
    constexpr Rgba Hex(std::uint32_t rgb, float a = 1.0f) noexcept
    {
        return Rgba{static_cast<float>((rgb >> 16u) & 0xFFu) / 255.0f, static_cast<float>((rgb >> 8u) & 0xFFu) / 255.0f,
                    static_cast<float>(rgb & 0xFFu) / 255.0f, a};
    }

    /// @brief Build a colour from a packed 0xRRGGBBAA value (the Palettes.inl encoding).
    constexpr Rgba FromPackedRgba(std::uint32_t rgba) noexcept
    {
        return Rgba{static_cast<float>((rgba >> 24u) & 0xFFu) / 255.0f,
                    static_cast<float>((rgba >> 16u) & 0xFFu) / 255.0f,
                    static_cast<float>((rgba >> 8u) & 0xFFu) / 255.0f, static_cast<float>(rgba & 0xFFu) / 255.0f};
    }

    /// @brief Appearance axis of a palette family.
    enum class Appearance : std::uint8_t
    {
        Dark,
        Light,
        HighContrast
    };

    /// @brief Semantic colour roles. The order is the column order of Tokens::color.
    enum class Role : std::uint16_t
    {
        SurfaceSunken,
        SurfaceBase,
        SurfaceRaised,
        SurfaceOverlay,
        SurfaceHover,
        SurfaceSelected,
        SurfaceSelectedUnfocused,
        BorderSubtle,
        BorderStrong,
        BorderFocus,
        TextPrimary,
        TextSecondary,
        TextDisabled,
        TextOnAccent,
        TextLink,
        AccentFill,
        AccentFillHover,
        AccentFillActive,
        AccentText,
        Secondary,
        SuccessText,
        WarningText,
        ErrorText,
        InfoText,
        SuccessFill,
        WarningFill,
        ErrorFill,
        InfoFill,
        Graph1,
        Graph2,
        Graph3,
        Graph4,
        Graph5,
        AxisX,
        AxisY,
        AxisZ,
        Category1,
        Category2,
        Category3,
        Category4,
        Category5,
        Category6,
        Category7,
        Category8,
        ModalScrim,
        DropTarget,
        SelectionTextBg,
        Count
    };

    /// @brief Number of colour roles.
    inline constexpr std::size_t kRoleCount = static_cast<std::size_t>(Role::Count);

    /// @brief Palette seed: the eight colours a family is generated from (spec section 2.1).
    struct PaletteSeed
    {
        std::string_view id;
        Rgba sunken;
        Rgba base;
        Rgba raised;
        Rgba borderSubtle;
        Rgba text;
        Rgba textMuted;
        Rgba accent;
        Rgba secondary;
    };

    /// @brief Logical (unscaled, 100 %) spacing, radius and border metrics.
    struct Metrics
    {
        float windowPadX = 0.0f;
        float windowPadY = 0.0f;
        float framePadX = 0.0f;
        float framePadY = 0.0f;
        float itemSpacingX = 0.0f;
        float itemSpacingY = 0.0f;
        float itemInnerX = 0.0f;
        float itemInnerY = 0.0f;
        float cellPadX = 0.0f;
        float cellPadY = 0.0f;
        float indent = 0.0f;
        float scrollbar = 0.0f;
        float grabMin = 0.0f;
        float rWindow = 0.0f;
        float rChild = 0.0f;
        float rFrame = 0.0f;
        float rPopup = 0.0f;
        float rScrollbar = 0.0f;
        float rGrab = 0.0f;
        float rTab = 0.0f;
        float rCard = 0.0f;
        float bWindow = 0.0f;
        float bChild = 0.0f;
        float bPopup = 0.0f;
        float bFrame = 0.0f;
        float tabOverline = 0.0f;
        float focusRing = 0.0f;
    };

    /// @brief Logical font sizes in pixels at 100 % for each FontRole.
    struct Typography
    {
        float small = 13.0f;
        float body = 15.0f;
        float strong = 15.0f;
        float h2 = 17.0f;
        float h1 = 20.0f;
        float display = 26.0f;
        float mono = 14.0f;
        float monoSmall = 12.5f;
    };

    /// @brief One complete token set: colours by role, metrics and typography.
    struct Tokens
    {
        Appearance appearance = Appearance::Dark;
        std::array<Rgba, kRoleCount> color{};
        Metrics metrics{};
        Typography type{};
    };

    /// @brief Seed of a built-in palette family (Ember, Cobalt, Graphite).
    const PaletteSeed& BuiltinSeed(PaletteId id) noexcept;

    /// @brief Generate the token set for @p seed in @p appearance with editor metrics.
    /// @details A built-in seed returns its audited table from Palettes.inl. Any other seed is
    ///          derived by the family rules (spec section 2.3) from the Ember table of the same
    ///          appearance, and its text roles are moved in OKLab lightness until they meet the
    ///          audit's contrast minimums.
    Tokens GenerateTokens(const PaletteSeed& seed, Appearance appearance, Density density = Density::Standard) noexcept;

    /// @brief Logical metrics for a density and surface profile.
    Metrics MetricsFor(Density density, MetricsProfile profile) noexcept;

    /// @brief Colour of role @p r in @p t.
    Rgba Get(const Tokens& t, Role r) noexcept;

    /// @brief Stable English identifier of a role ("TextPrimary"), for diagnostics.
    std::string_view RoleName(Role r) noexcept;

    /// @brief Stable English identifier of an appearance ("Dark", "Light", "HighContrast").
    std::string_view AppearanceName(Appearance appearance) noexcept;
} // namespace Spark::Ui
