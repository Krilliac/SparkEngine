/**
 * @file EditorTheme.h
 * @brief Professional theme management system with Unity/Unreal-inspired styling
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

struct ImVec4;
struct ImGuiStyle;

namespace SparkEditor
{

    /**
 * @brief Represents a color in the editor theme system
 */
    struct ThemeColor
    {
        float r, g, b, a;

        ThemeColor() : r(0.0f), g(0.0f), b(0.0f), a(1.0f) {}
        ThemeColor(float red, float green, float blue, float alpha = 1.0f) : r(red), g(green), b(blue), a(alpha) {}

        /**
     * @brief Convert to ImGui ImVec4 format
     */
        ImVec4 ToImVec4() const;

        /**
     * @brief Create color from RGB values (0-255)
     */
        static ThemeColor FromRGB(int red, int green, int blue, int alpha = 255);

        /**
     * @brief Create color from hex string
     */
        static ThemeColor FromHex(const std::string& hex);

        /**
     * @brief Interpolate between two colors
     */
        ThemeColor Lerp(const ThemeColor& other, float t) const;

        /**
     * @brief Create a darker version of this color
     */
        ThemeColor Darken(float amount) const;

        /**
     * @brief Create a lighter version of this color
     */
        ThemeColor Lighten(float amount) const;

        /**
     * @brief Create a desaturated version of this color
     */
        ThemeColor Desaturate(float amount) const;

        /**
     * @brief Adjust alpha channel
     */
        ThemeColor WithAlpha(float alpha) const;
    };

    /**
 * @brief Complete theme data structure with professional styling options
 */
    struct EditorThemeData
    {
        std::string name;
        std::string description;
        std::string author;

        // === COLOR PALETTE ===

        // Background Colors
        ThemeColor background;         // Main window background
        ThemeColor backgroundDark;     // Darker background for panels
        ThemeColor backgroundLight;    // Lighter background for buttons/frames
        ThemeColor backgroundAccent;   // Accent background for highlights
        ThemeColor backgroundHeader;   // Header background
        ThemeColor backgroundActive;   // Active element background
        ThemeColor backgroundHover;    // Hover background
        ThemeColor backgroundSelected; // Selected item background

        // Text Colors
        ThemeColor text;          // Primary text
        ThemeColor textDisabled;  // Disabled text
        ThemeColor textSecondary; // Secondary/dimmed text
        ThemeColor textAccent;    // Accent text (links, etc.)
        ThemeColor textWarning;   // Warning text
        ThemeColor textError;     // Error text
        ThemeColor textSuccess;   // Success text

        // UI Element Colors
        ThemeColor button;         // Button default
        ThemeColor buttonHovered;  // Button hovered
        ThemeColor buttonActive;   // Button pressed
        ThemeColor buttonDisabled; // Button disabled

        ThemeColor frame;        // Frame/input background
        ThemeColor frameHovered; // Frame hovered
        ThemeColor frameActive;  // Frame active

        ThemeColor border;          // Border color
        ThemeColor borderLight;     // Light border
        ThemeColor borderAccent;    // Accent border
        ThemeColor borderSeparator; // Separator lines

        // Panel-specific Colors
        ThemeColor titleBar;       // Title bar background
        ThemeColor titleBarActive; // Active title bar
        ThemeColor titleBarText;   // Title bar text

        ThemeColor menuBar;         // Menu bar background
        ThemeColor menuItem;        // Menu item background
        ThemeColor menuItemHovered; // Menu item hovered

        ThemeColor scrollbar;            // Scrollbar background
        ThemeColor scrollbarGrab;        // Scrollbar grab
        ThemeColor scrollbarGrabHovered; // Scrollbar grab hovered
        ThemeColor scrollbarGrabActive;  // Scrollbar grab active

        ThemeColor tab;          // Tab default
        ThemeColor tabHovered;   // Tab hovered
        ThemeColor tabActive;    // Tab active/selected
        ThemeColor tabUnfocused; // Tab unfocused

        // Special Colors
        ThemeColor accent;          // Primary accent color
        ThemeColor accentSecondary; // Secondary accent color
        ThemeColor focus;           // Focus indicator
        ThemeColor selection;       // Selection color
        ThemeColor drop;            // Drop target color

        // Graph/Chart Colors
        ThemeColor graph1, graph2, graph3, graph4, graph5;

        // === STYLE VALUES ===

        // Rounding
        float windowRounding = 0.0f;
        float childRounding = 0.0f;
        float frameRounding = 3.0f;
        float popupRounding = 0.0f;
        float scrollbarRounding = 9.0f;
        float grabRounding = 3.0f;
        float tabRounding = 4.0f;

        // Borders
        float windowBorderSize = 1.0f;
        float childBorderSize = 1.0f;
        float popupBorderSize = 1.0f;
        float frameBorderSize = 0.0f;

        // Spacing
        float indentSpacing = 21.0f;
        float scrollbarSize = 16.0f;
        float grabMinSize = 10.0f;

        // Padding
        float windowPaddingX = 8.0f;
        float windowPaddingY = 8.0f;
        float framePaddingX = 4.0f;
        float framePaddingY = 3.0f;
        float itemSpacingX = 8.0f;
        float itemSpacingY = 4.0f;
        float itemInnerSpacingX = 4.0f;
        float itemInnerSpacingY = 4.0f;

        // Additional Professional Features
        float shadowOpacity = 0.35f;
        float shadowSize = 8.0f;
        bool enableAnimations = true;
        bool enableGradients = true;
        bool enableShadows = true;

        // Typography
        float fontSize = 16.0f;
        float fontScale = 1.0f;
        std::string fontFamily = "Segoe UI";

        EditorThemeData() = default;
    };

    /**
 * @brief Professional theme management system
 */
    class EditorTheme
    {
      public:
        /**
     * @brief Apply a theme by name
     */
        static bool ApplyTheme(const std::string& themeName);

        /**
     * @brief Apply a theme directly
     */
        static bool ApplyTheme(const EditorThemeData& theme);

        /**
     * @brief Get list of available theme names
     */
        static std::vector<std::string> GetAvailableThemes();

        /**
     * @brief Get theme data by name
     */
        static const EditorThemeData* GetTheme(const std::string& themeName);

        /**
     * @brief Register a new theme
     */
        static bool RegisterTheme(const EditorThemeData& theme);

        /**
     * @brief Get current theme name
     */
        static const std::string& GetCurrentThemeName();

        /**
     * @brief Create and apply a custom interpolated theme between two themes
     */
        static bool CreateBlendedTheme(const std::string& theme1, const std::string& theme2, float blend,
                                       const std::string& resultName);

        /**
     * @brief Apply professional visual enhancements
     */
        static void ApplyProfessionalEnhancements();

        /**
     * @brief Apply custom fonts if available
     */
        static void ApplyCustomFonts();

        // === PREDEFINED THEMES ===

        /**
     * @brief Create Unity-inspired professional dark theme
     */
        static EditorThemeData CreateUnityProTheme();

        /**
     * @brief Create Unreal Engine-inspired dark theme
     */
        static EditorThemeData CreateUnrealProTheme();

        /**
     * @brief Create Visual Studio-inspired dark theme
     */
        static EditorThemeData CreateVSProTheme();

        /**
     * @brief Create JetBrains-inspired dark theme
     */
        static EditorThemeData CreateJetBrainsTheme();

        /**
     * @brief Create professional light theme
     */
        static EditorThemeData CreateProfessionalLightTheme();

        /**
     * @brief Create high contrast accessibility theme
     */
        static EditorThemeData CreateHighContrastTheme();

        /**
     * @brief Create custom blue accent theme
     */
        static EditorThemeData CreateBlueAccentTheme();

        /**
     * @brief Create custom orange accent theme
     */
        static EditorThemeData CreateOrangeAccentTheme();

      private:
        static void InitializeDefaultThemes();
        static void ApplyToImGui(const EditorThemeData& theme);
        static void ApplyAdvancedStyling(const EditorThemeData& theme);
        static void SetupCustomDrawCallbacks();
        static unsigned int ColorToImGui(const ThemeColor& color);

        // === COLOR HELPERS ===
        static ThemeColor GetSystemAccentColor();
        static ThemeColor CreateComplementaryColor(const ThemeColor& base);
        static std::vector<ThemeColor> CreateColorPalette(const ThemeColor& base);

        static std::unordered_map<std::string, EditorThemeData> s_registeredThemes;
        static std::string s_currentThemeName;
        static bool s_enhancementsEnabled;
        static bool s_customFontsLoaded;
    };

    /**
 * @brief Theme customization utilities
 */
    class ThemeCustomizer
    {
      public:
        /**
     * @brief Live theme editor for runtime customization
     */
        static void ShowThemeEditor();

        /**
     * @brief Export theme to file
     */
        static bool ExportTheme(const EditorThemeData& theme, const std::string& filepath);

        /**
     * @brief Import theme from file
     */
        static bool ImportTheme(const std::string& filepath, EditorThemeData& outTheme);

        /**
     * @brief Generate theme variations automatically
     */
        static std::vector<EditorThemeData> GenerateThemeVariations(const EditorThemeData& baseTheme);
    };

} // namespace SparkEditor