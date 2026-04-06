/**
 * @file UIDesignerSystem.cpp
 * @brief WYSIWYG UI layout editor implementation
 */

#include "UIDesignerSystem.h"

#include <algorithm>

namespace SparkEditor
{

    void UIDesignerSystem::Initialize()
    {
        RegisterBuiltinPresets();
        m_initialized = true;
    }

    void UIDesignerSystem::Shutdown()
    {
        m_screens.clear();
        m_initialized = false;
    }

    uint32_t UIDesignerSystem::CreateScreen(const std::string& name, const std::string& category)
    {
        DesignerScreen screen;
        screen.screenId = m_nextScreenId++;
        screen.name = name;
        screen.category = category;

        // Add root panel
        DesignerWidget root;
        root.widgetId = m_nextWidgetId++;
        root.name = "Root";
        root.type = DesignerWidgetType::Panel;
        root.anchor = DesignerAnchorPreset::StretchAll;
        root.width = screen.designWidth;
        root.height = screen.designHeight;
        screen.widgets.push_back(root);

        m_screens.push_back(std::move(screen));
        return m_screens.back().screenId;
    }

    bool UIDesignerSystem::DeleteScreen(uint32_t screenId)
    {
        auto it = std::find_if(m_screens.begin(), m_screens.end(),
                               [screenId](const DesignerScreen& s) { return s.screenId == screenId; });
        if (it == m_screens.end())
            return false;
        m_screens.erase(it);
        return true;
    }

    bool UIDesignerSystem::ActivateScreen(uint32_t screenId)
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

    DesignerScreen* UIDesignerSystem::GetScreen(uint32_t screenId)
    {
        for (auto& s : m_screens)
        {
            if (s.screenId == screenId)
                return &s;
        }
        return nullptr;
    }

    const DesignerScreen* UIDesignerSystem::GetScreen(uint32_t screenId) const
    {
        for (const auto& s : m_screens)
        {
            if (s.screenId == screenId)
                return &s;
        }
        return nullptr;
    }

    uint32_t UIDesignerSystem::AddWidget(uint32_t screenId, DesignerWidgetType type, const std::string& name,
                                         uint32_t parentId)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return 0;

        DesignerWidget widget;
        widget.widgetId = m_nextWidgetId++;
        widget.name = name;
        widget.type = type;
        widget.parentId = parentId > 0 ? parentId : (screen->widgets.empty() ? 0 : screen->widgets.front().widgetId);
        SetDefaultWidgetSize(widget);

