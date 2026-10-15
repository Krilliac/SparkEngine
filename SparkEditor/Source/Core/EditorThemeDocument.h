/**
 * @file EditorThemeDocument.h
 * @brief Reader and writer for exported editor theme files (ThemeCustomizer import/export)
 *
 * ThemeCustomizer::ExportTheme writes what WriteThemeDocument renders and
 * ThemeCustomizer::ImportTheme parses a file through ParseThemeDocument. The format is a
 * JSON-like document scraped by key: "name"/"description"/"author"/"fontFamily"/"appearance"/
 * "density" strings, one [r, g, b, a] array per colour, one number per metric and one
 * true/false per feature flag. The format only ever grows: the original 25-colour documents
 * still import, a key a document lacks keeps the caller's base value, and unknown keys are
 * ignored. Both functions live in their own translation unit, ImGui-free and free of SparkUI
 * link dependencies, so the SEC-120 fuzz target links the shipped reader.
 *
 * Thread affinity: any thread; no shared state.
 * Ownership: inputs are borrowed; @p outTheme is written in place.
 * Allocation: the strings of the theme plus a few temporaries, bounded by the caller's
 * file-size limit.
 */

#pragma once

#include "EditorTheme.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace SparkEditor
{

    /// @brief Largest theme file ImportTheme reads (an exported theme is about 4 KB).
    inline constexpr std::uint64_t kMaxThemeDocumentBytes = std::uint64_t{1024} * 1024;

    /// @brief Value of the "version" key WriteThemeDocument emits. Readers ignore it today because
    ///        the format is additive; it lets a later incompatible revision be detected.
    inline constexpr int kThemeDocumentVersion = 1;

    // Pointer-to-member types are named once here: clang-format 18.1.3 (CI) and 18.1.8 disagree
    // on the spacing of a declarator written as `T Class::* name`, but agree on these aliases.

    /// @brief A colour field of EditorThemeData.
    using ThemeColourMember = ThemeColor EditorThemeData::*;
    /// @brief A numeric metric field of EditorThemeData.
    using ThemeMetricMember = float EditorThemeData::*;
    /// @brief A boolean feature-flag field of EditorThemeData.
    using ThemeFlagMember = bool EditorThemeData::*;

    /// @brief One colour the format carries: its document key and the field it fills.
    struct ThemeColourField
    {
        std::string_view key;
        ThemeColourMember member;
    };

    /// @brief One numeric metric the format carries; imported values are clamped to the range.
    struct ThemeMetricField
    {
        std::string_view key;
        ThemeMetricMember member;
        float minimum;
        float maximum;
    };

    /// @brief One boolean feature flag the format carries.
    struct ThemeFlagField
    {
        std::string_view key;
        ThemeFlagMember member;
    };

    /// @brief Every colour field except statusFills (keyed "statusFill<Info|Success|Warning|Error>").
    ///        The original 25-colour format is the first 25 entries.
    inline constexpr std::array<ThemeColourField, 58> kThemeColourFields = {{
        {"background", &EditorThemeData::background},
        {"backgroundDark", &EditorThemeData::backgroundDark},
        {"backgroundLight", &EditorThemeData::backgroundLight},
        {"backgroundAccent", &EditorThemeData::backgroundAccent},
        {"backgroundHeader", &EditorThemeData::backgroundHeader},
        {"backgroundActive", &EditorThemeData::backgroundActive},
        {"backgroundHover", &EditorThemeData::backgroundHover},
        {"backgroundSelected", &EditorThemeData::backgroundSelected},
        {"text", &EditorThemeData::text},
        {"textDisabled", &EditorThemeData::textDisabled},
        {"textSecondary", &EditorThemeData::textSecondary},
        {"textAccent", &EditorThemeData::textAccent},
        {"textWarning", &EditorThemeData::textWarning},
        {"textError", &EditorThemeData::textError},
        {"textSuccess", &EditorThemeData::textSuccess},
        {"button", &EditorThemeData::button},
        {"buttonHovered", &EditorThemeData::buttonHovered},
        {"buttonActive", &EditorThemeData::buttonActive},
        {"frame", &EditorThemeData::frame},
        {"frameHovered", &EditorThemeData::frameHovered},
        {"frameActive", &EditorThemeData::frameActive},
        {"border", &EditorThemeData::border},
        {"borderLight", &EditorThemeData::borderLight},
        {"borderAccent", &EditorThemeData::borderAccent},
        {"borderSeparator", &EditorThemeData::borderSeparator},
        {"buttonDisabled", &EditorThemeData::buttonDisabled},
        {"titleBar", &EditorThemeData::titleBar},
        {"titleBarActive", &EditorThemeData::titleBarActive},
        {"titleBarText", &EditorThemeData::titleBarText},
        {"menuBar", &EditorThemeData::menuBar},
        {"menuItem", &EditorThemeData::menuItem},
        {"menuItemHovered", &EditorThemeData::menuItemHovered},
        {"scrollbar", &EditorThemeData::scrollbar},
        {"scrollbarGrab", &EditorThemeData::scrollbarGrab},
        {"scrollbarGrabHovered", &EditorThemeData::scrollbarGrabHovered},
        {"scrollbarGrabActive", &EditorThemeData::scrollbarGrabActive},
        {"tab", &EditorThemeData::tab},
        {"tabHovered", &EditorThemeData::tabHovered},
        {"tabActive", &EditorThemeData::tabActive},
        {"tabUnfocused", &EditorThemeData::tabUnfocused},
        {"accent", &EditorThemeData::accent},
        {"accentSecondary", &EditorThemeData::accentSecondary},
        {"focus", &EditorThemeData::focus},
        {"selection", &EditorThemeData::selection},
        {"drop", &EditorThemeData::drop},
        {"graph1", &EditorThemeData::graph1},
        {"graph2", &EditorThemeData::graph2},
        {"graph3", &EditorThemeData::graph3},
        {"graph4", &EditorThemeData::graph4},
        {"graph5", &EditorThemeData::graph5},
        {"borderStrong", &EditorThemeData::borderStrong},
        {"surfaceOverlay", &EditorThemeData::surfaceOverlay},
        {"surfaceSelectedUnfocused", &EditorThemeData::surfaceSelectedUnfocused},
        {"textOnAccent", &EditorThemeData::textOnAccent},
        {"textInfo", &EditorThemeData::textInfo},
        {"accentHover", &EditorThemeData::accentHover},
        {"accentActive", &EditorThemeData::accentActive},
        {"accentText", &EditorThemeData::accentText},
    }};

    /// @brief Every float metric (rounding, borders, spacing, padding, shadow).
    inline constexpr std::array<ThemeMetricField, 24> kThemeMetricFields = {{
        {"windowRounding", &EditorThemeData::windowRounding, 0.0f, 64.0f},
        {"childRounding", &EditorThemeData::childRounding, 0.0f, 64.0f},
        {"frameRounding", &EditorThemeData::frameRounding, 0.0f, 64.0f},
        {"popupRounding", &EditorThemeData::popupRounding, 0.0f, 64.0f},
        {"scrollbarRounding", &EditorThemeData::scrollbarRounding, 0.0f, 64.0f},
        {"grabRounding", &EditorThemeData::grabRounding, 0.0f, 64.0f},
        {"tabRounding", &EditorThemeData::tabRounding, 0.0f, 64.0f},
        {"windowBorderSize", &EditorThemeData::windowBorderSize, 0.0f, 16.0f},
        {"childBorderSize", &EditorThemeData::childBorderSize, 0.0f, 16.0f},
        {"popupBorderSize", &EditorThemeData::popupBorderSize, 0.0f, 16.0f},
        {"frameBorderSize", &EditorThemeData::frameBorderSize, 0.0f, 16.0f},
        {"indentSpacing", &EditorThemeData::indentSpacing, 0.0f, 128.0f},
        {"scrollbarSize", &EditorThemeData::scrollbarSize, 4.0f, 64.0f},
        {"grabMinSize", &EditorThemeData::grabMinSize, 1.0f, 64.0f},
        {"windowPaddingX", &EditorThemeData::windowPaddingX, 0.0f, 64.0f},
        {"windowPaddingY", &EditorThemeData::windowPaddingY, 0.0f, 64.0f},
        {"framePaddingX", &EditorThemeData::framePaddingX, 0.0f, 64.0f},
        {"framePaddingY", &EditorThemeData::framePaddingY, 0.0f, 64.0f},
        {"itemSpacingX", &EditorThemeData::itemSpacingX, 0.0f, 64.0f},
        {"itemSpacingY", &EditorThemeData::itemSpacingY, 0.0f, 64.0f},
        {"itemInnerSpacingX", &EditorThemeData::itemInnerSpacingX, 0.0f, 64.0f},
        {"itemInnerSpacingY", &EditorThemeData::itemInnerSpacingY, 0.0f, 64.0f},
        {"shadowOpacity", &EditorThemeData::shadowOpacity, 0.0f, 1.0f},
        {"shadowSize", &EditorThemeData::shadowSize, 0.0f, 128.0f},
    }};

    /// @brief Typography and focus-ring metrics.
    inline constexpr std::array<ThemeMetricField, 3> kThemeTypographyFields = {{
        {"fontSize", &EditorThemeData::fontSize, 6.0f, 96.0f},
        {"fontScale", &EditorThemeData::fontScale, 0.5f, 3.0f},
        {"focusRingWidth", &EditorThemeData::focusRingWidth, 0.0f, 8.0f},
    }};

    /// @brief Every boolean feature flag.
    inline constexpr std::array<ThemeFlagField, 3> kThemeFlagFields = {{
        {"enableAnimations", &EditorThemeData::enableAnimations},
        {"enableGradients", &EditorThemeData::enableGradients},
        {"enableShadows", &EditorThemeData::enableShadows},
    }};

    /**
     * @brief Parse an exported theme into @p outTheme, taking missing fields from @p base.
     *
     * @p outTheme starts as a copy of @p base. Name, description and author are always read from
     * the document (empty when absent). Every other key the document carries replaces the base
     * value; a key that is missing, malformed or not finite keeps the base value, and metrics
     * are clamped into their ranges. When @p base carries no Ember Signal roles (every one of
     * them is the default opaque black, as in a default-constructed theme) the roles the
     * document does not define are derived from the colours it does define, so a legacy
     * 25-colour document imported over a default theme never yields black fills.
     * @param content The document text (callers bound it by kMaxThemeDocumentBytes).
     * @param base Source of every value the document does not define.
     * @param outTheme Receives the theme; must not alias @p base.
     * @param error When non-null, receives the reason on failure and is cleared on success.
     * @return true when the document names a theme.
     */
    bool ParseThemeDocument(const std::string& content, const EditorThemeData& base, EditorThemeData& outTheme,
                            std::string* error = nullptr);

    /**
     * @brief Parse an exported theme into @p outTheme, keeping its current values for missing keys.
     *
     * Equivalent to the base overload with the theme @p outTheme holds on entry as the base.
     * @return true when the document names a theme.
     */
    bool ParseThemeDocument(const std::string& content, EditorThemeData& outTheme);

    /// @brief Render @p theme as ExportTheme writes it: version, strings, appearance, density,
    ///        every colour, every metric and every feature flag.
    std::string WriteThemeDocument(const EditorThemeData& theme);

} // namespace SparkEditor
