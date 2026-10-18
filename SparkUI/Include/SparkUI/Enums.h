/**
 * @file Enums.h
 * @brief Small value enums shared by every Spark UI surface (density, metrics profile, status,
 *        font role and built-in palette id).
 *
 * ImGui-free and header-only: headless code (editor preferences, the preferences fuzz target,
 * SparkTests headless sources) may include it without linking any SparkUI library.
 */

#pragma once

#include <cstdint>

namespace Spark::Ui
{
    /// @brief Control density. Text never shrinks; only paddings and spacings change.
    enum class Density : std::uint8_t
    {
        Standard, ///< 25 px controls at 100 % (meets the WCAG 2.5.8 24 px target size).
        Compact   ///< 21 px controls; opt-in.
    };

    /// @brief Spacing profile of a surface.
    enum class MetricsProfile : std::uint8_t
    {
        Editor,      ///< Dense tool windows (editor panels).
        Comfortable, ///< Launcher and installer cards and forms.
        Hud          ///< In-game overlay.
    };

    /// @brief Semantic status. Rendered as glyph + word + colour, never by colour alone.
    enum class Status : std::uint8_t
    {
        Neutral,
        Info,
        Success,
        Warning,
        Error
    };

    /// @brief Typographic role; every role maps to one loaded face and one logical size.
    enum class FontRole : std::uint8_t
    {
        Small,
        Body,
        Strong,
        H2,
        H1,
        Display,
        Mono,
        MonoSmall
    };

    /// @brief Built-in palette families.
    enum class PaletteId : std::uint8_t
    {
        Ember,
        Cobalt,
        Graphite
    };
} // namespace Spark::Ui
