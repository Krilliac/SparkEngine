/**
 * @file EEUIEditorSystem.h
 * @brief WYSIWYG UI layout editor with widget tree and data binding
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a visual UI editor with drag-and-drop widget placement, anchor
 * presets, layout containers, property binding, and screen preview.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief A widget instance in the UI tree
    struct UIWidget
    {
        uint32_t widgetId = 0;
        uint32_t parentId = 0; ///< 0 = root
        std::string name;
        WidgetType type = WidgetType::Panel;
        AnchorPreset anchor = AnchorPreset::TopLeft;
        float posX = 0.0f, posY = 0.0f;
        float width = 100.0f, height = 40.0f;
        float pivotX = 0.0f, pivotY = 0.0f;
        bool isVisible = true;
        bool isInteractable = true;
        std::string text;              ///< For Label, Button, TextField
        float fillAmount = 1.0f;       ///< For ProgressBar
        std::string bindingExpression; ///< Data binding path
        std::string styleName;         ///< Style class reference
    };

    /// @brief A UI style definition
    struct UIStyle
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
    struct UIScreen
    {
        uint32_t screenId = 0;
        std::string name;
        std::string category; ///< "HUD", "Menu", "Popup", "Overlay"
        LayoutMode rootLayout = LayoutMode::Absolute;
        float designWidth = 1920.0f;
        float designHeight = 1080.0f;
        std::vector<UIWidget> widgets;
        std::vector<UIStyle> styles;
        bool isActive = false;
    };

    /**
     * @brief WYSIWYG UI editor for no-code interface design
     *
     * Manages UI screens with widget trees, layout modes, anchor/pivot
     * positioning, style system, data binding, and preset templates.
     */
    class EEUIEditorSystem
    {
      public:
        EEUIEditorSystem() = default;
        ~EEUIEditorSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Screen management
        uint32_t CreateScreen(const std::string& name, const std::string& category);
        bool DeleteScreen(uint32_t screenId);
        bool ActivateScreen(uint32_t screenId);

        // Widget operations
        uint32_t AddWidget(uint32_t screenId, WidgetType type, const std::string& name, uint32_t parentId = 0);
        bool RemoveWidget(uint32_t screenId, uint32_t widgetId);
        bool SetWidgetPosition(uint32_t screenId, uint32_t widgetId, float x, float y);
        bool SetWidgetSize(uint32_t screenId, uint32_t widgetId, float w, float h);
        bool SetWidgetText(uint32_t screenId, uint32_t widgetId, const std::string& text);
        bool SetWidgetAnchor(uint32_t screenId, uint32_t widgetId, AnchorPreset anchor);
        bool BindWidget(uint32_t screenId, uint32_t widgetId, const std::string& binding);

        // Styles
        uint32_t CreateStyle(uint32_t screenId, const std::string& name);
        bool ApplyStyle(uint32_t screenId, uint32_t widgetId, const std::string& styleName);

        // Presets
        uint32_t CreatePresetHUD(const std::string& name);
        uint32_t CreatePresetMainMenu(const std::string& name);
        uint32_t CreatePresetInventory(const std::string& name);

        // Queries
        size_t GetScreenCount() const { return m_screens.size(); }
        std::string GetScreenListString() const;
        std::string GetScreenDetailString(uint32_t screenId) const;
        std::string GetWidgetCatalogString() const;

      private:
        void RegisterBuiltinPresets();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<UIScreen> m_screens;
        uint32_t m_nextScreenId{1};
        uint32_t m_nextWidgetId{1};
        uint32_t m_nextStyleId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
