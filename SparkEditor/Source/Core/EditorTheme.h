/**
 * @file EditorTheme.h
 * @brief Editor theme registry and the adapter from EditorThemeData onto the Spark UI tokens and
 *        the live ImGui style.
 * @author Spark Engine Team
 * @date 2025
 *
 * The five built-in palettes (Ember Studio, Cobalt Forge, Graphite Signal, Professional Light,
 * High Contrast) are generated from the audited Spark UI token tables (SparkUI/Tokens.h); custom
 * and imported themes are applied byte-for-byte. ApplyTheme installs the theme's token set on the
 * current ImGui context (Spark::Ui::SetTokens, which rebuilds the style at the context's UI scale)
 * and then writes every colour field of EditorThemeData onto its ImGui colour slot, so a field is
 * never silently ignored.
 *
 * Thread affinity: UI thread only. The registry, the current-theme snapshot, the density and the
 * appearance override are process statics with no locking; every function that touches the style
 * acts on the current ImGui context. Ownership: EditorTheme owns the registered theme copies;
 * callers receive references that stay valid until the next RegisterTheme or ApplyTheme.
 */

#pragma once

#include "SparkUI/Enums.h"
#include "SparkUI/Tokens.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ImVec4;
struct ImGuiStyle;

namespace Spark
{
    class LocalFileCache;
}

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

    /// @brief Index of each status in EditorThemeData::statusFills.
    enum class ThemeStatusFill : std::uint8_t
    {
        Info,
        Success,
        Warning,
        Error,
        Count
    };

    /**
 * @brief Complete theme data: semantic colours, metrics and typography.
 *
 * Each colour field reaches the screen through exactly one primary sink, either an ImGui colour
 * slot or a Spark UI token role read by the widget kit (EditorTheme::ToTokens). Fields that alias
 * the same role in the built-in palettes keep separate sinks so a custom theme can still set them
 * independently. Struct defaults are the historical values; the built-in factories overwrite them.
 */
    struct EditorThemeData
    {
        std::string name;
        std::string description;
        std::string author;

        // === COLOR PALETTE ===

        // Background Colors
        ThemeColor background;         // Window background (WindowBg)
        ThemeColor backgroundDark;     // Sunken surface: empty dock space, Spark SurfaceSunken role
        ThemeColor backgroundLight;    // Raised surface: alternate table rows, Spark SurfaceRaised role
        ThemeColor backgroundAccent;   // Accent wash behind drop targets (DragDropTargetBg)
        ThemeColor backgroundHeader;   // Collapsing/tree headers and table headers
        ThemeColor backgroundActive;   // Pressed header/selectable
        ThemeColor backgroundHover;    // Spark SurfaceHover role
        ThemeColor backgroundSelected; // Selected checkbox background, Spark SurfaceSelected role

        // Text Colors
        ThemeColor text;          // Primary text
        ThemeColor textDisabled;  // Disabled text (ImGui TextDisabled)
        ThemeColor textSecondary; // Secondary/dimmed text
        ThemeColor textAccent;    // Links
        ThemeColor textWarning;   // Warning text
        ThemeColor textError;     // Error text
        ThemeColor textSuccess;   // Success text

        // UI Element Colors
        ThemeColor button;         // Button default
        ThemeColor buttonHovered;  // Button hovered
        ThemeColor buttonActive;   // Button pressed
        ThemeColor buttonDisabled; // Disabled-control text drawn by the widget kit (Spark TextDisabled role)

        ThemeColor frame;        // Frame/input background
        ThemeColor frameHovered; // Frame hovered
        ThemeColor frameActive;  // Frame active

        ThemeColor border;          // Subtle hairlines: table inner borders, tree lines
        ThemeColor borderLight;     // Hovered separator
        ThemeColor borderAccent;    // Active separator and resize grip
        ThemeColor borderSeparator; // Separator lines

        // Panel-specific Colors
        ThemeColor titleBar;       // Title bar background
        ThemeColor titleBarActive; // Active title bar; also the native window caption colour
        ThemeColor titleBarText;   // Native window caption text

        ThemeColor menuBar;         // Menu bar background
        ThemeColor menuItem;        // Menu, popup and tooltip background (PopupBg)
        ThemeColor menuItemHovered; // Hovered menu item, selectable and tree row (HeaderHovered)

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
        ThemeColor focus;           // Keyboard focus ring
        ThemeColor selection;       // Text selection
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
        float fontSize = 16.0f; // Body size in logical pixels (style.FontSizeBase)
        float fontScale = 1.0f; // Multiplies the user UI scale (style.FontScaleMain)
        std::string fontFamily = "Segoe UI";

        // === EMBER SIGNAL ROLES (appended; token-only unless noted) ===
        ThemeColor borderStrong;             // Frame and popup outlines, strong table borders (Border)
        ThemeColor surfaceOverlay;           // Floating chips, notifications and overlays
        ThemeColor surfaceSelectedUnfocused; // Selection in an unfocused panel
        ThemeColor textOnAccent;             // Text on accent fills (primary buttons, Play status bar)
        ThemeColor textInfo;                 // Informational status text
        ThemeColor accentHover;              // Hovered accent fill (slider grab while dragging)
        ThemeColor accentActive;             // Pressed accent fill (resize grip while dragging)
        ThemeColor accentText;               // Accent-coloured text on selected surfaces
        std::array<ThemeColor, static_cast<std::size_t>(ThemeStatusFill::Count)> statusFills; // By ThemeStatusFill
        Spark::Ui::Appearance appearance = Spark::Ui::Appearance::Dark;
        Spark::Ui::Density density = Spark::Ui::Density::Standard;
        float focusRingWidth = 2.0f; // Logical pixels

        EditorThemeData() = default;
    };

    /**
 * @brief Theme registry and the EditorThemeData to Spark UI token / ImGui style adapter.
 */
    class EditorTheme
    {
      public:
        /// @brief Colours for the operating system's window caption (title bar).
        struct NativeCaptionStyle
        {
            ThemeColor caption;   ///< Caption background (Windows 11 DWMWA_CAPTION_COLOR).
            ThemeColor text;      ///< Caption text (Windows 11 DWMWA_TEXT_COLOR).
            bool darkMode = true; ///< Request the dark caption (DWMWA_USE_IMMERSIVE_DARK_MODE).
        };

        /**
     * @brief Apply a theme by name
     * @details Retired names resolve through the alias table; unknown names apply Ember Studio. A
     *          built-in palette is regenerated for the active appearance override and density.
     */
        static bool ApplyTheme(const std::string& themeName);

        /**
     * @brief Apply a theme directly (byte-for-byte; never adjusted by the generator)
     */
        static bool ApplyTheme(const EditorThemeData& theme);

        /**
     * @brief Get list of available theme names (the five picker entries, sorted)
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
        /// Snapshot of the last applied palette, including unregistered custom themes.
        static const EditorThemeData& GetCurrentThemeData();

        /**
     * @brief Register a theme that interpolates every colour and metric of two registered themes
     * @param blend 0 = @p theme1, 1 = @p theme2 (clamped). Flags, strings and enums come from
     *        the nearer theme.
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

        /// @brief Token set equivalent to @p theme: every colour field mapped onto its role, the
        ///        roles without a field (axes, categories, scrim) from the Ember set of the theme's
        ///        appearance, metrics from the theme's style values.
        static Spark::Ui::Tokens ToTokens(const EditorThemeData& theme);

        /// @brief Request a UI scale change (Windows WM_DPICHANGED, SDL display change, user scale).
        /// @details Stored as pending on the current ImGui context and applied by
        ///          ApplyPendingScale() at the start of the next frame, never mid-frame.
        static void SetUiScale(float dpiScale, float userScale);

        /// @brief Effective UI scale of the current context, clamp(dpi x user, 0.75, 3).
        static float GetUiScale();

        /// @brief Apply a pending UI scale change and re-map the current theme onto the rebuilt
        ///        style. Call once per frame before ImGui::NewFrame().
        /// @return True when a pending change was applied.
        static bool ApplyPendingScale();

        /// @brief Set the control density used by the built-in palettes and re-apply the theme.
        static void SetDensity(Spark::Ui::Density density);

        /// @brief Force an appearance (Dark, Light, High Contrast) for the built-in palettes, or
        ///        clear the override (each palette's own appearance), and re-apply the theme.
        static void SetAppearanceOverride(std::optional<Spark::Ui::Appearance> appearance);

        /// @brief Incremented every time a theme is applied; lets window hosts refresh native chrome.
        static std::uint64_t GetRevision();

        /// @brief Native caption colours for @p theme (caption = titleBarActive, text = titleBarText,
        ///        dark mode when the window background is dark).
        static NativeCaptionStyle NativeCaptionStyleFor(const EditorThemeData& theme);

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
     * @brief Create professional light theme (Ember Light tokens)
     */
        static EditorThemeData CreateProfessionalLightTheme();

        /**
     * @brief Create high contrast accessibility theme (Ember High Contrast tokens)
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
        static void ApplyFieldColours(const EditorThemeData& theme, ImGuiStyle& style);
        static void ReapplyCurrentTheme();

        static std::unordered_map<std::string, EditorThemeData> s_registeredThemes;
        static std::string s_currentThemeName;
        static EditorThemeData s_currentThemeData;
        static bool s_currentIsBuiltinPalette;
        static Spark::Ui::Density s_density;
        static std::optional<Spark::Ui::Appearance> s_appearanceOverride;
        static std::uint64_t s_revision;
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
        static bool ExportTheme(const EditorThemeData& theme, const std::string& filepath,
                                Spark::LocalFileCache* cache = nullptr);

        /**
     * @brief Import theme from file
     */
        static bool ImportTheme(const std::string& filepath, EditorThemeData& outTheme,
                                Spark::LocalFileCache* cache = nullptr);

        /**
     * @brief Generate theme variations automatically
     */
        static std::vector<EditorThemeData> GenerateThemeVariations(const EditorThemeData& baseTheme);
    };

} // namespace SparkEditor
