/**
 * @file UIDesignerSystem.h
 * @brief WYSIWYG UI layout editor with widget tree and data binding
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a visual UI editor with drag-and-drop widget placement, anchor
 * presets, layout containers, property binding, style system, and
 * screen presets (HUD, MainMenu, Inventory).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SparkEditor
{

    /// @brief UI widget types available in the WYSIWYG editor
    enum class DesignerWidgetType : uint8_t
    {
        Panel = 0,
        Button = 1,
        Label = 2,
        Image = 3,
        ProgressBar = 4,
        Slider = 5,
        TextField = 6,
        Checkbox = 7,
        Dropdown = 8,
        ListView = 9,
        ScrollBox = 10,
        Canvas = 11,
        Count = 12
    };

    /// @brief UI layout modes
    enum class DesignerLayoutMode : uint8_t
    {
        Absolute = 0,
        Horizontal = 1,
        Vertical = 2,
        Grid = 3,
        Wrap = 4,
        Count = 5
    };

    /// @brief UI anchor presets
    enum class DesignerAnchorPreset : uint8_t
    {
        TopLeft = 0,
        TopCenter = 1,
        TopRight = 2,
        CenterLeft = 3,
        Center = 4,
        CenterRight = 5,
        BottomLeft = 6,
        BottomCenter = 7,
        BottomRight = 8,
        StretchHorizontal = 9,
        StretchVertical = 10,
        StretchAll = 11,
        Count = 12
    };

    /// @brief A widget instance in the UI tree
    struct DesignerWidget
    {
        uint32_t widgetId = 0;
        uint32_t parentId = 0; ///< 0 = root
        std::string name;
        DesignerWidgetType type = DesignerWidgetType::Panel;
        DesignerAnchorPreset anchor = DesignerAnchorPreset::TopLeft;
        float posX = 0.0f, posY = 0.0f;
        float width = 100.0f, height = 40.0f;
        bool isVisible = true;
        bool isInteractable = true;
        std::string text;              ///< For Label, Button, TextField
        float fillAmount = 1.0f;       ///< For ProgressBar
        std::string bindingExpression; ///< Data binding path
        std::string styleName;         ///< Style class reference
    };

    /// @brief A UI style definition
    struct DesignerStyle
    {
        uint32_t styleId = 0;
        std::string name;
        float fontSizePx = 16.0f;
        float bgR = 0.2f, bgG = 0.2f, bgB = 0.2f, bgA = 0.8f;
        float fgR = 1.0f, fgG = 1.0f, fgB = 1.0f, fgA = 1.0f;
        float borderRadius = 4.0f;
        float borderWidth = 1.0f;
        float padding = 4.0f;
    };

    /// @brief A complete UI screen/layout
    struct DesignerScreen
    {
        uint32_t screenId = 0;
        std::string name;
        std::string category; ///< "HUD", "Menu", "Popup", "Overlay"
        DesignerLayoutMode rootLayout = DesignerLayoutMode::Absolute;
        float designWidth = 1920.0f;
        float designHeight = 1080.0f;
        std::vector<DesignerWidget> widgets;
        std::vector<DesignerStyle> styles;
        bool isActive = false;
    };

    /**
     * @brief WYSIWYG UI design system for visual interface authoring
     *
     * Manages UI screens with widget trees, layout modes, anchor/pivot
     * positioning, style system, data binding, and preset templates.
     */
    class UIDesignerSystem
    {
      public:
        UIDesignerSystem() = default;
        ~UIDesignerSystem() = default;

        void Initialize();
        void Shutdown();

        // Screen management
        uint32_t CreateScreen(const std::string& name, const std::string& category);
        bool DeleteScreen(uint32_t screenId);
        bool ActivateScreen(uint32_t screenId);
        DesignerScreen* GetScreen(uint32_t screenId);
        const DesignerScreen* GetScreen(uint32_t screenId) const;

        // Widget operations
        uint32_t AddWidget(uint32_t screenId, DesignerWidgetType type, const std::string& name, uint32_t parentId = 0);
        bool RemoveWidget(uint32_t screenId, uint32_t widgetId);
        bool SetWidgetPosition(uint32_t screenId, uint32_t widgetId, float x, float y);
        bool SetWidgetSize(uint32_t screenId, uint32_t widgetId, float w, float h);
        bool SetWidgetText(uint32_t screenId, uint32_t widgetId, const std::string& text);
        bool SetWidgetAnchor(uint32_t screenId, uint32_t widgetId, DesignerAnchorPreset anchor);
        bool BindWidget(uint32_t screenId, uint32_t widgetId, const std::string& binding);

        // Styles
        uint32_t CreateStyle(uint32_t screenId, const std::string& name);
        bool ApplyStyle(uint32_t screenId, uint32_t widgetId, const std::string& styleName);

        // Presets
        uint32_t CreatePresetHUD();
        uint32_t CreatePresetMainMenu();
        uint32_t CreatePresetInventory();

        // Queries
        const std::vector<DesignerScreen>& GetScreens() const { return m_screens; }

      private:
        void RegisterBuiltinPresets();
        void SetDefaultWidgetSize(DesignerWidget& widget) const;

        std::vector<DesignerScreen> m_screens;
        uint32_t m_nextScreenId{1};
        uint32_t m_nextWidgetId{1};
        uint32_t m_nextStyleId{1};
        bool m_initialized{false};
    };

} // namespace SparkEditor
