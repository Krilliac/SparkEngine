/**
 * @file UIDesignerPanel.cpp
 * @brief WYSIWYG UI layout editor panel
 */

#include "UIDesignerPanel.h"

#include <imgui.h>
#include <string>

namespace SparkEditor
{

    static const char* kWidgetTypeNames[] = {"Panel",     "Button",   "Label",    "Image",    "Progress",  "Slider",
                                             "TextField", "Checkbox", "Dropdown", "ListView", "ScrollBox", "Canvas"};

    static const char* kAnchorNames[] = {"TopLeft", "TopCenter", "TopRight", "CenterL",  "Center",   "CenterR",
                                         "BottomL", "BottomC",   "BottomR",  "StretchH", "StretchV", "StretchAll"};

    UIDesignerPanel::UIDesignerPanel() : EditorPanel("UI Designer", "UIDesigner") {}

    bool UIDesignerPanel::Initialize()
    {
        m_system = std::make_unique<UIDesignerSystem>();
        m_system->Initialize();
        m_isInitialized = true;

        // Select first screen if available
        const auto& screens = m_system->GetScreens();
        if (!screens.empty())
            m_selectedScreenId = screens.front().screenId;

        return true;
    }

    // Intentional: deltaTime reserved for future UI animation preview
    void UIDesignerPanel::Update([[maybe_unused]] float deltaTime) {}

    void UIDesignerPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderPresetButtons();
        ImGui::Separator();

        // Two-column layout: left = screen list + widget tree, right = inspector + preview
        float panelWidth = ImGui::GetContentRegionAvail().x;
        float leftWidth = panelWidth * 0.4f;

        ImGui::BeginChild("LeftPane", ImVec2(leftWidth, 0), true);
        RenderScreenList();
        ImGui::Separator();
        RenderWidgetTree();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("RightPane", ImVec2(0, 0), true);
        RenderWidgetInspector();
        ImGui::Separator();
        RenderCanvasPreview();
        ImGui::EndChild();

