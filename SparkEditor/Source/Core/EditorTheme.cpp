/**
 * @file EditorTheme.cpp
 * @brief Theme registry and the EditorThemeData -> Spark UI tokens -> ImGui style adapter.
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorTheme.h"
#include "EditorThemeDocument.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Validate.h"
#include "SparkUI/Color.h"
#include "SparkUI/Runtime.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <unordered_set>

#include "../Utils/EditorFileRead.h"

namespace SparkEditor
{

    // Static member initialization
    std::unordered_map<std::string, EditorThemeData> EditorTheme::s_registeredThemes;
    std::string EditorTheme::s_currentThemeName;
    EditorThemeData EditorTheme::s_currentThemeData;
    bool EditorTheme::s_currentIsBuiltinPalette = false;
    Spark::Ui::Density EditorTheme::s_density = Spark::Ui::Density::Standard;
    std::optional<Spark::Ui::Appearance> EditorTheme::s_appearanceOverride;
    std::uint64_t EditorTheme::s_revision = 0;
    bool EditorTheme::s_enhancementsEnabled = false;
    bool EditorTheme::s_customFontsLoaded = false;

    namespace
    {
        using Spark::Ui::Appearance;
        using Spark::Ui::Density;
        using Spark::Ui::PaletteId;
        using Spark::Ui::Role;
        using Spark::Ui::Tokens;

        /// One entry of the theme picker, generated from the audited token tables.
        struct BuiltinPalette
        {
            const char* name;
            const char* description;
            PaletteId palette;
            Appearance appearance;
        };

        constexpr std::array<BuiltinPalette, 5> kBuiltinPalettes = {{
            {"Ember Studio", "Warm charcoal workspace with an ember accent and a cyan focus ring", PaletteId::Ember,
             Appearance::Dark},
            {"Cobalt Forge", "Cool slate workspace with a cyan accent and an amber focus ring", PaletteId::Cobalt,
             Appearance::Dark},
            {"Graphite Signal", "Neutral graphite workspace with a silver accent and a mint focus ring",
             PaletteId::Graphite, Appearance::Dark},
            {"Professional Light", "Ember Studio on light surfaces", PaletteId::Ember, Appearance::Light},
            {"High Contrast", "Ember Studio at 7:1 text contrast with a yellow focus ring", PaletteId::Ember,
             Appearance::HighContrast},
        }};

        /// Every ThemeColor member except the statusFills array (blended separately).
        constexpr auto kColorFields = std::to_array<ThemeColor EditorThemeData::*>({
            &EditorThemeData::background,
            &EditorThemeData::backgroundDark,
            &EditorThemeData::backgroundLight,
            &EditorThemeData::backgroundAccent,
            &EditorThemeData::backgroundHeader,
            &EditorThemeData::backgroundActive,
            &EditorThemeData::backgroundHover,
            &EditorThemeData::backgroundSelected,
            &EditorThemeData::text,
            &EditorThemeData::textDisabled,
            &EditorThemeData::textSecondary,
            &EditorThemeData::textAccent,
            &EditorThemeData::textWarning,
            &EditorThemeData::textError,
            &EditorThemeData::textSuccess,
            &EditorThemeData::button,
            &EditorThemeData::buttonHovered,
            &EditorThemeData::buttonActive,
            &EditorThemeData::buttonDisabled,
            &EditorThemeData::frame,
            &EditorThemeData::frameHovered,
            &EditorThemeData::frameActive,
            &EditorThemeData::border,
            &EditorThemeData::borderLight,
            &EditorThemeData::borderAccent,
            &EditorThemeData::borderSeparator,
            &EditorThemeData::titleBar,
            &EditorThemeData::titleBarActive,
            &EditorThemeData::titleBarText,
            &EditorThemeData::menuBar,
            &EditorThemeData::menuItem,
            &EditorThemeData::menuItemHovered,
            &EditorThemeData::scrollbar,
            &EditorThemeData::scrollbarGrab,
            &EditorThemeData::scrollbarGrabHovered,
            &EditorThemeData::scrollbarGrabActive,
            &EditorThemeData::tab,
            &EditorThemeData::tabHovered,
            &EditorThemeData::tabActive,
            &EditorThemeData::tabUnfocused,
            &EditorThemeData::accent,
            &EditorThemeData::accentSecondary,
            &EditorThemeData::focus,
            &EditorThemeData::selection,
            &EditorThemeData::drop,
            &EditorThemeData::graph1,
            &EditorThemeData::graph2,
            &EditorThemeData::graph3,
            &EditorThemeData::graph4,
            &EditorThemeData::graph5,
            &EditorThemeData::borderStrong,
            &EditorThemeData::surfaceOverlay,
            &EditorThemeData::surfaceSelectedUnfocused,
            &EditorThemeData::textOnAccent,
            &EditorThemeData::textInfo,
            &EditorThemeData::accentHover,
            &EditorThemeData::accentActive,
            &EditorThemeData::accentText,
        });

        /// Every float style value; CreateBlendedTheme interpolates all of them.
        constexpr auto kMetricFields = std::to_array<float EditorThemeData::*>({
            &EditorThemeData::windowRounding,    &EditorThemeData::childRounding,
            &EditorThemeData::frameRounding,     &EditorThemeData::popupRounding,
            &EditorThemeData::scrollbarRounding, &EditorThemeData::grabRounding,
            &EditorThemeData::tabRounding,       &EditorThemeData::windowBorderSize,
            &EditorThemeData::childBorderSize,   &EditorThemeData::popupBorderSize,
            &EditorThemeData::frameBorderSize,   &EditorThemeData::indentSpacing,
            &EditorThemeData::scrollbarSize,     &EditorThemeData::grabMinSize,
            &EditorThemeData::windowPaddingX,    &EditorThemeData::windowPaddingY,
            &EditorThemeData::framePaddingX,     &EditorThemeData::framePaddingY,
            &EditorThemeData::itemSpacingX,      &EditorThemeData::itemSpacingY,
            &EditorThemeData::itemInnerSpacingX, &EditorThemeData::itemInnerSpacingY,
            &EditorThemeData::shadowOpacity,     &EditorThemeData::shadowSize,
            &EditorThemeData::fontSize,          &EditorThemeData::fontScale,
            &EditorThemeData::focusRingWidth,
        });

        constexpr float kBodyFontSize = 15.0f;
        constexpr float kTableRowAltAlpha = 0.45f;
        constexpr float kDropWashAlpha = 0.12f;
        constexpr float kLegacyStatusFillMix = 0.15f;
        constexpr float kLegacyUnfocusedSelectionMix = 0.22f;
        constexpr float kLegacyAccentStep = 0.12f;

        ThemeColor FromRgba(Spark::Ui::Rgba c)
        {
            return ThemeColor(c.r, c.g, c.b, c.a);
        }

        Spark::Ui::Rgba ToRgba(const ThemeColor& c)
        {
            return Spark::Ui::Rgba{c.r, c.g, c.b, c.a};
        }

        ThemeColor RoleColor(const Tokens& t, Role role)
        {
            return FromRgba(Spark::Ui::Get(t, role));
        }

        ThemeColor& StatusFill(EditorThemeData& theme, ThemeStatusFill which)
        {
            return theme.statusFills[static_cast<std::size_t>(which)];
        }

        const ThemeColor& StatusFill(const EditorThemeData& theme, ThemeStatusFill which)
        {
            return theme.statusFills[static_cast<std::size_t>(which)];
        }

        float SaneFactor(float value, float fallback)
        {
            return std::isfinite(value) && value > 0.0f ? value : fallback;
        }

        const BuiltinPalette* FindBuiltinPalette(std::string_view name)
        {
            for (const BuiltinPalette& palette : kBuiltinPalettes)
            {
                if (name == palette.name)
                {
                    return &palette;
                }
            }
            return nullptr;
        }

        /// Built-in names that RegisterTheme replaced with a custom palette; those are applied as
        /// registered and never regenerated.
        std::unordered_set<std::string>& ShadowedBuiltinNames()
        {
            static std::unordered_set<std::string> names;
            return names;
        }

        /// EditorThemeData carrying every role of @p t (spec section 2.2 field mapping).
        EditorThemeData ThemeFromTokens(const Tokens& t, const BuiltinPalette& palette, Density density)
        {
            EditorThemeData theme;
            theme.name = palette.name;
            theme.description = palette.description;
            theme.author = "Spark Engine Team";

            const ThemeColor sunken = RoleColor(t, Role::SurfaceSunken);
            const ThemeColor base = RoleColor(t, Role::SurfaceBase);
            const ThemeColor raised = RoleColor(t, Role::SurfaceRaised);
            const ThemeColor overlay = RoleColor(t, Role::SurfaceOverlay);
            const ThemeColor hover = RoleColor(t, Role::SurfaceHover);
            const ThemeColor selected = RoleColor(t, Role::SurfaceSelected);
            const ThemeColor borderSubtle = RoleColor(t, Role::BorderSubtle);
            const ThemeColor borderStrong = RoleColor(t, Role::BorderStrong);
            const ThemeColor textPrimary = RoleColor(t, Role::TextPrimary);
            const ThemeColor textDisabled = RoleColor(t, Role::TextDisabled);
            const ThemeColor accent = RoleColor(t, Role::AccentFill);
            const ThemeColor secondary = RoleColor(t, Role::Secondary);

            theme.background = base;
            theme.backgroundDark = sunken;
            theme.backgroundLight = raised;
            theme.backgroundAccent = secondary.WithAlpha(secondary.a * kDropWashAlpha);
            theme.backgroundHeader = raised;
            theme.backgroundActive = selected;
            theme.backgroundHover = hover;
            theme.backgroundSelected = selected;

            theme.text = textPrimary;
            theme.textDisabled = textDisabled;
            theme.textSecondary = RoleColor(t, Role::TextSecondary);
            theme.textAccent = RoleColor(t, Role::TextLink);
            theme.textWarning = RoleColor(t, Role::WarningText);
            theme.textError = RoleColor(t, Role::ErrorText);
            theme.textSuccess = RoleColor(t, Role::SuccessText);

            theme.button = raised;
            theme.buttonHovered = hover;
            theme.buttonActive = selected;
            theme.buttonDisabled = textDisabled;

            theme.frame = sunken;
            theme.frameHovered = hover;
            theme.frameActive = selected;

            theme.border = borderSubtle;
            theme.borderLight = borderStrong;
            theme.borderAccent = accent;
            theme.borderSeparator = borderSubtle;

            theme.titleBar = sunken;
            theme.titleBarActive = sunken;
            theme.titleBarText = textPrimary;

            theme.menuBar = sunken;
            theme.menuItem = overlay;
            theme.menuItemHovered = hover;

            theme.scrollbar = sunken;
            theme.scrollbarGrab = borderSubtle;
            theme.scrollbarGrabHovered = borderStrong;
            theme.scrollbarGrabActive = accent;

            theme.tab = sunken;
            theme.tabHovered = hover;
            theme.tabActive = base;
            theme.tabUnfocused = sunken;

            theme.accent = accent;
            theme.accentSecondary = secondary;
            theme.focus = RoleColor(t, Role::BorderFocus);
            theme.selection = RoleColor(t, Role::SelectionTextBg);
            theme.drop = RoleColor(t, Role::DropTarget);

            theme.graph1 = RoleColor(t, Role::Graph1);
            theme.graph2 = RoleColor(t, Role::Graph2);
            theme.graph3 = RoleColor(t, Role::Graph3);
            theme.graph4 = RoleColor(t, Role::Graph4);
            theme.graph5 = RoleColor(t, Role::Graph5);

            theme.borderStrong = borderStrong;
            theme.surfaceOverlay = overlay;
            theme.surfaceSelectedUnfocused = RoleColor(t, Role::SurfaceSelectedUnfocused);
            theme.textOnAccent = RoleColor(t, Role::TextOnAccent);
            theme.textInfo = RoleColor(t, Role::InfoText);
            theme.accentHover = RoleColor(t, Role::AccentFillHover);
            theme.accentActive = RoleColor(t, Role::AccentFillActive);
            theme.accentText = RoleColor(t, Role::AccentText);
            StatusFill(theme, ThemeStatusFill::Info) = RoleColor(t, Role::InfoFill);
            StatusFill(theme, ThemeStatusFill::Success) = RoleColor(t, Role::SuccessFill);
            StatusFill(theme, ThemeStatusFill::Warning) = RoleColor(t, Role::WarningFill);
            StatusFill(theme, ThemeStatusFill::Error) = RoleColor(t, Role::ErrorFill);
            theme.appearance = t.appearance;
            theme.density = density;
            theme.focusRingWidth = t.metrics.focusRing;

            const Spark::Ui::Metrics& m = t.metrics;
            theme.windowRounding = m.rWindow;
            theme.childRounding = m.rChild;
            theme.frameRounding = m.rFrame;
            theme.popupRounding = m.rPopup;
            theme.scrollbarRounding = m.rScrollbar;
            theme.grabRounding = m.rGrab;
            theme.tabRounding = m.rTab;
            theme.windowBorderSize = m.bWindow;
            theme.childBorderSize = m.bChild;
            theme.popupBorderSize = m.bPopup;
            theme.frameBorderSize = m.bFrame;
            theme.indentSpacing = m.indent;
            theme.scrollbarSize = m.scrollbar;
            theme.grabMinSize = m.grabMin;
            theme.windowPaddingX = m.windowPadX;
            theme.windowPaddingY = m.windowPadY;
            theme.framePaddingX = m.framePadX;
            theme.framePaddingY = m.framePadY;
            theme.itemSpacingX = m.itemSpacingX;
            theme.itemSpacingY = m.itemSpacingY;
            theme.itemInnerSpacingX = m.itemInnerX;
            theme.itemInnerSpacingY = m.itemInnerY;

            theme.shadowOpacity = 0.45f;
            theme.shadowSize = 10.0f;
            theme.fontSize = t.type.body;
            theme.fontScale = 1.0f;
            theme.fontFamily = "IBM Plex Sans";
            return theme;
        }

        EditorThemeData BuiltinTheme(const BuiltinPalette& palette, Appearance appearance, Density density)
        {
            const Tokens tokens =
                Spark::Ui::GenerateTokens(Spark::Ui::BuiltinSeed(palette.palette), appearance, density);
            return ThemeFromTokens(tokens, palette, density);
        }

        /// Dark ink or white, whichever reads better on @p fill.
        ThemeColor ReadableInkOn(const ThemeColor& fill)
        {
            const Spark::Ui::Rgba darkInk = Spark::Ui::Hex(0x1a0f08);
            const Spark::Ui::Rgba lightInk = Spark::Ui::Hex(0xffffff);
            const Spark::Ui::Rgba background = ToRgba(fill.WithAlpha(1.0f));
            return FromRgba(Spark::Ui::ContrastRatio(darkInk, background) >=
                                    Spark::Ui::ContrastRatio(lightInk, background)
                                ? darkInk
                                : lightInk);
        }

        /// Fill the Ember Signal roles a hand-written (pre-token) palette does not set, from the
        /// colours it does set.
        void CompleteLegacyFields(EditorThemeData& t)
        {
            t.buttonDisabled = t.textDisabled;
            t.menuItem = t.backgroundLight;
            t.borderStrong = t.borderLight;
            t.surfaceOverlay = t.backgroundLight;
            t.surfaceSelectedUnfocused = t.background.Lerp(t.textSecondary, kLegacyUnfocusedSelectionMix);
            t.textInfo = t.textAccent;
            t.accentHover = t.accent.Lighten(kLegacyAccentStep);
            t.accentActive = t.accent.Darken(kLegacyAccentStep);
            t.accentText = t.textAccent;
            t.textOnAccent = ReadableInkOn(t.accent);
            const ThemeColor opaqueBackground = t.background.WithAlpha(1.0f);
            StatusFill(t, ThemeStatusFill::Info) =
                opaqueBackground.Lerp(t.textInfo.WithAlpha(1.0f), kLegacyStatusFillMix);
            StatusFill(t, ThemeStatusFill::Success) =
                opaqueBackground.Lerp(t.textSuccess.WithAlpha(1.0f), kLegacyStatusFillMix);
            StatusFill(t, ThemeStatusFill::Warning) =
                opaqueBackground.Lerp(t.textWarning.WithAlpha(1.0f), kLegacyStatusFillMix);
            StatusFill(t, ThemeStatusFill::Error) =
                opaqueBackground.Lerp(t.textError.WithAlpha(1.0f), kLegacyStatusFillMix);
            t.appearance =
                Spark::Ui::RelativeLuminance(ToRgba(t.background)) > 0.5f ? Appearance::Light : Appearance::Dark;
        }

        /// The registered theme, or its regeneration for the active appearance override and density
        /// when it is a built-in palette nobody replaced.
        EditorThemeData ResolveForApply(const EditorThemeData& registered, Density density,
                                        const std::optional<Appearance>& appearanceOverride, bool& isBuiltin)
        {
            const BuiltinPalette* palette = FindBuiltinPalette(registered.name);
            isBuiltin = palette != nullptr && !ShadowedBuiltinNames().contains(registered.name);
            if (!isBuiltin || (!appearanceOverride.has_value() && density == Density::Standard))
            {
                return registered;
            }
            return BuiltinTheme(*palette, appearanceOverride.value_or(palette->appearance), density);
        }
    } // namespace

    // ===================================================================
    // ThemeColor implementations
    // ===================================================================

    ImVec4 ThemeColor::ToImVec4() const
    {
        return ImVec4(r, g, b, a);
    }

    ThemeColor ThemeColor::FromRGB(int red, int green, int blue, int alpha)
    {
        return ThemeColor(red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
    }

    ThemeColor ThemeColor::FromHex(const std::string& hex)
    {
        std::string h = hex;
        if (!h.empty() && h[0] == '#')
            h = h.substr(1);

        unsigned int val = 0;
        std::istringstream iss(h);
        iss >> std::hex >> val;

        if (h.length() == 8)
        {
            return ThemeColor(((val >> 24) & 0xFF) / 255.0f, ((val >> 16) & 0xFF) / 255.0f,
                              ((val >> 8) & 0xFF) / 255.0f, (val & 0xFF) / 255.0f);
        }
        return ThemeColor(((val >> 16) & 0xFF) / 255.0f, ((val >> 8) & 0xFF) / 255.0f, (val & 0xFF) / 255.0f, 1.0f);
    }

    ThemeColor ThemeColor::Lerp(const ThemeColor& other, float t) const
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return ThemeColor(r + (other.r - r) * t, g + (other.g - g) * t, b + (other.b - b) * t, a + (other.a - a) * t);
    }

    ThemeColor ThemeColor::Darken(float amount) const
    {
        float factor = 1.0f - std::clamp(amount, 0.0f, 1.0f);
        return ThemeColor(r * factor, g * factor, b * factor, a);
    }

    ThemeColor ThemeColor::Lighten(float amount) const
    {
        float factor = std::clamp(amount, 0.0f, 1.0f);
        return ThemeColor(r + (1.0f - r) * factor, g + (1.0f - g) * factor, b + (1.0f - b) * factor, a);
    }

    ThemeColor ThemeColor::Desaturate(float amount) const
    {
        float gray = r * 0.299f + g * 0.587f + b * 0.114f;
        float t = std::clamp(amount, 0.0f, 1.0f);
        return ThemeColor(r + (gray - r) * t, g + (gray - g) * t, b + (gray - b) * t, a);
    }

    ThemeColor ThemeColor::WithAlpha(float alpha) const
    {
        return ThemeColor(r, g, b, alpha);
    }

    // ===================================================================
    // EditorTheme implementations
    // ===================================================================

    bool EditorTheme::ApplyTheme(const std::string& themeName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !themeName.empty(), false);
        if (s_registeredThemes.empty())
        {
            InitializeDefaultThemes();
        }

        const auto* theme = GetTheme(themeName);
        if (!theme)
        {
            std::cout << "[EditorTheme] Theme '" << themeName << "' not found, applying Ember Studio\n";
            theme = GetTheme("Ember Studio");
            if (!theme)
                return false;
        }

        bool isBuiltin = false;
        EditorThemeData resolved = ResolveForApply(*theme, s_density, s_appearanceOverride, isBuiltin);
        ApplyToImGui(resolved);
        s_currentThemeName = theme->name;
        s_currentThemeData = std::move(resolved);
        s_currentIsBuiltinPalette = isBuiltin;
        ++s_revision;
        std::cout << "[EditorTheme] Applied theme: " << s_currentThemeName << "\n";
        return true;
    }

    bool EditorTheme::ApplyTheme(const EditorThemeData& theme)
    {
        ApplyToImGui(theme);
        s_currentThemeData = theme;
        s_currentThemeName = theme.name;
        s_currentIsBuiltinPalette = false;
        ++s_revision;
        return true;
    }

    std::vector<std::string> EditorTheme::GetAvailableThemes()
    {
        if (s_registeredThemes.empty())
            InitializeDefaultThemes();
        std::vector<std::string> names;
        names.reserve(s_registeredThemes.size());
        for (const auto& [name, _] : s_registeredThemes)
        {
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    const EditorThemeData* EditorTheme::GetTheme(const std::string& themeName)
    {
        if (s_registeredThemes.empty())
            InitializeDefaultThemes();
        auto it = s_registeredThemes.find(themeName);
        // Explicit custom registrations win. Legacy built-in names remain valid
        // for launch settings without multiplying entries in the theme picker.
        if (it == s_registeredThemes.end())
        {
            static const std::unordered_map<std::string, std::string> aliases = {
                {"Spark Professional", "Ember Studio"}, {"Spark Ember", "Ember Studio"},
                {"Orange Accent", "Ember Studio"},      {"Spark Fusion", "Cobalt Forge"},
                {"Unreal Pro", "Cobalt Forge"},         {"Blue Accent", "Cobalt Forge"},
                {"Unity Pro", "Graphite Signal"},       {"VS Pro", "Graphite Signal"},
                {"JetBrains", "Graphite Signal"}};
            const auto alias = aliases.find(themeName);
            if (alias != aliases.end())
                it = s_registeredThemes.find(alias->second);
        }
        return (it != s_registeredThemes.end()) ? &it->second : nullptr;
    }

    bool EditorTheme::RegisterTheme(const EditorThemeData& theme)
    {
        InitializeDefaultThemes();
        if (FindBuiltinPalette(theme.name) != nullptr)
        {
            ShadowedBuiltinNames().insert(theme.name);
        }
        s_registeredThemes[theme.name] = theme;
        return true;
    }

    const std::string& EditorTheme::GetCurrentThemeName()
    {
        return s_currentThemeName;
    }

    const EditorThemeData& EditorTheme::GetCurrentThemeData()
    {
        return s_currentThemeData;
    }

    bool EditorTheme::CreateBlendedTheme(const std::string& theme1, const std::string& theme2, float blend,
                                         const std::string& resultName)
    {
        const EditorThemeData* first = GetTheme(theme1);
        const EditorThemeData* second = GetTheme(theme2);
        if (!first || !second)
            return false;

        const float t = std::isfinite(blend) ? std::clamp(blend, 0.0f, 1.0f) : 0.0f;
        // Flags, strings and enums cannot be interpolated; they come from the nearer theme.
        EditorThemeData blended = t < 0.5f ? *first : *second;
        blended.name = resultName;
        blended.description = "Blend of " + first->name + " and " + second->name;
        for (ThemeColor EditorThemeData::*field : kColorFields)
        {
            blended.*field = (first->*field).Lerp(second->*field, t);
        }
        for (std::size_t i = 0; i < blended.statusFills.size(); ++i)
        {
            blended.statusFills[i] = first->statusFills[i].Lerp(second->statusFills[i], t);
        }
        for (float EditorThemeData::*field : kMetricFields)
        {
            blended.*field = first->*field + (second->*field - first->*field) * t;
        }
        RegisterTheme(blended);
        return true;
    }

    void EditorTheme::ApplyProfessionalEnhancements()
    {
        s_enhancementsEnabled = true;
    }

    void EditorTheme::ApplyCustomFonts()
    {
        s_customFontsLoaded = true;
    }

    Spark::Ui::Tokens EditorTheme::ToTokens(const EditorThemeData& theme)
    {
        // Roles without an EditorThemeData field (axes, categories, modal scrim) come from the
        // audited Ember set of the theme's appearance; they are identical across palette families.
        Tokens t = Spark::Ui::GenerateTokens(Spark::Ui::BuiltinSeed(PaletteId::Ember), theme.appearance, theme.density);
        t.appearance = theme.appearance;
        const auto set = [&t](Role role, const ThemeColor& colour)
        { t.color[static_cast<std::size_t>(role)] = ToRgba(colour); };

        set(Role::SurfaceSunken, theme.backgroundDark);
        set(Role::SurfaceBase, theme.background);
        set(Role::SurfaceRaised, theme.backgroundLight);
        set(Role::SurfaceOverlay, theme.surfaceOverlay);
        set(Role::SurfaceHover, theme.backgroundHover);
        set(Role::SurfaceSelected, theme.backgroundSelected);
        set(Role::SurfaceSelectedUnfocused, theme.surfaceSelectedUnfocused);
        set(Role::BorderSubtle, theme.border);
        set(Role::BorderStrong, theme.borderStrong);
        set(Role::BorderFocus, theme.focus);
        set(Role::TextPrimary, theme.text);
        set(Role::TextSecondary, theme.textSecondary);
        set(Role::TextDisabled, theme.buttonDisabled);
        set(Role::TextOnAccent, theme.textOnAccent);
        set(Role::TextLink, theme.textAccent);
        set(Role::AccentFill, theme.accent);
        set(Role::AccentFillHover, theme.accentHover);
        set(Role::AccentFillActive, theme.accentActive);
        set(Role::AccentText, theme.accentText);
        set(Role::Secondary, theme.accentSecondary);
        set(Role::SuccessText, theme.textSuccess);
        set(Role::WarningText, theme.textWarning);
        set(Role::ErrorText, theme.textError);
        set(Role::InfoText, theme.textInfo);
        set(Role::InfoFill, StatusFill(theme, ThemeStatusFill::Info));
        set(Role::SuccessFill, StatusFill(theme, ThemeStatusFill::Success));
        set(Role::WarningFill, StatusFill(theme, ThemeStatusFill::Warning));
        set(Role::ErrorFill, StatusFill(theme, ThemeStatusFill::Error));
        set(Role::Graph1, theme.graph1);
        set(Role::Graph2, theme.graph2);
        set(Role::Graph3, theme.graph3);
        set(Role::Graph4, theme.graph4);
        set(Role::Graph5, theme.graph5);
        set(Role::DropTarget, theme.drop);
        set(Role::SelectionTextBg, theme.selection);

        Spark::Ui::Metrics& m = t.metrics;
        m.windowPadX = theme.windowPaddingX;
        m.windowPadY = theme.windowPaddingY;
        m.framePadX = theme.framePaddingX;
        m.framePadY = theme.framePaddingY;
        m.itemSpacingX = theme.itemSpacingX;
        m.itemSpacingY = theme.itemSpacingY;
        m.itemInnerX = theme.itemInnerSpacingX;
        m.itemInnerY = theme.itemInnerSpacingY;
        m.indent = theme.indentSpacing;
        m.scrollbar = theme.scrollbarSize;
        m.grabMin = theme.grabMinSize;
        m.rWindow = theme.windowRounding;
        m.rChild = theme.childRounding;
        m.rFrame = theme.frameRounding;
        m.rPopup = theme.popupRounding;
        m.rScrollbar = theme.scrollbarRounding;
        m.rGrab = theme.grabRounding;
        m.rTab = theme.tabRounding;
        m.bWindow = theme.windowBorderSize;
        m.bChild = theme.childBorderSize;
        m.bPopup = theme.popupBorderSize;
        m.bFrame = theme.frameBorderSize;
        m.focusRing = theme.focusRingWidth;

        // Every typographic role keeps its ratio to the body size the theme asks for.
        const float typeScale = SaneFactor(theme.fontSize, kBodyFontSize) / kBodyFontSize;
        Spark::Ui::Typography& type = t.type;
        type.caption *= typeScale;
        type.body *= typeScale;
        type.strong *= typeScale;
        type.h2 *= typeScale;
        type.h1 *= typeScale;
        type.display *= typeScale;
        type.mono *= typeScale;
        type.monoSmall *= typeScale;
        return t;
    }

    void EditorTheme::SetUiScale(float dpiScale, float userScale)
    {
        Spark::Ui::RequestScale(dpiScale, userScale);
    }

    float EditorTheme::GetUiScale()
    {
        return Spark::Ui::Scale();
    }

    bool EditorTheme::ApplyPendingScale()
    {
        if (!Spark::Ui::BeginFrameScale())
        {
            return false;
        }
        // BeginFrameScale rebuilt the style from the tokens; restore the per-field colour mapping.
        if (!s_currentThemeName.empty())
        {
            ApplyFieldColours(s_currentThemeData, ImGui::GetStyle());
        }
        return true;
    }

    void EditorTheme::SetDensity(Spark::Ui::Density density)
    {
        if (s_density == density)
        {
            return;
        }
        s_density = density;
        ReapplyCurrentTheme();
    }

    void EditorTheme::SetAppearanceOverride(std::optional<Spark::Ui::Appearance> appearance)
    {
        if (s_appearanceOverride == appearance)
        {
            return;
        }
        s_appearanceOverride = appearance;
        ReapplyCurrentTheme();
    }

    std::uint64_t EditorTheme::GetRevision()
    {
        return s_revision;
    }

    EditorTheme::NativeCaptionStyle EditorTheme::NativeCaptionStyleFor(const EditorThemeData& theme)
    {
        NativeCaptionStyle style;
        style.caption = theme.titleBarActive.WithAlpha(1.0f);
        style.text = theme.titleBarText.WithAlpha(1.0f);
        style.darkMode = Spark::Ui::RelativeLuminance(ToRgba(theme.background)) <= 0.5f;
        return style;
    }

    void EditorTheme::ReapplyCurrentTheme()
    {
        // Density and the appearance override only regenerate built-in palettes; a custom theme
        // stays exactly as it was applied.
        if (s_currentIsBuiltinPalette && !s_currentThemeName.empty())
        {
            ApplyTheme(s_currentThemeName);
        }
    }

    // ===================================================================
    // Core theme application — tokens first, then every field onto its slot
    // ===================================================================

    void EditorTheme::ApplyToImGui(const EditorThemeData& theme)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return;
        }
        // Rebuilds the whole style (metrics, colours by role, FontSizeBase) at the context's
        // current UI scale; the widget kit reads the same tokens.
        Spark::Ui::SetTokens(ToTokens(theme));
        ApplyFieldColours(theme, ImGui::GetStyle());
    }

    void EditorTheme::ApplyFieldColours(const EditorThemeData& theme, ImGuiStyle& style)
    {
        ImVec4* c = style.Colors;
        const auto set = [c](ImGuiCol slot, const ThemeColor& colour) { c[slot] = colour.ToImVec4(); };

        set(ImGuiCol_Text, theme.text);
        set(ImGuiCol_TextDisabled, theme.textDisabled);
        set(ImGuiCol_WindowBg, theme.background);
        set(ImGuiCol_PopupBg, theme.menuItem);
        set(ImGuiCol_Border, theme.borderStrong);
        set(ImGuiCol_FrameBg, theme.frame);
        set(ImGuiCol_FrameBgHovered, theme.frameHovered);
        set(ImGuiCol_FrameBgActive, theme.frameActive);
        set(ImGuiCol_TitleBg, theme.titleBar);
        set(ImGuiCol_TitleBgActive, theme.titleBarActive);
        set(ImGuiCol_TitleBgCollapsed, theme.titleBar);
        set(ImGuiCol_MenuBarBg, theme.menuBar);
        set(ImGuiCol_ScrollbarBg, theme.scrollbar);
        set(ImGuiCol_ScrollbarGrab, theme.scrollbarGrab);
        set(ImGuiCol_ScrollbarGrabHovered, theme.scrollbarGrabHovered);
        set(ImGuiCol_ScrollbarGrabActive, theme.scrollbarGrabActive);
        set(ImGuiCol_CheckMark, theme.accent);
        set(ImGuiCol_CheckboxSelectedBg, theme.backgroundSelected);
        set(ImGuiCol_SliderGrab, theme.accent);
        set(ImGuiCol_SliderGrabActive, theme.accentHover);
        set(ImGuiCol_Button, theme.button);
        set(ImGuiCol_ButtonHovered, theme.buttonHovered);
        set(ImGuiCol_ButtonActive, theme.buttonActive);
        set(ImGuiCol_Header, theme.backgroundHeader);
        set(ImGuiCol_HeaderHovered, theme.menuItemHovered);
        set(ImGuiCol_HeaderActive, theme.backgroundActive);
        set(ImGuiCol_Separator, theme.borderSeparator);
        set(ImGuiCol_SeparatorHovered, theme.borderLight);
        set(ImGuiCol_SeparatorActive, theme.borderAccent);
        set(ImGuiCol_ResizeGripHovered, theme.borderAccent);
        set(ImGuiCol_ResizeGripActive, theme.accentActive);
        set(ImGuiCol_InputTextCursor, theme.accent);
        set(ImGuiCol_TabHovered, theme.tabHovered);
        set(ImGuiCol_Tab, theme.tab);
        set(ImGuiCol_TabSelected, theme.tabActive);
        set(ImGuiCol_TabSelectedOverline, theme.accent);
        set(ImGuiCol_TabDimmed, theme.tabUnfocused);
        set(ImGuiCol_TabDimmedSelected, theme.tabActive);
        set(ImGuiCol_TabDimmedSelectedOverline, theme.borderStrong);
        set(ImGuiCol_DockingPreview, theme.drop);
        set(ImGuiCol_DockingEmptyBg, theme.backgroundDark);
        set(ImGuiCol_PlotLines, theme.accentSecondary);
        set(ImGuiCol_PlotLinesHovered, theme.accentHover);
        set(ImGuiCol_PlotHistogram, theme.graph1);
        set(ImGuiCol_PlotHistogramHovered, theme.accentHover);
        set(ImGuiCol_TableHeaderBg, theme.backgroundHeader);
        set(ImGuiCol_TableBorderStrong, theme.borderStrong);
        set(ImGuiCol_TableBorderLight, theme.border);
        set(ImGuiCol_TableRowBgAlt, theme.backgroundLight.WithAlpha(theme.backgroundLight.a * kTableRowAltAlpha));
        set(ImGuiCol_TextLink, theme.textAccent);
        set(ImGuiCol_TextSelectedBg, theme.selection);
        set(ImGuiCol_TreeLines, theme.border);
        set(ImGuiCol_DragDropTarget, theme.drop);
        set(ImGuiCol_DragDropTargetBg, theme.backgroundAccent);
        set(ImGuiCol_UnsavedMarker, theme.accent);
        set(ImGuiCol_NavCursor, theme.focus);
        set(ImGuiCol_NavWindowingHighlight, theme.focus);
        // ChildBg, BorderShadow, ResizeGrip, TableRowBg and the two dim backgrounds keep the token
        // mapping (transparent or ModalScrim); no EditorThemeData field describes them.

        // style.FontScaleMain = user scale x theme font scale, recomputed (never accumulated).
        style.FontScaleMain = Spark::Ui::Scale() / Spark::Ui::DpiScale() * SaneFactor(theme.fontScale, 1.0f);

        // Smooth rendering: anti-aliased lines and fills, finer curve and circle tessellation.
        style.AntiAliasedLines = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill = true;
        style.CurveTessellationTol = 1.0f;
        style.CircleTessellationMaxError = 0.10f;
    }

    // ===================================================================
    // Predefined Themes
    // ===================================================================

    void EditorTheme::InitializeDefaultThemes()
    {
        // Registration is also a public entry point. Initialize built-ins before
        // accepting custom palettes, without recursing through RegisterTheme.
        static bool initialized = false;
        if (initialized)
            return;
        initialized = true;
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing default editor themes");
        for (const BuiltinPalette& palette : kBuiltinPalettes)
        {
            s_registeredThemes[palette.name] = BuiltinTheme(palette, palette.appearance, Density::Standard);
        }
    }

    // -------------------------------------------------------------------
    // SPARK PROFESSIONAL — retired teal palette, still the base of the
    // Blue Accent and Orange Accent factories.
    // -------------------------------------------------------------------
    static EditorThemeData CreateSparkThemeImpl()
    {
        EditorThemeData t;
        t.name = "Spark Professional";
        t.description = "Sleek dark theme with teal-cyan accents and warm amber highlights";
        t.author = "Spark Engine Team";

        // Backgrounds — deep charcoal with subtle warm undertone (not blue-cold)
        t.background = ThemeColor::FromHex("#1B1D22");
        t.backgroundDark = ThemeColor::FromHex("#131518");
        t.backgroundLight = ThemeColor::FromHex("#242730");
        t.backgroundAccent = ThemeColor::FromHex("#0E3D4A");
        t.backgroundHeader = ThemeColor::FromHex("#1E2028");
        t.backgroundActive = ThemeColor::FromHex("#1AAFBC").WithAlpha(0.22f);
        t.backgroundHover = ThemeColor::FromHex("#282C36");
        t.backgroundSelected = ThemeColor::FromHex("#1AAFBC").WithAlpha(0.15f);

        // Text — high-contrast cool white, warm disabled tones
        t.text = ThemeColor::FromHex("#D8DCE6");
        t.textDisabled = ThemeColor::FromHex("#4E5462");
        t.textSecondary = ThemeColor::FromHex("#8890A0");
        t.textAccent = ThemeColor::FromHex("#36C8D6");
        t.textWarning = ThemeColor::FromHex("#F0A830");
        t.textError = ThemeColor::FromHex("#E84040");
        t.textSuccess = ThemeColor::FromHex("#3DD68C");

        // Buttons — slightly raised from background, not flat
        t.button = ThemeColor::FromHex("#282C36");
        t.buttonHovered = ThemeColor::FromHex("#323844");
        t.buttonActive = ThemeColor::FromHex("#1AAFBC");
        t.buttonDisabled = ThemeColor::FromHex("#1E2028");

        // Frames (input fields) — recessed, darker than background
        t.frame = ThemeColor::FromHex("#161820");
        t.frameHovered = ThemeColor::FromHex("#1E222C");
        t.frameActive = ThemeColor::FromHex("#1AAFBC").WithAlpha(0.35f);

        // Borders — barely visible on idle, accent on focus
        t.border = ThemeColor::FromHex("#2A2E3A");
        t.borderLight = ThemeColor::FromHex("#353A48");
        t.borderAccent = ThemeColor::FromHex("#1AAFBC");
        t.borderSeparator = ThemeColor::FromHex("#22252E");

        // Title bar — near-black, minimal contrast with background
        t.titleBar = ThemeColor::FromHex("#131518");
        t.titleBarActive = ThemeColor::FromHex("#0E3D4A");
        t.titleBarText = ThemeColor::FromHex("#D8DCE6");

        // Menu bar — seamless with main background
        t.menuBar = ThemeColor::FromHex("#1B1D22");
        t.menuItemHovered = ThemeColor::FromHex("#1AAFBC").WithAlpha(0.35f);

        // Scrollbar — thin, subtle, only visible on hover
        t.scrollbar = ThemeColor::FromHex("#131518");
        t.scrollbarGrab = ThemeColor::FromHex("#363C4A");
        t.scrollbarGrabHovered = ThemeColor::FromHex("#4A5266");
        t.scrollbarGrabActive = ThemeColor::FromHex("#1AAFBC");

        // Tabs — active tab has accent underline feel
        t.tab = ThemeColor::FromHex("#1B1D22");
        t.tabHovered = ThemeColor::FromHex("#1AAFBC").WithAlpha(0.28f);
        t.tabActive = ThemeColor::FromHex("#242730");
        t.tabUnfocused = ThemeColor::FromHex("#131518");

        // Accent colors — teal-cyan primary, warm amber secondary
        t.accent = ThemeColor::FromHex("#1AAFBC");
        t.accentSecondary = ThemeColor::FromHex("#F0A830");
        t.focus = ThemeColor::FromHex("#1AAFBC");
        t.selection = ThemeColor::FromHex("#1AAFBC").WithAlpha(0.25f);
        t.drop = ThemeColor::FromHex("#F0A830").WithAlpha(0.75f);

        // Graph colors — distinguishable, vibrant on dark
        t.graph1 = ThemeColor::FromHex("#1AAFBC");
        t.graph2 = ThemeColor::FromHex("#F0A830");
        t.graph3 = ThemeColor::FromHex("#3DD68C");
        t.graph4 = ThemeColor::FromHex("#E84040");
        t.graph5 = ThemeColor::FromHex("#A86EDB");

        // Style values — slightly more rounded than before for modern feel
        t.windowRounding = 4.0f;
        t.childRounding = 4.0f;
        t.frameRounding = 4.0f;
        t.popupRounding = 6.0f;
        t.scrollbarRounding = 12.0f;
        t.grabRounding = 4.0f;
        t.tabRounding = 4.0f;

        t.windowBorderSize = 1.0f;
        t.childBorderSize = 1.0f;
        t.popupBorderSize = 1.0f;
        t.frameBorderSize = 0.0f;

        t.windowPaddingX = 10.0f;
        t.windowPaddingY = 10.0f;
        t.framePaddingX = 8.0f;
        t.framePaddingY = 5.0f;
        t.itemSpacingX = 8.0f;
        t.itemSpacingY = 5.0f;
        t.itemInnerSpacingX = 5.0f;
        t.itemInnerSpacingY = 5.0f;

        t.indentSpacing = 20.0f;
        t.scrollbarSize = 12.0f;
        t.grabMinSize = 10.0f;

        t.fontSize = 15.0f;
        t.fontScale = 1.0f;
        t.fontFamily = "Roboto";

        return t;
    }
    EditorThemeData EditorTheme::CreateUnityProTheme()
    {
        EditorThemeData t;
        t.name = "Unity Pro";
        t.description = "Unity-inspired professional dark theme";
        t.author = "Spark Engine Team";

        t.background = ThemeColor::FromRGB(56, 56, 56);
        t.backgroundDark = ThemeColor::FromRGB(48, 48, 48);
        t.backgroundLight = ThemeColor::FromRGB(70, 70, 70);
        t.backgroundAccent = ThemeColor::FromRGB(62, 95, 150);
        t.backgroundHeader = ThemeColor::FromRGB(60, 60, 60);
        t.backgroundActive = ThemeColor::FromRGB(62, 95, 150, 128);
        t.backgroundHover = ThemeColor::FromRGB(75, 75, 75);
        t.backgroundSelected = ThemeColor::FromRGB(62, 95, 150, 100);

        t.text = ThemeColor::FromRGB(210, 210, 210);
        t.textDisabled = ThemeColor::FromRGB(128, 128, 128);
        t.textSecondary = ThemeColor::FromRGB(170, 170, 170);
        t.textAccent = ThemeColor::FromRGB(62, 125, 200);
        t.textWarning = ThemeColor::FromRGB(230, 180, 50);
        t.textError = ThemeColor::FromRGB(200, 60, 60);
        t.textSuccess = ThemeColor::FromRGB(60, 180, 80);

        t.button = ThemeColor::FromRGB(72, 72, 72);
        t.buttonHovered = ThemeColor::FromRGB(90, 90, 90);
        t.buttonActive = ThemeColor::FromRGB(62, 95, 150);
        t.buttonDisabled = ThemeColor::FromRGB(56, 56, 56);

        t.frame = ThemeColor::FromRGB(42, 42, 42);
        t.frameHovered = ThemeColor::FromRGB(52, 52, 52);
        t.frameActive = ThemeColor::FromRGB(62, 95, 150, 120);

        t.border = ThemeColor::FromRGB(35, 35, 35);
        t.borderLight = ThemeColor::FromRGB(65, 65, 65);
        t.borderAccent = ThemeColor::FromRGB(62, 95, 150);
        t.borderSeparator = ThemeColor::FromRGB(40, 40, 40);

        t.titleBar = ThemeColor::FromRGB(48, 48, 48);
        t.titleBarActive = ThemeColor::FromRGB(50, 50, 50);
        t.titleBarText = ThemeColor::FromRGB(200, 200, 200);

        t.menuBar = ThemeColor::FromRGB(56, 56, 56);
        t.menuItemHovered = ThemeColor::FromRGB(62, 95, 150, 180);

        t.scrollbar = ThemeColor::FromRGB(40, 40, 40);
        t.scrollbarGrab = ThemeColor::FromRGB(80, 80, 80);
        t.scrollbarGrabHovered = ThemeColor::FromRGB(100, 100, 100);
        t.scrollbarGrabActive = ThemeColor::FromRGB(120, 120, 120);

        t.tab = ThemeColor::FromRGB(48, 48, 48);
        t.tabHovered = ThemeColor::FromRGB(70, 70, 70);
        t.tabActive = ThemeColor::FromRGB(56, 56, 56);
        t.tabUnfocused = ThemeColor::FromRGB(42, 42, 42);

        t.accent = ThemeColor::FromRGB(62, 125, 200);
        t.accentSecondary = ThemeColor::FromRGB(62, 95, 150);
        t.focus = ThemeColor::FromRGB(62, 125, 200);
        t.selection = ThemeColor::FromRGB(62, 95, 150, 100);
        t.drop = ThemeColor::FromRGB(62, 125, 200, 200);

        t.graph1 = ThemeColor::FromRGB(62, 125, 200);
        t.graph2 = ThemeColor::FromRGB(230, 180, 50);
        t.graph3 = ThemeColor::FromRGB(60, 180, 80);
        t.graph4 = ThemeColor::FromRGB(200, 60, 60);
        t.graph5 = ThemeColor::FromRGB(150, 80, 200);

        t.windowRounding = 0.0f;
        t.frameRounding = 2.0f;
        t.tabRounding = 2.0f;
        t.frameBorderSize = 0.0f;

        CompleteLegacyFields(t);
        return t;
    }

    EditorThemeData EditorTheme::CreateUnrealProTheme()
    {
        EditorThemeData t;
        t.name = "Unreal Pro";
        t.description = "Unreal Engine-inspired dark theme";
        t.author = "Spark Engine Team";

        t.background = ThemeColor::FromRGB(36, 36, 36);
        t.backgroundDark = ThemeColor::FromRGB(24, 24, 24);
        t.backgroundLight = ThemeColor::FromRGB(50, 50, 50);
        t.backgroundAccent = ThemeColor::FromRGB(0, 90, 180);
        t.backgroundHeader = ThemeColor::FromRGB(30, 30, 30);
        t.backgroundActive = ThemeColor::FromRGB(0, 90, 180, 128);
        t.backgroundHover = ThemeColor::FromRGB(55, 55, 55);
        t.backgroundSelected = ThemeColor::FromRGB(0, 90, 180, 80);

        t.text = ThemeColor::FromRGB(220, 220, 220);
        t.textDisabled = ThemeColor::FromRGB(100, 100, 100);
        t.textSecondary = ThemeColor::FromRGB(170, 170, 170);
        t.textAccent = ThemeColor::FromRGB(0, 140, 220);
        t.textWarning = ThemeColor::FromRGB(240, 200, 50);
        t.textError = ThemeColor::FromRGB(220, 50, 50);
        t.textSuccess = ThemeColor::FromRGB(50, 200, 70);

        t.button = ThemeColor::FromRGB(50, 50, 50);
        t.buttonHovered = ThemeColor::FromRGB(65, 65, 65);
        t.buttonActive = ThemeColor::FromRGB(0, 90, 180);
        t.buttonDisabled = ThemeColor::FromRGB(40, 40, 40);

        t.frame = ThemeColor::FromRGB(20, 20, 20);
        t.frameHovered = ThemeColor::FromRGB(30, 30, 30);
        t.frameActive = ThemeColor::FromRGB(0, 90, 180, 120);

        t.border = ThemeColor::FromRGB(10, 10, 10);
        t.borderLight = ThemeColor::FromRGB(50, 50, 50);
        t.borderAccent = ThemeColor::FromRGB(0, 90, 180);
        t.borderSeparator = ThemeColor::FromRGB(15, 15, 15);

        t.titleBar = ThemeColor::FromRGB(24, 24, 24);
        t.titleBarActive = ThemeColor::FromRGB(0, 60, 120);
        t.titleBarText = ThemeColor::FromRGB(200, 200, 200);

        t.menuBar = ThemeColor::FromRGB(36, 36, 36);
        t.menuItemHovered = ThemeColor::FromRGB(0, 90, 180, 180);

        t.scrollbar = ThemeColor::FromRGB(20, 20, 20);
        t.scrollbarGrab = ThemeColor::FromRGB(60, 60, 60);
        t.scrollbarGrabHovered = ThemeColor::FromRGB(80, 80, 80);
        t.scrollbarGrabActive = ThemeColor::FromRGB(100, 100, 100);

        t.tab = ThemeColor::FromRGB(24, 24, 24);
        t.tabHovered = ThemeColor::FromRGB(50, 50, 50);
        t.tabActive = ThemeColor::FromRGB(36, 36, 36);
        t.tabUnfocused = ThemeColor::FromRGB(18, 18, 18);

        t.accent = ThemeColor::FromRGB(0, 120, 215);
        t.accentSecondary = ThemeColor::FromRGB(0, 90, 180);
        t.focus = ThemeColor::FromRGB(0, 120, 215);
        t.selection = ThemeColor::FromRGB(0, 90, 180, 100);
        t.drop = ThemeColor::FromRGB(0, 120, 215, 200);

        t.graph1 = ThemeColor::FromRGB(0, 120, 215);
        t.graph2 = ThemeColor::FromRGB(240, 200, 50);
        t.graph3 = ThemeColor::FromRGB(50, 200, 70);
        t.graph4 = ThemeColor::FromRGB(220, 50, 50);
        t.graph5 = ThemeColor::FromRGB(140, 70, 210);

        t.windowRounding = 0.0f;
        t.frameRounding = 1.0f;
        t.tabRounding = 0.0f;
        t.frameBorderSize = 1.0f;

        CompleteLegacyFields(t);
        return t;
    }

    EditorThemeData EditorTheme::CreateVSProTheme()
    {
        EditorThemeData t;
        t.name = "VS Pro";
        t.description = "Visual Studio-inspired dark theme";

        t.background = ThemeColor::FromRGB(30, 30, 30);
        t.backgroundDark = ThemeColor::FromRGB(25, 25, 25);
        t.backgroundLight = ThemeColor::FromRGB(45, 45, 48);
        t.backgroundAccent = ThemeColor::FromRGB(0, 122, 204);
        t.backgroundHeader = ThemeColor::FromRGB(37, 37, 38);
        t.backgroundActive = ThemeColor::FromRGB(0, 122, 204, 120);
        t.backgroundHover = ThemeColor::FromRGB(51, 51, 52);
        t.backgroundSelected = ThemeColor::FromRGB(0, 122, 204, 80);

        t.text = ThemeColor::FromRGB(220, 220, 220);
        t.textDisabled = ThemeColor::FromRGB(110, 110, 110);
        t.textSecondary = ThemeColor::FromRGB(160, 160, 160);
        t.textAccent = ThemeColor::FromRGB(0, 122, 204);
        t.textWarning = ThemeColor::FromRGB(230, 180, 50);
        t.textError = ThemeColor::FromRGB(210, 50, 50);
        t.textSuccess = ThemeColor::FromRGB(50, 180, 70);

        t.button = ThemeColor::FromRGB(51, 51, 55);
        t.buttonHovered = ThemeColor::FromRGB(63, 63, 70);
        t.buttonActive = ThemeColor::FromRGB(0, 122, 204);
        t.buttonDisabled = ThemeColor::FromRGB(40, 40, 42);

        t.frame = ThemeColor::FromRGB(51, 51, 55);
        t.frameHovered = ThemeColor::FromRGB(63, 63, 70);
        t.frameActive = ThemeColor::FromRGB(0, 122, 204, 120);

        t.border = ThemeColor::FromRGB(45, 45, 48);
        t.borderLight = ThemeColor::FromRGB(63, 63, 70);
        t.borderAccent = ThemeColor::FromRGB(0, 122, 204);
        t.borderSeparator = ThemeColor::FromRGB(45, 45, 48);

        t.titleBar = ThemeColor::FromRGB(45, 45, 48);
        t.titleBarActive = ThemeColor::FromRGB(0, 122, 204);
        t.titleBarText = ThemeColor::FromRGB(220, 220, 220);
        t.menuBar = ThemeColor::FromRGB(45, 45, 48);
        t.menuItemHovered = ThemeColor::FromRGB(0, 122, 204, 180);

        t.scrollbar = ThemeColor::FromRGB(30, 30, 30);
        t.scrollbarGrab = ThemeColor::FromRGB(80, 80, 80);
        t.scrollbarGrabHovered = ThemeColor::FromRGB(100, 100, 100);
        t.scrollbarGrabActive = ThemeColor::FromRGB(120, 120, 120);

        t.tab = ThemeColor::FromRGB(45, 45, 48);
        t.tabHovered = ThemeColor::FromRGB(28, 151, 234);
        t.tabActive = ThemeColor::FromRGB(0, 122, 204);
        t.tabUnfocused = ThemeColor::FromRGB(37, 37, 38);

        t.accent = ThemeColor::FromRGB(0, 122, 204);
        t.accentSecondary = ThemeColor::FromRGB(28, 151, 234);
        t.focus = ThemeColor::FromRGB(0, 122, 204);
        t.selection = ThemeColor::FromRGB(0, 122, 204, 100);
        t.drop = ThemeColor::FromRGB(0, 122, 204, 200);
        t.graph1 = t.accent;
        t.graph2 = t.accentSecondary;
        t.graph3 = t.textSuccess;
        t.graph4 = t.textError;
        t.graph5 = ThemeColor::FromRGB(140, 70, 210);

        t.windowRounding = 0.0f;
        t.frameRounding = 0.0f;
        t.tabRounding = 0.0f;
        t.frameBorderSize = 1.0f;
        CompleteLegacyFields(t);
        return t;
    }

    EditorThemeData EditorTheme::CreateJetBrainsTheme()
    {
        EditorThemeData t;
        t.name = "JetBrains";
        t.description = "JetBrains-inspired dark theme";

        t.background = ThemeColor::FromRGB(43, 43, 43);
        t.backgroundDark = ThemeColor::FromRGB(30, 30, 30);
        t.backgroundLight = ThemeColor::FromRGB(60, 63, 65);
        t.backgroundAccent = ThemeColor::FromRGB(75, 110, 175);
        t.backgroundHeader = ThemeColor::FromRGB(49, 51, 53);
        t.backgroundActive = ThemeColor::FromRGB(75, 110, 175, 128);
        t.backgroundHover = ThemeColor::FromRGB(69, 73, 74);
        t.backgroundSelected = ThemeColor::FromRGB(75, 110, 175, 80);

        t.text = ThemeColor::FromRGB(187, 187, 187);
        t.textDisabled = ThemeColor::FromRGB(100, 100, 100);
        t.textSecondary = ThemeColor::FromRGB(150, 150, 150);
        t.textAccent = ThemeColor::FromRGB(104, 151, 187);
        t.textWarning = ThemeColor::FromRGB(187, 181, 41);
        t.textError = ThemeColor::FromRGB(188, 63, 60);
        t.textSuccess = ThemeColor::FromRGB(106, 135, 89);

        t.button = ThemeColor::FromRGB(60, 63, 65);
        t.buttonHovered = ThemeColor::FromRGB(75, 78, 80);
        t.buttonActive = ThemeColor::FromRGB(75, 110, 175);
        t.buttonDisabled = ThemeColor::FromRGB(50, 52, 54);
        t.frame = ThemeColor::FromRGB(69, 73, 74);
        t.frameHovered = ThemeColor::FromRGB(80, 84, 85);
        t.frameActive = ThemeColor::FromRGB(75, 110, 175, 120);
        t.border = ThemeColor::FromRGB(50, 50, 50);
        t.borderLight = ThemeColor::FromRGB(70, 70, 70);
        t.borderAccent = ThemeColor::FromRGB(75, 110, 175);
        t.borderSeparator = ThemeColor::FromRGB(50, 50, 50);
        t.titleBar = ThemeColor::FromRGB(60, 63, 65);
        t.titleBarActive = ThemeColor::FromRGB(75, 110, 175);
        t.titleBarText = ThemeColor::FromRGB(187, 187, 187);
        t.menuBar = ThemeColor::FromRGB(43, 43, 43);
        t.menuItemHovered = ThemeColor::FromRGB(75, 110, 175, 180);
        t.scrollbar = ThemeColor::FromRGB(43, 43, 43);
        t.scrollbarGrab = ThemeColor::FromRGB(80, 80, 80);
        t.scrollbarGrabHovered = ThemeColor::FromRGB(100, 100, 100);
        t.scrollbarGrabActive = ThemeColor::FromRGB(120, 120, 120);
        t.tab = ThemeColor::FromRGB(43, 43, 43);
        t.tabHovered = ThemeColor::FromRGB(60, 63, 65);
        t.tabActive = ThemeColor::FromRGB(49, 51, 53);
        t.tabUnfocused = ThemeColor::FromRGB(38, 38, 38);
        t.accent = ThemeColor::FromRGB(75, 110, 175);
        t.accentSecondary = ThemeColor::FromRGB(104, 151, 187);
        t.focus = ThemeColor::FromRGB(75, 110, 175);
        t.selection = ThemeColor::FromRGB(33, 66, 131, 120);
        t.drop = ThemeColor::FromRGB(75, 110, 175, 200);
        t.graph1 = t.accent;
        t.graph2 = t.textWarning;
        t.graph3 = t.textSuccess;
        t.graph4 = t.textError;
        t.graph5 = ThemeColor::FromRGB(140, 70, 210);

        t.windowRounding = 4.0f;
        t.frameRounding = 3.0f;
        t.tabRounding = 4.0f;
        t.frameBorderSize = 0.0f;
        CompleteLegacyFields(t);
        return t;
    }

    // -------------------------------------------------------------------
    // Token-generated built-ins and retired accent variants
    // -------------------------------------------------------------------

    EditorThemeData EditorTheme::CreateProfessionalLightTheme()
    {
        return BuiltinTheme(kBuiltinPalettes[3], Appearance::Light, Density::Standard);
    }

    EditorThemeData EditorTheme::CreateHighContrastTheme()
    {
        return BuiltinTheme(kBuiltinPalettes[4], Appearance::HighContrast, Density::Standard);
    }

    EditorThemeData EditorTheme::CreateBlueAccentTheme()
    {
        EditorThemeData t = CreateSparkThemeImpl();
        t.name = "Blue Accent";
        t.description = "Dark theme with bold blue accent";
        t.accent = ThemeColor::FromRGB(30, 136, 229);
        t.accentSecondary = ThemeColor::FromRGB(66, 165, 245);
        t.tabActive = t.accent;
        t.titleBarActive = t.accent.Darken(0.3f);
        t.buttonActive = t.accent;
        CompleteLegacyFields(t);
        return t;
    }

    EditorThemeData EditorTheme::CreateOrangeAccentTheme()
    {
        EditorThemeData t = CreateSparkThemeImpl();
        t.name = "Orange Accent";
        t.description = "Dark theme with warm orange accent";
        t.accent = ThemeColor::FromRGB(245, 166, 35);
        t.accentSecondary = ThemeColor::FromRGB(45, 140, 240);
        t.tabActive = t.accent;
        t.titleBarActive = t.accent.Darken(0.4f);
        t.buttonActive = t.accent;
        t.textAccent = t.accent;
        CompleteLegacyFields(t);
        return t;
    }

    // ===================================================================
    // ThemeCustomizer implementations
    // ===================================================================

    void ThemeCustomizer::ShowThemeEditor()
    {
        if (!ImGui::Begin("Theme Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }

        // Get current theme
        auto currentThemeName = EditorTheme::GetCurrentThemeName();
        ImGui::Text("Current Theme: %s", currentThemeName.c_str());
        ImGui::Separator();

        const EditorThemeData* themePtr = EditorTheme::GetTheme(currentThemeName);
        if (!themePtr)
        {
            ImGui::Text("No theme loaded.");
            ImGui::End();
            return;
        }

        // Work on a mutable copy
        static EditorThemeData editTheme;
        static bool initialized = false;
        if (!initialized)
        {
            editTheme = *themePtr;
            initialized = true;
        }

        EditorThemeData& theme = editTheme;
        bool changed = false;

        // Theme metadata
        if (ImGui::CollapsingHeader("Theme Info", ImGuiTreeNodeFlags_DefaultOpen))
        {
            char nameBuf[256];
            strncpy(nameBuf, theme.name.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            {
                theme.name = nameBuf;
                changed = true;
            }
        }

        // Background colors
        if (ImGui::CollapsingHeader("Background Colors"))
        {
            auto editColor = [&](const char* label, ThemeColor& color)
            {
                float c[4] = {color.r, color.g, color.b, color.a};
                if (ImGui::ColorEdit4(label, c))
                {
                    color = ThemeColor(c[0], c[1], c[2], c[3]);
                    changed = true;
                }
            };
            editColor("Background", theme.background);
            editColor("Background Dark", theme.backgroundDark);
            editColor("Background Light", theme.backgroundLight);
            editColor("Background Accent", theme.backgroundAccent);
            editColor("Background Header", theme.backgroundHeader);
            editColor("Background Active", theme.backgroundActive);
            editColor("Background Hover", theme.backgroundHover);
            editColor("Background Selected", theme.backgroundSelected);
        }

        // Text colors
        if (ImGui::CollapsingHeader("Text Colors"))
        {
            auto editColor = [&](const char* label, ThemeColor& color)
            {
                float c[4] = {color.r, color.g, color.b, color.a};
                if (ImGui::ColorEdit4(label, c))
                {
                    color = ThemeColor(c[0], c[1], c[2], c[3]);
                    changed = true;
                }
            };
            editColor("Text", theme.text);
            editColor("Text Disabled", theme.textDisabled);
            editColor("Text Secondary", theme.textSecondary);
            editColor("Text Accent", theme.textAccent);
            editColor("Text Warning", theme.textWarning);
            editColor("Text Error", theme.textError);
            editColor("Text Success", theme.textSuccess);
        }

        // UI Element colors
        if (ImGui::CollapsingHeader("UI Elements"))
        {
            auto editColor = [&](const char* label, ThemeColor& color)
            {
                float c[4] = {color.r, color.g, color.b, color.a};
                if (ImGui::ColorEdit4(label, c))
                {
                    color = ThemeColor(c[0], c[1], c[2], c[3]);
                    changed = true;
                }
            };
            editColor("Button", theme.button);
            editColor("Button Hovered", theme.buttonHovered);
            editColor("Button Active", theme.buttonActive);
            editColor("Frame", theme.frame);
            editColor("Frame Hovered", theme.frameHovered);
            editColor("Frame Active", theme.frameActive);
            editColor("Border", theme.border);
            editColor("Border Light", theme.borderLight);
        }

        ImGui::Separator();

        if (changed)
        {
            EditorTheme::ApplyTheme(theme);
        }

        // Actions
        if (ImGui::Button("Apply Theme"))
        {
            EditorTheme::ApplyTheme(theme);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to Default"))
        {
            EditorTheme::ApplyTheme("Ember Studio");
            const EditorThemeData* resetTheme = EditorTheme::GetTheme("Ember Studio");
            if (resetTheme)
                editTheme = *resetTheme;
        }

        ImGui::End();
    }

    bool ThemeCustomizer::ExportTheme(const EditorThemeData& theme, const std::string& filepath,
                                      Spark::LocalFileCache* cache)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !filepath.empty(), false);
        try
        {
            std::ofstream file(filepath);
            if (!file.is_open())
                return false;

            file << WriteThemeDocument(theme);
            file.close();

            if (cache)
            {
                cache->Invalidate(filepath);
            }

            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to export theme to '%s': %s", filepath.c_str(),
                            e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to export theme to '%s': unknown exception",
                            filepath.c_str());
            return false;
        }
    }

    bool ThemeCustomizer::ImportTheme(const std::string& filepath, EditorThemeData& outTheme,
                                      [[maybe_unused]] Spark::LocalFileCache* cache)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !filepath.empty(), false);
        try
        {
            // Read through one opened handle, bounded: the file (or the cache's unbounded read
            // of it) used to be read whole whatever its size.
            std::string content;
            if (ReadRegularFileBounded(std::filesystem::path(filepath), kMaxThemeDocumentBytes, content) !=
                BoundedReadStatus::Ok)
            {
                return false;
            }
            return ParseThemeDocument(content, outTheme);
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to import theme from '%s': %s", filepath.c_str(),
                            e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to import theme from '%s': unknown exception",
                            filepath.c_str());
            return false;
        }
    }

    std::vector<EditorThemeData> ThemeCustomizer::GenerateThemeVariations(const EditorThemeData& baseTheme)
    {
        std::vector<EditorThemeData> variations;

        // Warm variation - shift toward orange/warm tones
        {
            EditorThemeData warm = baseTheme;
            warm.name = baseTheme.name + " (Warm)";
            warm.description = "Warm variation of " + baseTheme.name;
            auto warmShift = [](ThemeColor c)
            {
                c.r = std::min(1.0f, c.r * 1.1f);
                c.b = c.b * 0.9f;
                return c;
            };
            warm.background = warmShift(warm.background);
            warm.backgroundDark = warmShift(warm.backgroundDark);
            warm.backgroundLight = warmShift(warm.backgroundLight);
            warm.backgroundAccent = warmShift(warm.backgroundAccent);
            warm.backgroundHeader = warmShift(warm.backgroundHeader);
            variations.push_back(warm);
        }

        // Cool variation - shift toward blue/cool tones
        {
            EditorThemeData cool = baseTheme;
            cool.name = baseTheme.name + " (Cool)";
            cool.description = "Cool variation of " + baseTheme.name;
            auto coolShift = [](ThemeColor c)
            {
                c.r = c.r * 0.9f;
                c.b = std::min(1.0f, c.b * 1.1f);
                return c;
            };
            cool.background = coolShift(cool.background);
            cool.backgroundDark = coolShift(cool.backgroundDark);
            cool.backgroundLight = coolShift(cool.backgroundLight);
            cool.backgroundAccent = coolShift(cool.backgroundAccent);
            cool.backgroundHeader = coolShift(cool.backgroundHeader);
            variations.push_back(cool);
        }

        // High contrast variation
        {
            EditorThemeData highContrast = baseTheme;
            highContrast.name = baseTheme.name + " (High Contrast)";
            highContrast.description = "High contrast variation of " + baseTheme.name;
            highContrast.background = highContrast.background.Darken(0.3f);
            highContrast.backgroundDark = highContrast.backgroundDark.Darken(0.3f);
            highContrast.text = highContrast.text.Lighten(0.3f);
            highContrast.textSecondary = highContrast.textSecondary.Lighten(0.2f);
            highContrast.border = highContrast.border.Lighten(0.3f);
            highContrast.borderLight = highContrast.borderLight.Lighten(0.3f);
            variations.push_back(highContrast);
        }

        // Muted variation - desaturate all colors
        {
            EditorThemeData muted = baseTheme;
            muted.name = baseTheme.name + " (Muted)";
            muted.description = "Muted variation of " + baseTheme.name;
            muted.backgroundAccent = muted.backgroundAccent.Desaturate(0.4f);
            muted.textAccent = muted.textAccent.Desaturate(0.3f);
            muted.button = muted.button.Desaturate(0.3f);
            muted.buttonHovered = muted.buttonHovered.Desaturate(0.3f);
            muted.buttonActive = muted.buttonActive.Desaturate(0.3f);
            muted.borderAccent = muted.borderAccent.Desaturate(0.4f);
            variations.push_back(muted);
        }

        return variations;
    }

} // namespace SparkEditor
