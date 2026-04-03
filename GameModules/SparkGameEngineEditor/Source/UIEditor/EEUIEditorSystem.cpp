/**
 * @file EEUIEditorSystem.cpp
 * @brief WYSIWYG UI layout editor with widget tree and data binding
 *
 * Implements the visual UI editor with drag-and-drop widget placement,
 * anchor presets, layout containers, style system, and preset templates.
 */

#include "EEUIEditorSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace EngineEditor
{

    bool EEUIEditorSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinPresets();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "UI editor system initialized (%zu screens)", m_screens.size());
        return true;
    }

    void EEUIEditorSystem::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void EEUIEditorSystem::Shutdown()
    {
        m_screens.clear();
        m_initialized = false;
    }

    void EEUIEditorSystem::RenderDebugUI() {}

    uint32_t EEUIEditorSystem::CreateScreen(const std::string& name, const std::string& category)
    {
        UIScreen screen;
        screen.screenId = m_nextScreenId++;
        screen.name = name;
        screen.category = category;

        // Add root panel
        UIWidget root;
        root.widgetId = m_nextWidgetId++;
        root.name = "Root";
        root.type = WidgetType::Panel;
        root.anchor = AnchorPreset::StretchAll;
        root.width = screen.designWidth;
        root.height = screen.designHeight;
        screen.widgets.push_back(root);

        m_screens.push_back(std::move(screen));
        return m_screens.back().screenId;
    }

    bool EEUIEditorSystem::DeleteScreen(uint32_t screenId)
    {
        auto it = std::find_if(m_screens.begin(), m_screens.end(),
                               [screenId](const UIScreen& s) { return s.screenId == screenId; });
        if (it == m_screens.end())
            return false;
        m_screens.erase(it);
        return true;
    }

    bool EEUIEditorSystem::ActivateScreen(uint32_t screenId)
    {
        bool found = false;
        for (auto& s : m_screens)
        {
            if (s.screenId == screenId)
            {
                s.isActive = true;
                found = true;
            }
            else
            {
                s.isActive = false;
            }
        }
        return found;
    }

    uint32_t EEUIEditorSystem::AddWidget(uint32_t screenId, WidgetType type, const std::string& name, uint32_t parentId)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                UIWidget widget;
                widget.widgetId = m_nextWidgetId++;
                widget.name = name;
                widget.type = type;
                widget.parentId = parentId > 0 ? parentId : screen.widgets.front().widgetId;

                // Default sizes by type
                switch (type)
                {
                case WidgetType::Button:
                    widget.width = 150.0f;
                    widget.height = 40.0f;
                    widget.text = "Button";
                    break;
                case WidgetType::Label:
                    widget.width = 200.0f;
                    widget.height = 30.0f;
                    widget.text = "Label";
                    break;
                case WidgetType::Image:
                    widget.width = 100.0f;
                    widget.height = 100.0f;
                    break;
                case WidgetType::ProgressBar:
                    widget.width = 200.0f;
                    widget.height = 20.0f;
                    break;
                case WidgetType::Slider:
                    widget.width = 200.0f;
                    widget.height = 25.0f;
                    break;
                case WidgetType::TextField:
                    widget.width = 200.0f;
                    widget.height = 30.0f;
                    break;
                case WidgetType::Checkbox:
                    widget.width = 20.0f;
                    widget.height = 20.0f;
                    break;
                case WidgetType::Dropdown:
                    widget.width = 150.0f;
                    widget.height = 30.0f;
                    break;
                case WidgetType::ListView:
                    widget.width = 300.0f;
                    widget.height = 400.0f;
                    break;
                case WidgetType::ScrollBox:
                    widget.width = 300.0f;
                    widget.height = 300.0f;
                    break;
                default:
                    widget.width = 200.0f;
                    widget.height = 200.0f;
                    break;
                }

                screen.widgets.push_back(widget);
                return widget.widgetId;
            }
        }
        return 0;
    }

    bool EEUIEditorSystem::RemoveWidget(uint32_t screenId, uint32_t widgetId)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                // Don't remove root
                if (!screen.widgets.empty() && screen.widgets.front().widgetId == widgetId)
                    return false;

                // Remove widget and its children
                std::erase_if(screen.widgets, [widgetId](const UIWidget& w)
                              { return w.widgetId == widgetId || w.parentId == widgetId; });
                return true;
            }
        }
        return false;
    }

    bool EEUIEditorSystem::SetWidgetPosition(uint32_t screenId, uint32_t widgetId, float x, float y)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                for (auto& w : screen.widgets)
                {
                    if (w.widgetId == widgetId)
                    {
                        w.posX = x;
                        w.posY = y;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool EEUIEditorSystem::SetWidgetSize(uint32_t screenId, uint32_t widgetId, float w, float h)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                for (auto& widget : screen.widgets)
                {
                    if (widget.widgetId == widgetId)
                    {
                        widget.width = w;
                        widget.height = h;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool EEUIEditorSystem::SetWidgetText(uint32_t screenId, uint32_t widgetId, const std::string& text)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                for (auto& w : screen.widgets)
                {
                    if (w.widgetId == widgetId)
                    {
                        w.text = text;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool EEUIEditorSystem::SetWidgetAnchor(uint32_t screenId, uint32_t widgetId, AnchorPreset anchor)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                for (auto& w : screen.widgets)
                {
                    if (w.widgetId == widgetId)
                    {
                        w.anchor = anchor;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool EEUIEditorSystem::BindWidget(uint32_t screenId, uint32_t widgetId, const std::string& binding)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                for (auto& w : screen.widgets)
                {
                    if (w.widgetId == widgetId)
                    {
                        w.bindingExpression = binding;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    uint32_t EEUIEditorSystem::CreateStyle(uint32_t screenId, const std::string& name)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                UIStyle style;
                style.styleId = m_nextStyleId++;
                style.name = name;
                screen.styles.push_back(style);
                return style.styleId;
            }
        }
        return 0;
    }

    bool EEUIEditorSystem::ApplyStyle(uint32_t screenId, uint32_t widgetId, const std::string& styleName)
    {
        for (auto& screen : m_screens)
        {
            if (screen.screenId == screenId)
            {
                for (auto& w : screen.widgets)
                {
                    if (w.widgetId == widgetId)
                    {
                        w.styleName = styleName;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    uint32_t EEUIEditorSystem::CreatePresetHUD(const std::string& name)
    {
        uint32_t id = CreateScreen(name, "HUD");
        for (auto& screen : m_screens)
        {
            if (screen.screenId == id)
            {
                uint32_t rootId = screen.widgets.front().widgetId;

                // Health bar
                uint32_t hpPanel = AddWidget(id, WidgetType::Panel, "HealthPanel", rootId);
                SetWidgetPosition(id, hpPanel, 20.0f, 20.0f);
                SetWidgetSize(id, hpPanel, 300.0f, 40.0f);
                uint32_t hpLabel = AddWidget(id, WidgetType::Label, "HealthLabel", hpPanel);
                SetWidgetText(id, hpLabel, "HP");
                SetWidgetPosition(id, hpLabel, 0.0f, 5.0f);
                uint32_t hpBar = AddWidget(id, WidgetType::ProgressBar, "HealthBar", hpPanel);
                SetWidgetPosition(id, hpBar, 40.0f, 10.0f);
                SetWidgetSize(id, hpBar, 250.0f, 20.0f);
                BindWidget(id, hpBar, "Player.Health / Player.MaxHealth");

                // Ammo counter
                uint32_t ammoLabel = AddWidget(id, WidgetType::Label, "AmmoCounter", rootId);
                SetWidgetText(id, ammoLabel, "30 / 120");
                SetWidgetPosition(id, ammoLabel, 1700.0f, 1000.0f);
                SetWidgetAnchor(id, ammoLabel, AnchorPreset::BottomRight);
                BindWidget(id, ammoLabel, "Weapon.CurrentAmmo + \" / \" + Weapon.TotalAmmo");

                // Minimap
                uint32_t minimap = AddWidget(id, WidgetType::Image, "Minimap", rootId);
                SetWidgetPosition(id, minimap, 1720.0f, 20.0f);
                SetWidgetSize(id, minimap, 180.0f, 180.0f);
                SetWidgetAnchor(id, minimap, AnchorPreset::TopRight);

                // Crosshair
                uint32_t crosshair = AddWidget(id, WidgetType::Image, "Crosshair", rootId);
                SetWidgetPosition(id, crosshair, 952.0f, 532.0f);
                SetWidgetSize(id, crosshair, 16.0f, 16.0f);
                SetWidgetAnchor(id, crosshair, AnchorPreset::Center);

                break;
            }
        }
        return id;
    }

    uint32_t EEUIEditorSystem::CreatePresetMainMenu(const std::string& name)
    {
        uint32_t id = CreateScreen(name, "Menu");
        for (auto& screen : m_screens)
        {
            if (screen.screenId == id)
            {
                uint32_t rootId = screen.widgets.front().widgetId;

                // Title
                uint32_t title = AddWidget(id, WidgetType::Label, "Title", rootId);
                SetWidgetText(id, title, "GAME TITLE");
                SetWidgetPosition(id, title, 760.0f, 200.0f);
                SetWidgetSize(id, title, 400.0f, 80.0f);
                SetWidgetAnchor(id, title, AnchorPreset::TopCenter);

                // Button panel
                uint32_t btnPanel = AddWidget(id, WidgetType::Panel, "ButtonPanel", rootId);
                SetWidgetPosition(id, btnPanel, 810.0f, 400.0f);
                SetWidgetSize(id, btnPanel, 300.0f, 350.0f);
                SetWidgetAnchor(id, btnPanel, AnchorPreset::Center);

                uint32_t playBtn = AddWidget(id, WidgetType::Button, "PlayButton", btnPanel);
                SetWidgetText(id, playBtn, "Play");
                SetWidgetPosition(id, playBtn, 75.0f, 20.0f);

                uint32_t settingsBtn = AddWidget(id, WidgetType::Button, "SettingsButton", btnPanel);
                SetWidgetText(id, settingsBtn, "Settings");
                SetWidgetPosition(id, settingsBtn, 75.0f, 80.0f);

                uint32_t creditsBtn = AddWidget(id, WidgetType::Button, "CreditsButton", btnPanel);
                SetWidgetText(id, creditsBtn, "Credits");
                SetWidgetPosition(id, creditsBtn, 75.0f, 140.0f);

                uint32_t quitBtn = AddWidget(id, WidgetType::Button, "QuitButton", btnPanel);
                SetWidgetText(id, quitBtn, "Quit");
                SetWidgetPosition(id, quitBtn, 75.0f, 200.0f);

                // Version label
                uint32_t verLabel = AddWidget(id, WidgetType::Label, "Version", rootId);
                SetWidgetText(id, verLabel, "v1.0.0");
                SetWidgetPosition(id, verLabel, 1800.0f, 1060.0f);
                SetWidgetAnchor(id, verLabel, AnchorPreset::BottomRight);

                break;
            }
        }
        return id;
    }

    uint32_t EEUIEditorSystem::CreatePresetInventory(const std::string& name)
    {
        uint32_t id = CreateScreen(name, "Popup");
        for (auto& screen : m_screens)
        {
            if (screen.screenId == id)
            {
                uint32_t rootId = screen.widgets.front().widgetId;

                // Background panel
                uint32_t bg = AddWidget(id, WidgetType::Panel, "Background", rootId);
                SetWidgetPosition(id, bg, 460.0f, 140.0f);
                SetWidgetSize(id, bg, 1000.0f, 800.0f);
                SetWidgetAnchor(id, bg, AnchorPreset::Center);

                // Title
                uint32_t title = AddWidget(id, WidgetType::Label, "Title", bg);
                SetWidgetText(id, title, "Inventory");
                SetWidgetPosition(id, title, 400.0f, 10.0f);

                // Item grid
                uint32_t grid = AddWidget(id, WidgetType::Panel, "ItemGrid", bg);
                SetWidgetPosition(id, grid, 20.0f, 60.0f);
                SetWidgetSize(id, grid, 600.0f, 700.0f);

                // Item detail panel
                uint32_t detail = AddWidget(id, WidgetType::Panel, "ItemDetail", bg);
                SetWidgetPosition(id, detail, 640.0f, 60.0f);
                SetWidgetSize(id, detail, 340.0f, 500.0f);

                uint32_t itemName = AddWidget(id, WidgetType::Label, "ItemName", detail);
                SetWidgetText(id, itemName, "Select an item");
                SetWidgetPosition(id, itemName, 20.0f, 20.0f);

                uint32_t itemImage = AddWidget(id, WidgetType::Image, "ItemImage", detail);
                SetWidgetPosition(id, itemImage, 70.0f, 60.0f);
                SetWidgetSize(id, itemImage, 200.0f, 200.0f);

                uint32_t itemDesc = AddWidget(id, WidgetType::Label, "ItemDescription", detail);
                SetWidgetPosition(id, itemDesc, 20.0f, 280.0f);
                SetWidgetSize(id, itemDesc, 300.0f, 100.0f);

                // Close button
                uint32_t closeBtn = AddWidget(id, WidgetType::Button, "CloseButton", bg);
                SetWidgetText(id, closeBtn, "Close");
                SetWidgetPosition(id, closeBtn, 850.0f, 10.0f);
                SetWidgetSize(id, closeBtn, 100.0f, 35.0f);

                break;
            }
        }
        return id;
    }

    std::string EEUIEditorSystem::GetScreenListString() const
    {
        std::string s = "=== UI Screens ===\n";
        for (const auto& scr : m_screens)
        {
            s += "  [" + std::to_string(scr.screenId) + "] " + scr.name;
            s += " (" + scr.category + ", " + std::to_string(scr.widgets.size()) + " widgets";
            if (scr.isActive)
                s += ", ACTIVE";
            s += ")\n";
        }
        if (m_screens.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEUIEditorSystem::GetScreenDetailString(uint32_t screenId) const
    {
        for (const auto& scr : m_screens)
        {
            if (scr.screenId == screenId)
            {
                std::string s = "=== Screen: " + scr.name + " ===\n";
                s += "Category: " + scr.category + "\n";
                s += "Design: " + std::to_string(static_cast<int>(scr.designWidth)) + "x" +
                     std::to_string(static_cast<int>(scr.designHeight)) + "\n";
                s += "Widgets: " + std::to_string(scr.widgets.size()) + "\n";
                for (const auto& w : scr.widgets)
                {
                    s += "  [" + std::to_string(w.widgetId) + "] " + w.name +
                         " (type=" + std::to_string(static_cast<int>(w.type));
                    if (!w.text.empty())
                        s += " text=\"" + w.text + "\"";
                    if (!w.bindingExpression.empty())
                        s += " bind=\"" + w.bindingExpression + "\"";
                    s += ")\n";
                }
                s += "Styles: " + std::to_string(scr.styles.size()) + "\n";
                return s;
            }
        }
        return "Screen not found";
    }

    std::string EEUIEditorSystem::GetWidgetCatalogString() const
    {
        return "Widget Types: Panel, Button, Label, Image, ProgressBar, Slider, "
               "TextField, Checkbox, Dropdown, ListView, ScrollBox, Canvas\n"
               "Anchors: TopLeft/Center/Right, CenterLeft/Center/Right, "
               "BottomLeft/Center/Right, StretchH/V/All\n";
    }

    void EEUIEditorSystem::RegisterBuiltinPresets()
    {
        CreatePresetHUD("UI_HUD");
        CreatePresetMainMenu("UI_MainMenu");
        CreatePresetInventory("UI_Inventory");
    }

} // namespace EngineEditor