        EndPanel();
    }

    void UIDesignerPanel::Shutdown()
    {
        if (m_system)
            m_system->Shutdown();
    }

    void UIDesignerPanel::RenderScreenList()
    {
        ImGui::Text("Screens");

        const auto& screens = m_system->GetScreens();
        for (const auto& s : screens)
        {
            bool selected = (m_selectedScreenId == s.screenId);
            std::string label = s.name + " [" + s.category + "]";
            if (s.isActive)
                label += " *";
            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedScreenId = s.screenId;
                m_selectedWidgetId = 0;
            }
        }

        // New screen
        static char newName[64] = "";
        static char newCategory[32] = "HUD";
        ImGui::InputText("Name##scr", newName, sizeof(newName));
        ImGui::InputText("Category##scr", newCategory, sizeof(newCategory));
        if (ImGui::Button("New Screen") && newName[0] != '\0')
        {
            uint32_t id = m_system->CreateScreen(newName, newCategory);
            m_selectedScreenId = id;
            newName[0] = '\0';
            SetModified(true);
        }
    }

    void UIDesignerPanel::RenderWidgetTree()
    {
        ImGui::Text("Widget Tree");

        const auto* screen = m_system->GetScreen(m_selectedScreenId);
        if (!screen)
        {
            ImGui::TextDisabled("No screen selected");
            return;
        }

        for (const auto& w : screen->widgets)
        {
            bool selected = (m_selectedWidgetId == w.widgetId);
            int typeIdx = static_cast<int>(w.type);
            const char* typeName = (typeIdx >= 0 && typeIdx < static_cast<int>(DesignerWidgetType::Count))
                                       ? kWidgetTypeNames[typeIdx]
                                       : "?";

            std::string label = w.name + " (" + typeName + ")";
            if (w.parentId == 0)
                label = "[ROOT] " + label;

            if (ImGui::Selectable(label.c_str(), selected))
                m_selectedWidgetId = w.widgetId;
        }

        // Add widget
        static int addTypeIdx = 0;
        static char widgetName[64] = "";
        ImGui::Combo("Type##add", &addTypeIdx, kWidgetTypeNames, static_cast<int>(DesignerWidgetType::Count));
        ImGui::InputText("Name##wid", widgetName, sizeof(widgetName));
        if (ImGui::Button("Add Widget") && widgetName[0] != '\0')
        {
            auto type = static_cast<DesignerWidgetType>(addTypeIdx);
            uint32_t parentId = m_selectedWidgetId > 0 ? m_selectedWidgetId : 0;
            uint32_t id = m_system->AddWidget(m_selectedScreenId, type, widgetName, parentId);
            m_selectedWidgetId = id;
            widgetName[0] = '\0';
            SetModified(true);
        }

        if (m_selectedWidgetId > 0)
        {
            ImGui::SameLine();
            if (ImGui::Button("Remove"))
            {
                m_system->RemoveWidget(m_selectedScreenId, m_selectedWidgetId);
                m_selectedWidgetId = 0;
                SetModified(true);
            }
        }
    }

    void UIDesignerPanel::RenderWidgetInspector()
    {
        ImGui::Text("Inspector");

        auto* screen = m_system->GetScreen(m_selectedScreenId);
        if (!screen || m_selectedWidgetId == 0)
        {
            ImGui::TextDisabled("Select a widget");
            return;
        }

        // Find the selected widget
        DesignerWidget* widget = nullptr;
        for (auto& w : screen->widgets)
        {
            if (w.widgetId == m_selectedWidgetId)
            {
                widget = &w;
                break;
            }
        }
        if (!widget)
            return;

        ImGui::Text("ID: %u  Type: %s", widget->widgetId, kWidgetTypeNames[static_cast<int>(widget->type)]);

        // Position
        float pos[2] = {widget->posX, widget->posY};
        if (ImGui::DragFloat2("Position", pos, 1.0f))
        {
            m_system->SetWidgetPosition(m_selectedScreenId, m_selectedWidgetId, pos[0], pos[1]);
            SetModified(true);
        }

        // Size
        float size[2] = {widget->width, widget->height};
        if (ImGui::DragFloat2("Size", size, 1.0f, 1.0f, 4096.0f))
        {
            m_system->SetWidgetSize(m_selectedScreenId, m_selectedWidgetId, size[0], size[1]);
            SetModified(true);
        }

        // Anchor
        int anchorIdx = static_cast<int>(widget->anchor);
        if (ImGui::Combo("Anchor", &anchorIdx, kAnchorNames, static_cast<int>(DesignerAnchorPreset::Count)))
        {
            m_system->SetWidgetAnchor(m_selectedScreenId, m_selectedWidgetId,
                                      static_cast<DesignerAnchorPreset>(anchorIdx));
            SetModified(true);
        }

        // Text (for text-capable widgets)
        if (widget->type == DesignerWidgetType::Label || widget->type == DesignerWidgetType::Button ||
            widget->type == DesignerWidgetType::TextField)
        {
            char textBuf[256];
            strncpy(textBuf, widget->text.c_str(), sizeof(textBuf) - 1);
            textBuf[sizeof(textBuf) - 1] = '\0';
            if (ImGui::InputText("Text", textBuf, sizeof(textBuf)))
            {
                m_system->SetWidgetText(m_selectedScreenId, m_selectedWidgetId, textBuf);
                SetModified(true);
            }
        }

        // Data binding
        char bindBuf[256];
        strncpy(bindBuf, widget->bindingExpression.c_str(), sizeof(bindBuf) - 1);
        bindBuf[sizeof(bindBuf) - 1] = '\0';
        if (ImGui::InputText("Binding", bindBuf, sizeof(bindBuf)))
        {
            m_system->BindWidget(m_selectedScreenId, m_selectedWidgetId, bindBuf);
            SetModified(true);
        }

        ImGui::Checkbox("Visible", &widget->isVisible);
        ImGui::Checkbox("Interactable", &widget->isInteractable);
    }

    void UIDesignerPanel::RenderCanvasPreview()
    {
        ImGui::Text("Canvas Preview");

        const auto* screen = m_system->GetScreen(m_selectedScreenId);
        if (!screen)
        {
            ImGui::TextDisabled("No screen selected");
            return;
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float scale = std::min(avail.x / screen->designWidth, avail.y / screen->designHeight);
        scale = std::min(scale, 0.3f);

        ImVec2 canvasSize(screen->designWidth * scale, screen->designHeight * scale);
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 30, 255));
        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                          IM_COL32(100, 100, 100, 255));

        // Draw widgets
        for (const auto& w : screen->widgets)
        {
            if (w.parentId == 0 && w.type == DesignerWidgetType::Panel)
                continue; // Skip root

            float wx = canvasPos.x + w.posX * scale;
            float wy = canvasPos.y + w.posY * scale;
            float ww = w.width * scale;
            float wh = w.height * scale;

            ImU32 fillCol =
                (w.widgetId == m_selectedWidgetId) ? IM_COL32(80, 120, 200, 100) : IM_COL32(60, 60, 60, 100);
            ImU32 borderCol =
                (w.widgetId == m_selectedWidgetId) ? IM_COL32(100, 150, 255, 255) : IM_COL32(120, 120, 120, 200);

            drawList->AddRectFilled(ImVec2(wx, wy), ImVec2(wx + ww, wy + wh), fillCol);
            drawList->AddRect(ImVec2(wx, wy), ImVec2(wx + ww, wy + wh), borderCol);

            if (ww > 20.0f && wh > 10.0f)
            {
                std::string label = w.name;
                if (label.size() > 10)
                    label = label.substr(0, 10) + "..";
                drawList->AddText(ImVec2(wx + 2.0f, wy + 1.0f), IM_COL32(200, 200, 200, 255), label.c_str());
            }
        }

        ImGui::Dummy(canvasSize);
        ImGui::Text("%.0fx%.0f (%.0f%%)", screen->designWidth, screen->designHeight, scale * 100.0f);
    }

    void UIDesignerPanel::RenderPresetButtons()
    {
        if (ImGui::Button("+ HUD Preset"))
        {
            m_selectedScreenId = m_system->CreatePresetHUD();
            SetModified(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Menu Preset"))
        {
            m_selectedScreenId = m_system->CreatePresetMainMenu();
            SetModified(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Inventory Preset"))
        {
            m_selectedScreenId = m_system->CreatePresetInventory();
            SetModified(true);
        }
    }

} // namespace SparkEditor