        screen->widgets.push_back(widget);
        return widget.widgetId;
    }

    bool UIDesignerSystem::RemoveWidget(uint32_t screenId, uint32_t widgetId)
    {
        auto* screen = GetScreen(screenId);
        if (!screen || screen->widgets.empty())
            return false;

        // Don't remove root
        if (screen->widgets.front().widgetId == widgetId)
            return false;

        std::erase_if(screen->widgets,
                      [widgetId](const DesignerWidget& w) { return w.widgetId == widgetId || w.parentId == widgetId; });
        return true;
    }

    bool UIDesignerSystem::SetWidgetPosition(uint32_t screenId, uint32_t widgetId, float x, float y)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return false;
        for (auto& w : screen->widgets)
        {
            if (w.widgetId == widgetId)
            {
                w.posX = x;
                w.posY = y;
                return true;
            }
        }
        return false;
    }

    bool UIDesignerSystem::SetWidgetSize(uint32_t screenId, uint32_t widgetId, float w, float h)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return false;
        for (auto& widget : screen->widgets)
        {
            if (widget.widgetId == widgetId)
            {
                widget.width = w;
                widget.height = h;
                return true;
            }
        }
        return false;
    }

    bool UIDesignerSystem::SetWidgetText(uint32_t screenId, uint32_t widgetId, const std::string& text)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return false;
        for (auto& w : screen->widgets)
        {
            if (w.widgetId == widgetId)
            {
                w.text = text;
                return true;
            }
        }
        return false;
    }

    bool UIDesignerSystem::SetWidgetAnchor(uint32_t screenId, uint32_t widgetId, DesignerAnchorPreset anchor)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return false;
        for (auto& w : screen->widgets)
        {
            if (w.widgetId == widgetId)
            {
                w.anchor = anchor;
                return true;
            }
        }
        return false;
    }

    bool UIDesignerSystem::BindWidget(uint32_t screenId, uint32_t widgetId, const std::string& binding)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return false;
        for (auto& w : screen->widgets)
        {
            if (w.widgetId == widgetId)
            {
                w.bindingExpression = binding;
                return true;
            }
        }
        return false;
    }

    uint32_t UIDesignerSystem::CreateStyle(uint32_t screenId, const std::string& name)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return 0;
        DesignerStyle style;
        style.styleId = m_nextStyleId++;
        style.name = name;
        screen->styles.push_back(style);
        return style.styleId;
    }

    bool UIDesignerSystem::ApplyStyle(uint32_t screenId, uint32_t widgetId, const std::string& styleName)
    {
        auto* screen = GetScreen(screenId);
        if (!screen)
            return false;
        for (auto& w : screen->widgets)
        {
            if (w.widgetId == widgetId)
            {
                w.styleName = styleName;
                return true;
            }
        }
        return false;
    }

    uint32_t UIDesignerSystem::CreatePresetHUD()
    {
        uint32_t id = CreateScreen("UI_HUD", "HUD");
        auto* screen = GetScreen(id);
        if (!screen)
            return id;

        uint32_t rootId = screen->widgets.front().widgetId;

        // Health bar
        uint32_t hpPanel = AddWidget(id, DesignerWidgetType::Panel, "HealthPanel", rootId);
        SetWidgetPosition(id, hpPanel, 20.0f, 20.0f);
        SetWidgetSize(id, hpPanel, 300.0f, 40.0f);

        uint32_t hpLabel = AddWidget(id, DesignerWidgetType::Label, "HealthLabel", hpPanel);
        SetWidgetText(id, hpLabel, "HP");
        SetWidgetPosition(id, hpLabel, 0.0f, 5.0f);

        uint32_t hpBar = AddWidget(id, DesignerWidgetType::ProgressBar, "HealthBar", hpPanel);
        SetWidgetPosition(id, hpBar, 40.0f, 10.0f);
        SetWidgetSize(id, hpBar, 250.0f, 20.0f);
        BindWidget(id, hpBar, "Player.Health / Player.MaxHealth");

        // Ammo counter
        uint32_t ammoLabel = AddWidget(id, DesignerWidgetType::Label, "AmmoCounter", rootId);
        SetWidgetText(id, ammoLabel, "30 / 120");
        SetWidgetPosition(id, ammoLabel, 1700.0f, 1000.0f);
        SetWidgetAnchor(id, ammoLabel, DesignerAnchorPreset::BottomRight);
        BindWidget(id, ammoLabel, "Weapon.CurrentAmmo + \" / \" + Weapon.TotalAmmo");

        // Minimap
        uint32_t minimap = AddWidget(id, DesignerWidgetType::Image, "Minimap", rootId);
        SetWidgetPosition(id, minimap, 1720.0f, 20.0f);
        SetWidgetSize(id, minimap, 180.0f, 180.0f);
        SetWidgetAnchor(id, minimap, DesignerAnchorPreset::TopRight);

        // Crosshair
        uint32_t crosshair = AddWidget(id, DesignerWidgetType::Image, "Crosshair", rootId);
        SetWidgetPosition(id, crosshair, 952.0f, 532.0f);
        SetWidgetSize(id, crosshair, 16.0f, 16.0f);
        SetWidgetAnchor(id, crosshair, DesignerAnchorPreset::Center);

        return id;
    }

    uint32_t UIDesignerSystem::CreatePresetMainMenu()
    {
        uint32_t id = CreateScreen("UI_MainMenu", "Menu");
        auto* screen = GetScreen(id);
        if (!screen)
            return id;

        uint32_t rootId = screen->widgets.front().widgetId;

        uint32_t title = AddWidget(id, DesignerWidgetType::Label, "Title", rootId);
        SetWidgetText(id, title, "GAME TITLE");
        SetWidgetPosition(id, title, 760.0f, 200.0f);
        SetWidgetSize(id, title, 400.0f, 80.0f);
        SetWidgetAnchor(id, title, DesignerAnchorPreset::TopCenter);

        uint32_t btnPanel = AddWidget(id, DesignerWidgetType::Panel, "ButtonPanel", rootId);
        SetWidgetPosition(id, btnPanel, 810.0f, 400.0f);
        SetWidgetSize(id, btnPanel, 300.0f, 350.0f);
        SetWidgetAnchor(id, btnPanel, DesignerAnchorPreset::Center);

        const char* buttons[] = {"Play", "Settings", "Credits", "Quit"};
        for (int i = 0; i < 4; i++)
        {
            uint32_t btn = AddWidget(id, DesignerWidgetType::Button, std::string(buttons[i]) + "Button", btnPanel);
            SetWidgetText(id, btn, buttons[i]);
            SetWidgetPosition(id, btn, 75.0f, 20.0f + i * 60.0f);
        }

        uint32_t verLabel = AddWidget(id, DesignerWidgetType::Label, "Version", rootId);
        SetWidgetText(id, verLabel, "v1.0.0");
        SetWidgetPosition(id, verLabel, 1800.0f, 1060.0f);
        SetWidgetAnchor(id, verLabel, DesignerAnchorPreset::BottomRight);

        return id;
    }

    uint32_t UIDesignerSystem::CreatePresetInventory()
    {
        uint32_t id = CreateScreen("UI_Inventory", "Popup");
        auto* screen = GetScreen(id);
        if (!screen)
            return id;

        uint32_t rootId = screen->widgets.front().widgetId;

        uint32_t bg = AddWidget(id, DesignerWidgetType::Panel, "Background", rootId);
        SetWidgetPosition(id, bg, 460.0f, 140.0f);
        SetWidgetSize(id, bg, 1000.0f, 800.0f);
        SetWidgetAnchor(id, bg, DesignerAnchorPreset::Center);

        uint32_t title = AddWidget(id, DesignerWidgetType::Label, "Title", bg);
        SetWidgetText(id, title, "Inventory");
        SetWidgetPosition(id, title, 400.0f, 10.0f);

        uint32_t grid = AddWidget(id, DesignerWidgetType::Panel, "ItemGrid", bg);
        SetWidgetPosition(id, grid, 20.0f, 60.0f);
        SetWidgetSize(id, grid, 600.0f, 700.0f);

        uint32_t detail = AddWidget(id, DesignerWidgetType::Panel, "ItemDetail", bg);
        SetWidgetPosition(id, detail, 640.0f, 60.0f);
        SetWidgetSize(id, detail, 340.0f, 500.0f);

        uint32_t itemName = AddWidget(id, DesignerWidgetType::Label, "ItemName", detail);
        SetWidgetText(id, itemName, "Select an item");
        SetWidgetPosition(id, itemName, 20.0f, 20.0f);

        uint32_t itemImage = AddWidget(id, DesignerWidgetType::Image, "ItemImage", detail);
        SetWidgetPosition(id, itemImage, 70.0f, 60.0f);
        SetWidgetSize(id, itemImage, 200.0f, 200.0f);

        uint32_t closeBtn = AddWidget(id, DesignerWidgetType::Button, "CloseButton", bg);
        SetWidgetText(id, closeBtn, "Close");
        SetWidgetPosition(id, closeBtn, 850.0f, 10.0f);
        SetWidgetSize(id, closeBtn, 100.0f, 35.0f);

        return id;
    }

    void UIDesignerSystem::SetDefaultWidgetSize(DesignerWidget& widget) const
    {
        switch (widget.type)
        {
        case DesignerWidgetType::Button:
            widget.width = 150.0f;
            widget.height = 40.0f;
            widget.text = "Button";
            break;
        case DesignerWidgetType::Label:
            widget.width = 200.0f;
            widget.height = 30.0f;
            widget.text = "Label";
            break;
        case DesignerWidgetType::Image:
            widget.width = 100.0f;
            widget.height = 100.0f;
            break;
        case DesignerWidgetType::ProgressBar:
            widget.width = 200.0f;
            widget.height = 20.0f;
            break;
        case DesignerWidgetType::Slider:
            widget.width = 200.0f;
            widget.height = 25.0f;
            break;
        case DesignerWidgetType::TextField:
            widget.width = 200.0f;
            widget.height = 30.0f;
            break;
        case DesignerWidgetType::Checkbox:
            widget.width = 20.0f;
            widget.height = 20.0f;
            break;
        case DesignerWidgetType::Dropdown:
            widget.width = 150.0f;
            widget.height = 30.0f;
            break;
        case DesignerWidgetType::ListView:
            widget.width = 300.0f;
            widget.height = 400.0f;
            break;
        case DesignerWidgetType::ScrollBox:
            widget.width = 300.0f;
            widget.height = 300.0f;
            break;
        default:
            widget.width = 200.0f;
            widget.height = 200.0f;
            break;
        }
    }

    void UIDesignerSystem::RegisterBuiltinPresets()
    {
        CreatePresetHUD();
        CreatePresetMainMenu();
        CreatePresetInventory();
    }

} // namespace SparkEditor
