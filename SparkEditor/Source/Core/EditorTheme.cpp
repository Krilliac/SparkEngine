/**
 * @file EditorTheme.cpp
 * @brief Theme system implementation for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorTheme.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstring>

namespace SparkEditor
{

    // Static member initialization
    std::unordered_map<std::string, EditorThemeData> EditorTheme::s_registeredThemes;
    std::string EditorTheme::s_currentThemeName;
    bool EditorTheme::s_enhancementsEnabled = false;
    bool EditorTheme::s_customFontsLoaded = false;

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

        auto it = s_registeredThemes.find(themeName);
        if (it == s_registeredThemes.end())
        {
            std::cout << "[EditorTheme] Theme '" << themeName << "' not found, applying Spark Professional\n";
            it = s_registeredThemes.find("Spark Professional");
            if (it == s_registeredThemes.end())
                return false;
        }

        ApplyToImGui(it->second);
        s_currentThemeName = it->first;
        std::cout << "[EditorTheme] Applied theme: " << s_currentThemeName << "\n";
        return true;
    }

    bool EditorTheme::ApplyTheme(const EditorThemeData& theme)
    {
        ApplyToImGui(theme);
        s_currentThemeName = theme.name;
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
        return names;
    }

    const EditorThemeData* EditorTheme::GetTheme(const std::string& themeName)
    {
        if (s_registeredThemes.empty())
            InitializeDefaultThemes();
        auto it = s_registeredThemes.find(themeName);
        return (it != s_registeredThemes.end()) ? &it->second : nullptr;
    }

    bool EditorTheme::RegisterTheme(const EditorThemeData& theme)
    {
        s_registeredThemes[theme.name] = theme;
        return true;
    }

    const std::string& EditorTheme::GetCurrentThemeName()
    {
        return s_currentThemeName;
    }

    bool EditorTheme::CreateBlendedTheme(const std::string& theme1, const std::string& theme2, float blend,
                                         const std::string& resultName)
    {
        auto* t1 = GetTheme(theme1);
        auto* t2 = GetTheme(theme2);
        if (!t1 || !t2)
            return false;

        EditorThemeData blended;
        blended.name = resultName;
        blended.background = t1->background.Lerp(t2->background, blend);
        blended.text = t1->text.Lerp(t2->text, blend);
        blended.accent = t1->accent.Lerp(t2->accent, blend);
        // Simplified — only blend key colors
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

    // ===================================================================
    // Core theme application — maps EditorThemeData to ImGui colors
    // ===================================================================

    void EditorTheme::ApplyToImGui(const EditorThemeData& theme)
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // --- Style values ---
        style.WindowRounding = theme.windowRounding;
        style.ChildRounding = theme.childRounding;
        style.FrameRounding = theme.frameRounding;
        style.PopupRounding = theme.popupRounding;
        style.ScrollbarRounding = theme.scrollbarRounding;
        style.GrabRounding = theme.grabRounding;
        style.TabRounding = theme.tabRounding;

        style.WindowBorderSize = theme.windowBorderSize;
        style.ChildBorderSize = theme.childBorderSize;
        style.PopupBorderSize = theme.popupBorderSize;
        style.FrameBorderSize = theme.frameBorderSize;

        style.IndentSpacing = theme.indentSpacing;
        style.ScrollbarSize = theme.scrollbarSize;
        style.GrabMinSize = theme.grabMinSize;

        style.WindowPadding = ImVec2(theme.windowPaddingX, theme.windowPaddingY);
        style.FramePadding = ImVec2(theme.framePaddingX, theme.framePaddingY);
        style.ItemSpacing = ImVec2(theme.itemSpacingX, theme.itemSpacingY);
        style.ItemInnerSpacing = ImVec2(theme.itemInnerSpacingX, theme.itemInnerSpacingY);

        // Smooth rendering — AA lines + fill, more tessellation on curves/circles.
        // Matches the soft edges of the SparkEditor hi-fi design.
        style.AntiAliasedLines = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill = true;
        style.CurveTessellationTol = 1.0f;
        style.CircleTessellationMaxError = 0.10f;

        // --- Color mapping ---
        ImVec4* c = style.Colors;

        // Window / Background
        c[ImGuiCol_WindowBg] = theme.background.ToImVec4();
        c[ImGuiCol_ChildBg] = theme.backgroundDark.ToImVec4();
        c[ImGuiCol_PopupBg] = theme.backgroundLight.WithAlpha(0.95f).ToImVec4();

        // Text
        c[ImGuiCol_Text] = theme.text.ToImVec4();
        c[ImGuiCol_TextDisabled] = theme.textDisabled.ToImVec4();

        // Borders
        c[ImGuiCol_Border] = theme.border.ToImVec4();
        c[ImGuiCol_BorderShadow] = ThemeColor(0, 0, 0, 0).ToImVec4();

        // Frame (input fields, checkboxes, etc.)
        c[ImGuiCol_FrameBg] = theme.frame.ToImVec4();
        c[ImGuiCol_FrameBgHovered] = theme.frameHovered.ToImVec4();
        c[ImGuiCol_FrameBgActive] = theme.frameActive.ToImVec4();

        // Title bar
        c[ImGuiCol_TitleBg] = theme.titleBar.ToImVec4();
        c[ImGuiCol_TitleBgActive] = theme.titleBarActive.ToImVec4();
        c[ImGuiCol_TitleBgCollapsed] = theme.titleBar.WithAlpha(0.6f).ToImVec4();

        // Menu bar
        c[ImGuiCol_MenuBarBg] = theme.menuBar.ToImVec4();

        // Scrollbar
        c[ImGuiCol_ScrollbarBg] = theme.scrollbar.ToImVec4();
        c[ImGuiCol_ScrollbarGrab] = theme.scrollbarGrab.ToImVec4();
        c[ImGuiCol_ScrollbarGrabHovered] = theme.scrollbarGrabHovered.ToImVec4();
        c[ImGuiCol_ScrollbarGrabActive] = theme.scrollbarGrabActive.ToImVec4();

        // Checkbox / Radio
        c[ImGuiCol_CheckMark] = theme.accent.ToImVec4();

        // Slider
        c[ImGuiCol_SliderGrab] = theme.accent.WithAlpha(0.8f).ToImVec4();
        c[ImGuiCol_SliderGrabActive] = theme.accent.ToImVec4();

        // Button
        c[ImGuiCol_Button] = theme.button.ToImVec4();
        c[ImGuiCol_ButtonHovered] = theme.buttonHovered.ToImVec4();
        c[ImGuiCol_ButtonActive] = theme.buttonActive.ToImVec4();

        // Header (collapsing headers, tree nodes, selectables)
        c[ImGuiCol_Header] = theme.backgroundHeader.ToImVec4();
        c[ImGuiCol_HeaderHovered] = theme.backgroundHover.ToImVec4();
        c[ImGuiCol_HeaderActive] = theme.backgroundActive.ToImVec4();

        // Separator
        c[ImGuiCol_Separator] = theme.borderSeparator.ToImVec4();
        c[ImGuiCol_SeparatorHovered] = theme.accent.WithAlpha(0.6f).ToImVec4();
        c[ImGuiCol_SeparatorActive] = theme.accent.ToImVec4();

        // Resize grip
        c[ImGuiCol_ResizeGrip] = theme.accent.WithAlpha(0.2f).ToImVec4();
        c[ImGuiCol_ResizeGripHovered] = theme.accent.WithAlpha(0.5f).ToImVec4();
        c[ImGuiCol_ResizeGripActive] = theme.accent.WithAlpha(0.8f).ToImVec4();

        // Tabs — active tab blends into panel, inactive recedes
        c[ImGuiCol_Tab] = theme.tab.ToImVec4();
        c[ImGuiCol_TabHovered] = theme.tabHovered.ToImVec4();
        c[ImGuiCol_TabSelected] = theme.tabActive.ToImVec4();
        c[ImGuiCol_TabDimmed] = theme.tabUnfocused.ToImVec4();
        c[ImGuiCol_TabDimmedSelected] = theme.tabActive.Darken(0.15f).ToImVec4();

        // Docking — accent preview, deep empty background
        c[ImGuiCol_DockingPreview] = theme.accent.WithAlpha(0.5f).ToImVec4();
        c[ImGuiCol_DockingEmptyBg] = theme.backgroundDark.Darken(0.3f).ToImVec4();

        // Plot
        c[ImGuiCol_PlotLines] = theme.graph1.ToImVec4();
        c[ImGuiCol_PlotLinesHovered] = theme.accent.ToImVec4();
        c[ImGuiCol_PlotHistogram] = theme.graph2.ToImVec4();
        c[ImGuiCol_PlotHistogramHovered] = theme.accentSecondary.ToImVec4();

        // Tables
        c[ImGuiCol_TableHeaderBg] = theme.backgroundHeader.ToImVec4();
        c[ImGuiCol_TableBorderStrong] = theme.border.ToImVec4();
        c[ImGuiCol_TableBorderLight] = theme.borderLight.ToImVec4();
        c[ImGuiCol_TableRowBg] = ThemeColor(0, 0, 0, 0).ToImVec4();
        c[ImGuiCol_TableRowBgAlt] = theme.backgroundDark.WithAlpha(0.3f).ToImVec4();

        // Text selection
        c[ImGuiCol_TextSelectedBg] = theme.selection.ToImVec4();

        // Drag / Drop
        c[ImGuiCol_DragDropTarget] = theme.drop.ToImVec4();

        // Nav highlight
        c[ImGuiCol_NavHighlight] = theme.focus.ToImVec4();
        c[ImGuiCol_NavWindowingHighlight] = theme.accent.WithAlpha(0.7f).ToImVec4();
        c[ImGuiCol_NavWindowingDimBg] = ThemeColor(0.2f, 0.2f, 0.2f, 0.2f).ToImVec4();

        // Modal dimming
        c[ImGuiCol_ModalWindowDimBg] = ThemeColor(0.0f, 0.0f, 0.0f, 0.5f).ToImVec4();
    }

    // ===================================================================
    // Predefined Themes
    // ===================================================================

    // Forward declaration
    static EditorThemeData CreateSparkThemeImpl();
    static EditorThemeData CreateSparkFusionThemeImpl();
    static EditorThemeData CreateSparkEmberThemeImpl();

    void EditorTheme::InitializeDefaultThemes()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing default editor themes");
        RegisterTheme(CreateSparkThemeImpl());
        RegisterTheme(CreateSparkFusionThemeImpl());
        RegisterTheme(CreateSparkEmberThemeImpl());
        RegisterTheme(CreateUnityProTheme());
        RegisterTheme(CreateUnrealProTheme());
        RegisterTheme(CreateVSProTheme());
        RegisterTheme(CreateJetBrainsTheme());
        RegisterTheme(CreateProfessionalLightTheme());
        RegisterTheme(CreateHighContrastTheme());
        RegisterTheme(CreateBlueAccentTheme());
        RegisterTheme(CreateOrangeAccentTheme());
    }

    // -------------------------------------------------------------------
    // SPARK PROFESSIONAL — Signature theme
    // Deep charcoal base, refined teal-cyan accent, warm amber secondary
    // Sleek dark techie aesthetic inspired by Unreal/Unity/Godot best practices
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
        t.menuItem = ThemeColor(0, 0, 0, 0);
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

    // -------------------------------------------------------------------
    // SPARK FUSION — Unreal + Unity + Godot inspired with Spark's own signature
    // High readability, sculpted contrast, cyan/amber visual language
    // -------------------------------------------------------------------
    static EditorThemeData CreateSparkFusionThemeImpl()
    {
        EditorThemeData t;
        t.name = "Spark Fusion";
        t.description = "Sleek pro dark blend of Unreal/Unity/Godot with Spark signature cyan + ember accents";
        t.author = "Spark Engine Team";

        // Background stack: deep neutral slate base with layered panel contrast.
        t.background = ThemeColor::FromHex("#171A20");
        t.backgroundDark = ThemeColor::FromHex("#101318");
        t.backgroundLight = ThemeColor::FromHex("#222734");
        t.backgroundAccent = ThemeColor::FromHex("#113A4A");
        t.backgroundHeader = ThemeColor::FromHex("#1C212C");
        t.backgroundActive = ThemeColor::FromHex("#2FB8CC").WithAlpha(0.24f);
        t.backgroundHover = ThemeColor::FromHex("#2A3140");
        t.backgroundSelected = ThemeColor::FromHex("#2FB8CC").WithAlpha(0.18f);

        // Text hierarchy tuned for long editor sessions.
        t.text = ThemeColor::FromHex("#E5EAF3");
        t.textDisabled = ThemeColor::FromHex("#596477");
        t.textSecondary = ThemeColor::FromHex("#97A4B8");
        t.textAccent = ThemeColor::FromHex("#5CD8EA");
        t.textWarning = ThemeColor::FromHex("#F5B45A");
        t.textError = ThemeColor::FromHex("#F06363");
        t.textSuccess = ThemeColor::FromHex("#54D79A");

        // Buttons: Unity-like clarity with Unreal-like depth.
        t.button = ThemeColor::FromHex("#2A3040");
        t.buttonHovered = ThemeColor::FromHex("#343C4F");
        t.buttonActive = ThemeColor::FromHex("#2FB8CC");
        t.buttonDisabled = ThemeColor::FromHex("#1E2430");

        // Frames: Godot-style recessed fields for dense data entry workflows.
        t.frame = ThemeColor::FromHex("#141923");
        t.frameHovered = ThemeColor::FromHex("#1C2431");
        t.frameActive = ThemeColor::FromHex("#2FB8CC").WithAlpha(0.37f);

        t.border = ThemeColor::FromHex("#2A3343");
        t.borderLight = ThemeColor::FromHex("#3A4558");
        t.borderAccent = ThemeColor::FromHex("#2FB8CC");
        t.borderSeparator = ThemeColor::FromHex("#242C39");

        t.titleBar = ThemeColor::FromHex("#0F131A");
        t.titleBarActive = ThemeColor::FromHex("#1D4D61");
        t.titleBarText = ThemeColor::FromHex("#E5EAF3");

        t.menuBar = ThemeColor::FromHex("#171A20");
        t.menuItem = ThemeColor(0, 0, 0, 0);
        t.menuItemHovered = ThemeColor::FromHex("#2FB8CC").WithAlpha(0.32f);

        t.scrollbar = ThemeColor::FromHex("#11161E");
        t.scrollbarGrab = ThemeColor::FromHex("#3B465A");
        t.scrollbarGrabHovered = ThemeColor::FromHex("#4D5B74");
        t.scrollbarGrabActive = ThemeColor::FromHex("#2FB8CC");

        t.tab = ThemeColor::FromHex("#1A1F29");
        t.tabHovered = ThemeColor::FromHex("#2FB8CC").WithAlpha(0.30f);
        t.tabActive = ThemeColor::FromHex("#232A37");
        t.tabUnfocused = ThemeColor::FromHex("#131720");

        // Spark twist: cool cyan primary, warm ember secondary for key states.
        t.accent = ThemeColor::FromHex("#2FB8CC");
        t.accentSecondary = ThemeColor::FromHex("#EE9C44");
        t.focus = ThemeColor::FromHex("#47CEE0");
        t.selection = ThemeColor::FromHex("#2FB8CC").WithAlpha(0.28f);
        t.drop = ThemeColor::FromHex("#EE9C44").WithAlpha(0.80f);

        t.graph1 = ThemeColor::FromHex("#2FB8CC");
        t.graph2 = ThemeColor::FromHex("#EE9C44");
        t.graph3 = ThemeColor::FromHex("#54D79A");
        t.graph4 = ThemeColor::FromHex("#F06363");
        t.graph5 = ThemeColor::FromHex("#A98AF9");

        // Style tuning: slightly crisper than Spark Professional, still modern.
        t.windowRounding = 5.0f;
        t.childRounding = 4.0f;
        t.frameRounding = 5.0f;
        t.popupRounding = 7.0f;
        t.scrollbarRounding = 11.0f;
        t.grabRounding = 4.0f;
        t.tabRounding = 5.0f;

        t.windowBorderSize = 1.0f;
        t.childBorderSize = 1.0f;
        t.popupBorderSize = 1.0f;
        t.frameBorderSize = 0.0f;

        t.windowPaddingX = 11.0f;
        t.windowPaddingY = 10.0f;
        t.framePaddingX = 9.0f;
        t.framePaddingY = 6.0f;
        t.itemSpacingX = 9.0f;
        t.itemSpacingY = 6.0f;
        t.itemInnerSpacingX = 6.0f;
        t.itemInnerSpacingY = 5.0f;

        t.indentSpacing = 20.0f;
        t.scrollbarSize = 12.0f;
        t.grabMinSize = 10.0f;

        t.shadowOpacity = 0.42f;
        t.shadowSize = 10.0f;
        t.fontSize = 15.0f;
        t.fontScale = 1.0f;
        t.fontFamily = "Inter";

        return t;
    }

    // -------------------------------------------------------------------
    // SPARK EMBER — Warm charcoal IDE with ember/spark accent
    // Matches the SparkEditor hi-fi design (IBM Plex Sans + JetBrains Mono,
    // OKLCH neutrals at hue 60, ember accent at oklch(0.72 0.16 50)).
    // -------------------------------------------------------------------
    static EditorThemeData CreateSparkEmberThemeImpl()
    {
        EditorThemeData t;
        t.name = "Spark Ember";
        t.description = "Warm charcoal IDE with ember/spark accent — matches SparkEditor hi-fi design";
        t.author = "Spark Engine Team";

        // Warm neutral background stack (hue ≈ 60, very low chroma).
        t.background = ThemeColor::FromHex("#1A1716");      // bg-1: panel
        t.backgroundDark = ThemeColor::FromHex("#110E0D");  // bg-0: deepest
        t.backgroundLight = ThemeColor::FromHex("#302D2A"); // bg-3: hover/input
        t.backgroundAccent = ThemeColor::FromHex("#AF530D").WithAlpha(0.22f);
        t.backgroundHeader = ThemeColor::FromHex("#23201E"); // bg-2: header row
        t.backgroundActive = ThemeColor::FromHex("#F1823A").WithAlpha(0.20f);
        t.backgroundHover = ThemeColor::FromHex("#302D2A");
        t.backgroundSelected = ThemeColor::FromHex("#F1823A").WithAlpha(0.16f);

        // Warm off-white foreground (hue ≈ 80).
        t.text = ThemeColor::FromHex("#C5C3C1");          // fg-1
        t.textDisabled = ThemeColor::FromHex("#64625F");  // fg-3
        t.textSecondary = ThemeColor::FromHex("#8D8B88"); // fg-2
        t.textAccent = ThemeColor::FromHex("#F1823A");    // ember
        t.textWarning = ThemeColor::FromHex("#EDCF59");   // yellow
        t.textError = ThemeColor::FromHex("#FA6862");     // red
        t.textSuccess = ThemeColor::FromHex("#6ED086");   // green

        // Buttons — slightly raised from panel.
        t.button = ThemeColor::FromHex("#23201E");
        t.buttonHovered = ThemeColor::FromHex("#302D2A");
        t.buttonActive = ThemeColor::FromHex("#F1823A");
        t.buttonDisabled = ThemeColor::FromHex("#1A1716");

        // Frames (input fields) — recessed, darker than panel.
        t.frame = ThemeColor::FromHex("#110E0D");
        t.frameHovered = ThemeColor::FromHex("#23201E");
        t.frameActive = ThemeColor::FromHex("#F1823A").WithAlpha(0.35f);

        // Borders — barely visible on idle, ember on focus.
        t.border = ThemeColor::FromHex("#35322F");      // line
        t.borderLight = ThemeColor::FromHex("#403C38"); // bg-4
        t.borderAccent = ThemeColor::FromHex("#F1823A");
        t.borderSeparator = ThemeColor::FromHex("#282523"); // line-soft

        // Title bar — deepest charcoal; active tab uses a dim ember wash.
        t.titleBar = ThemeColor::FromHex("#110E0D");
        t.titleBarActive = ThemeColor::FromHex("#AF530D");
        t.titleBarText = ThemeColor::FromHex("#EFEEEB");

        // Menu bar — seamless with panel background.
        t.menuBar = ThemeColor::FromHex("#1A1716");
        t.menuItem = ThemeColor(0, 0, 0, 0);
        t.menuItemHovered = ThemeColor::FromHex("#F1823A").WithAlpha(0.30f);

        // Scrollbars — thin, warm, ember-active.
        t.scrollbar = ThemeColor::FromHex("#110E0D");
        t.scrollbarGrab = ThemeColor::FromHex("#403C38");
        t.scrollbarGrabHovered = ThemeColor::FromHex("#5A544E");
        t.scrollbarGrabActive = ThemeColor::FromHex("#F1823A");

        // Tabs — active tab rises to header tone.
        t.tab = ThemeColor::FromHex("#1A1716");
        t.tabHovered = ThemeColor::FromHex("#F1823A").WithAlpha(0.25f);
        t.tabActive = ThemeColor::FromHex("#23201E");
        t.tabUnfocused = ThemeColor::FromHex("#110E0D");

        // Accents — ember primary, cyan secondary (matches design's dual accent).
        t.accent = ThemeColor::FromHex("#F1823A");
        t.accentSecondary = ThemeColor::FromHex("#5FCEEE");
        t.focus = ThemeColor::FromHex("#F1823A");
        t.selection = ThemeColor::FromHex("#F1823A").WithAlpha(0.28f);
        t.drop = ThemeColor::FromHex("#F1823A").WithAlpha(0.75f);

        // Graph palette — ember, cyan, green, red, blue (design's semantic set).
        t.graph1 = ThemeColor::FromHex("#F1823A");
        t.graph2 = ThemeColor::FromHex("#5FCEEE");
        t.graph3 = ThemeColor::FromHex("#6ED086");
        t.graph4 = ThemeColor::FromHex("#FA6862");
        t.graph5 = ThemeColor::FromHex("#6AA7F4");

        // Style tuning — tight, IDE-like radii matching the hi-fi design.
        t.windowRounding = 3.0f;
        t.childRounding = 3.0f;
        t.frameRounding = 3.0f;
        t.popupRounding = 5.0f;
        t.scrollbarRounding = 5.0f;
        t.grabRounding = 3.0f;
        t.tabRounding = 4.0f;

        t.windowBorderSize = 1.0f;
        t.childBorderSize = 1.0f;
        t.popupBorderSize = 1.0f;
        // Design separates frames by color, not by stroke — disable per-frame border.
        t.frameBorderSize = 0.0f;

        t.windowPaddingX = 10.0f;
        t.windowPaddingY = 10.0f;
        t.framePaddingX = 9.0f;
        t.framePaddingY = 5.0f;
        t.itemSpacingX = 8.0f;
        t.itemSpacingY = 5.0f;
        t.itemInnerSpacingX = 6.0f;
        t.itemInnerSpacingY = 5.0f;

        t.indentSpacing = 18.0f;
        t.scrollbarSize = 10.0f;
        t.grabMinSize = 10.0f;

        t.shadowOpacity = 0.45f;
        t.shadowSize = 10.0f;
        t.fontSize = 13.0f;
        t.fontScale = 1.0f;
        t.fontFamily = "IBM Plex Sans";

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
        t.menuItem = ThemeColor(0, 0, 0, 0);
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
        t.menuItem = ThemeColor(0, 0, 0, 0);
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
        t.menuItem = ThemeColor(0, 0, 0, 0);
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
        t.menuItem = ThemeColor(0, 0, 0, 0);
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
        return t;
    }

    EditorThemeData EditorTheme::CreateProfessionalLightTheme()
    {
        EditorThemeData t;
        t.name = "Professional Light";
        t.description = "Clean, professional light theme";

        t.background = ThemeColor::FromRGB(240, 240, 240);
        t.backgroundDark = ThemeColor::FromRGB(230, 230, 230);
        t.backgroundLight = ThemeColor::FromRGB(255, 255, 255);
        t.backgroundAccent = ThemeColor::FromRGB(0, 120, 215);
        t.backgroundHeader = ThemeColor::FromRGB(235, 235, 235);
        t.backgroundActive = ThemeColor::FromRGB(0, 120, 215, 50);
        t.backgroundHover = ThemeColor::FromRGB(225, 225, 225);
        t.backgroundSelected = ThemeColor::FromRGB(0, 120, 215, 30);

        t.text = ThemeColor::FromRGB(30, 30, 30);
        t.textDisabled = ThemeColor::FromRGB(160, 160, 160);
        t.textSecondary = ThemeColor::FromRGB(100, 100, 100);
        t.textAccent = ThemeColor::FromRGB(0, 100, 200);
        t.textWarning = ThemeColor::FromRGB(180, 130, 0);
        t.textError = ThemeColor::FromRGB(190, 40, 40);
        t.textSuccess = ThemeColor::FromRGB(40, 150, 60);

        t.button = ThemeColor::FromRGB(225, 225, 225);
        t.buttonHovered = ThemeColor::FromRGB(210, 210, 210);
        t.buttonActive = ThemeColor::FromRGB(0, 120, 215);
        t.buttonDisabled = ThemeColor::FromRGB(235, 235, 235);
        t.frame = ThemeColor::FromRGB(255, 255, 255);
        t.frameHovered = ThemeColor::FromRGB(245, 245, 245);
        t.frameActive = ThemeColor::FromRGB(0, 120, 215, 80);
        t.border = ThemeColor::FromRGB(200, 200, 200);
        t.borderLight = ThemeColor::FromRGB(220, 220, 220);
        t.borderAccent = ThemeColor::FromRGB(0, 120, 215);
        t.borderSeparator = ThemeColor::FromRGB(210, 210, 210);
        t.titleBar = ThemeColor::FromRGB(230, 230, 230);
        t.titleBarActive = ThemeColor::FromRGB(0, 120, 215);
        t.titleBarText = ThemeColor::FromRGB(30, 30, 30);
        t.menuBar = ThemeColor::FromRGB(240, 240, 240);
        t.menuItem = ThemeColor(0, 0, 0, 0);
        t.menuItemHovered = ThemeColor::FromRGB(0, 120, 215, 80);
        t.scrollbar = ThemeColor::FromRGB(240, 240, 240);
        t.scrollbarGrab = ThemeColor::FromRGB(180, 180, 180);
        t.scrollbarGrabHovered = ThemeColor::FromRGB(160, 160, 160);
        t.scrollbarGrabActive = ThemeColor::FromRGB(140, 140, 140);
        t.tab = ThemeColor::FromRGB(230, 230, 230);
        t.tabHovered = ThemeColor::FromRGB(200, 220, 240);
        t.tabActive = ThemeColor::FromRGB(255, 255, 255);
        t.tabUnfocused = ThemeColor::FromRGB(235, 235, 235);
        t.accent = ThemeColor::FromRGB(0, 120, 215);
        t.accentSecondary = ThemeColor::FromRGB(0, 90, 180);
        t.focus = ThemeColor::FromRGB(0, 120, 215);
        t.selection = ThemeColor::FromRGB(0, 120, 215, 60);
        t.drop = ThemeColor::FromRGB(0, 120, 215, 150);
        t.graph1 = t.accent;
        t.graph2 = t.textWarning;
        t.graph3 = t.textSuccess;
        t.graph4 = t.textError;
        t.graph5 = ThemeColor::FromRGB(120, 60, 190);

        t.windowRounding = 2.0f;
        t.frameRounding = 2.0f;
        t.tabRounding = 3.0f;
        t.frameBorderSize = 1.0f;
        return t;
    }

    EditorThemeData EditorTheme::CreateHighContrastTheme()
    {
        EditorThemeData t;
        t.name = "High Contrast";
        t.description = "High contrast accessibility theme";

        t.background = ThemeColor::FromRGB(0, 0, 0);
        t.backgroundDark = ThemeColor::FromRGB(0, 0, 0);
        t.backgroundLight = ThemeColor::FromRGB(20, 20, 20);
        t.backgroundAccent = ThemeColor::FromRGB(0, 120, 255);
        t.backgroundHeader = ThemeColor::FromRGB(15, 15, 15);
        t.backgroundActive = ThemeColor::FromRGB(0, 120, 255, 150);
        t.backgroundHover = ThemeColor::FromRGB(30, 30, 30);
        t.backgroundSelected = ThemeColor::FromRGB(0, 120, 255, 100);

        t.text = ThemeColor::FromRGB(255, 255, 255);
        t.textDisabled = ThemeColor::FromRGB(128, 128, 128);
        t.textSecondary = ThemeColor::FromRGB(200, 200, 200);
        t.textAccent = ThemeColor::FromRGB(0, 200, 255);
        t.textWarning = ThemeColor::FromRGB(255, 220, 0);
        t.textError = ThemeColor::FromRGB(255, 50, 50);
        t.textSuccess = ThemeColor::FromRGB(0, 255, 100);

        t.button = ThemeColor::FromRGB(30, 30, 30);
        t.buttonHovered = ThemeColor::FromRGB(50, 50, 50);
        t.buttonActive = ThemeColor::FromRGB(0, 120, 255);
        t.buttonDisabled = ThemeColor::FromRGB(20, 20, 20);
        t.frame = ThemeColor::FromRGB(10, 10, 10);
        t.frameHovered = ThemeColor::FromRGB(30, 30, 30);
        t.frameActive = ThemeColor::FromRGB(0, 120, 255, 150);
        t.border = ThemeColor::FromRGB(200, 200, 200);
        t.borderLight = ThemeColor::FromRGB(150, 150, 150);
        t.borderAccent = ThemeColor::FromRGB(0, 200, 255);
        t.borderSeparator = ThemeColor::FromRGB(150, 150, 150);
        t.titleBar = ThemeColor::FromRGB(0, 0, 0);
        t.titleBarActive = ThemeColor::FromRGB(0, 120, 255);
        t.titleBarText = ThemeColor::FromRGB(255, 255, 255);
        t.menuBar = ThemeColor::FromRGB(0, 0, 0);
        t.menuItem = ThemeColor(0, 0, 0, 0);
        t.menuItemHovered = ThemeColor::FromRGB(0, 120, 255, 200);
        t.scrollbar = ThemeColor::FromRGB(0, 0, 0);
        t.scrollbarGrab = ThemeColor::FromRGB(100, 100, 100);
        t.scrollbarGrabHovered = ThemeColor::FromRGB(150, 150, 150);
        t.scrollbarGrabActive = ThemeColor::FromRGB(200, 200, 200);
        t.tab = ThemeColor::FromRGB(0, 0, 0);
        t.tabHovered = ThemeColor::FromRGB(30, 30, 30);
        t.tabActive = ThemeColor::FromRGB(0, 120, 255);
        t.tabUnfocused = ThemeColor::FromRGB(0, 0, 0);
        t.accent = ThemeColor::FromRGB(0, 200, 255);
        t.accentSecondary = ThemeColor::FromRGB(0, 120, 255);
        t.focus = ThemeColor::FromRGB(0, 200, 255);
        t.selection = ThemeColor::FromRGB(0, 120, 255, 120);
        t.drop = ThemeColor::FromRGB(0, 200, 255, 200);
        t.graph1 = t.accent;
        t.graph2 = t.textWarning;
        t.graph3 = t.textSuccess;
        t.graph4 = t.textError;
        t.graph5 = ThemeColor::FromRGB(200, 100, 255);

        t.windowRounding = 0.0f;
        t.frameRounding = 0.0f;
        t.tabRounding = 0.0f;
        t.frameBorderSize = 2.0f;
        t.windowBorderSize = 2.0f;
        return t;
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
        return t;
    }

    // ===================================================================
    // Private helpers
    // ===================================================================

    ThemeColor EditorTheme::GetSystemAccentColor()
    {
        return ThemeColor::FromHex("#2D8CF0");
    }

    ThemeColor EditorTheme::CreateComplementaryColor(const ThemeColor& base)
    {
        return ThemeColor(1.0f - base.r, 1.0f - base.g, 1.0f - base.b, base.a);
    }

    std::vector<ThemeColor> EditorTheme::CreateColorPalette(const ThemeColor& base)
    {
        return {base, base.Lighten(0.2f), base.Darken(0.2f), base.Desaturate(0.3f), base.Lighten(0.4f)};
    }

    unsigned int EditorTheme::ColorToImGui(const ThemeColor& color)
    {
        return IM_COL32((int)(color.r * 255.0f), (int)(color.g * 255.0f), (int)(color.b * 255.0f),
                        (int)(color.a * 255.0f));
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
            EditorTheme::ApplyTheme("SparkDark");
            const EditorThemeData* resetTheme = EditorTheme::GetTheme("SparkDark");
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

            auto writeColor = [&](const std::string& name, const ThemeColor& c)
            { file << "    \"" << name << "\": [" << c.r << ", " << c.g << ", " << c.b << ", " << c.a << "]"; };

            file << "{\n";
            file << "  \"name\": \"" << theme.name << "\",\n";
            file << "  \"description\": \"" << theme.description << "\",\n";
            file << "  \"author\": \"" << theme.author << "\",\n";
            file << "  \"colors\": {\n";

            writeColor("background", theme.background);
            file << ",\n";
            writeColor("backgroundDark", theme.backgroundDark);
            file << ",\n";
            writeColor("backgroundLight", theme.backgroundLight);
            file << ",\n";
            writeColor("backgroundAccent", theme.backgroundAccent);
            file << ",\n";
            writeColor("backgroundHeader", theme.backgroundHeader);
            file << ",\n";
            writeColor("backgroundActive", theme.backgroundActive);
            file << ",\n";
            writeColor("backgroundHover", theme.backgroundHover);
            file << ",\n";
            writeColor("backgroundSelected", theme.backgroundSelected);
            file << ",\n";
            writeColor("text", theme.text);
            file << ",\n";
            writeColor("textDisabled", theme.textDisabled);
            file << ",\n";
            writeColor("textSecondary", theme.textSecondary);
            file << ",\n";
            writeColor("textAccent", theme.textAccent);
            file << ",\n";
            writeColor("textWarning", theme.textWarning);
            file << ",\n";
            writeColor("textError", theme.textError);
            file << ",\n";
            writeColor("textSuccess", theme.textSuccess);
            file << ",\n";
            writeColor("button", theme.button);
            file << ",\n";
            writeColor("buttonHovered", theme.buttonHovered);
            file << ",\n";
            writeColor("buttonActive", theme.buttonActive);
            file << ",\n";
            writeColor("frame", theme.frame);
            file << ",\n";
            writeColor("frameHovered", theme.frameHovered);
            file << ",\n";
            writeColor("frameActive", theme.frameActive);
            file << ",\n";
            writeColor("border", theme.border);
            file << ",\n";
            writeColor("borderLight", theme.borderLight);
            file << ",\n";
            writeColor("borderAccent", theme.borderAccent);
            file << ",\n";
            writeColor("borderSeparator", theme.borderSeparator);
            file << "\n";

            file << "  }\n}\n";
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
                                      Spark::LocalFileCache* cache)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !filepath.empty(), false);
        try
        {
            std::string content;

            if (cache)
            {
                auto result = cache->ReadText(filepath);
                if (result.IsOk())
                {
                    content = result.Value();
                }
            }

            if (content.empty())
            {
                std::ifstream file(filepath);
                if (!file.is_open())
                    return false;
                content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                file.close();
            }

            // Simple JSON-like parser for theme colors
            auto extractString = [&](const std::string& key) -> std::string
            {
                std::string search = "\"" + key + "\": \"";
                auto pos = content.find(search);
                if (pos == std::string::npos)
                    return "";
                pos += search.length();
                auto end = content.find("\"", pos);
                if (end == std::string::npos)
                    return "";
                return content.substr(pos, end - pos);
            };

            auto extractColor = [&](const std::string& key) -> ThemeColor
            {
                std::string search = "\"" + key + "\": [";
                auto pos = content.find(search);
                if (pos == std::string::npos)
                    return ThemeColor();
                pos += search.length();
                auto end = content.find("]", pos);
                if (end == std::string::npos)
                    return ThemeColor();
                std::string vals = content.substr(pos, end - pos);
                float r = 0, g = 0, b = 0, a = 1;
                // r, g and b are required; a keeps its default when the entry omits alpha.
                if (std::sscanf(vals.c_str(), "%f, %f, %f, %f", &r, &g, &b, &a) < 3)
                    return ThemeColor();
                return ThemeColor(r, g, b, a);
            };

            outTheme.name = extractString("name");
            outTheme.description = extractString("description");
            outTheme.author = extractString("author");

            outTheme.background = extractColor("background");
            outTheme.backgroundDark = extractColor("backgroundDark");
            outTheme.backgroundLight = extractColor("backgroundLight");
            outTheme.backgroundAccent = extractColor("backgroundAccent");
            outTheme.backgroundHeader = extractColor("backgroundHeader");
            outTheme.backgroundActive = extractColor("backgroundActive");
            outTheme.backgroundHover = extractColor("backgroundHover");
            outTheme.backgroundSelected = extractColor("backgroundSelected");
            outTheme.text = extractColor("text");
            outTheme.textDisabled = extractColor("textDisabled");
            outTheme.textSecondary = extractColor("textSecondary");
            outTheme.textAccent = extractColor("textAccent");
            outTheme.textWarning = extractColor("textWarning");
            outTheme.textError = extractColor("textError");
            outTheme.textSuccess = extractColor("textSuccess");
            outTheme.button = extractColor("button");
            outTheme.buttonHovered = extractColor("buttonHovered");
            outTheme.buttonActive = extractColor("buttonActive");
            outTheme.frame = extractColor("frame");
            outTheme.frameHovered = extractColor("frameHovered");
            outTheme.frameActive = extractColor("frameActive");
            outTheme.border = extractColor("border");
            outTheme.borderLight = extractColor("borderLight");
            outTheme.borderAccent = extractColor("borderAccent");
            outTheme.borderSeparator = extractColor("borderSeparator");

            return !outTheme.name.empty();
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
