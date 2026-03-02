/**
 * @file SimpleHierarchyPanel.cpp
 * @brief Simple hierarchy panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "SimpleHierarchyPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace SparkEditor {

SimpleHierarchyPanel::SimpleHierarchyPanel() 
    : EditorPanel("Hierarchy", "simple_hierarchy_panel") {
}

SimpleHierarchyPanel::~SimpleHierarchyPanel() {
}

bool SimpleHierarchyPanel::Initialize() {
    std::cout << "Initializing Simple Hierarchy panel\n";
    
    // Add some default objects
    m_sceneObjects.push_back("Main Camera");
    m_sceneObjects.push_back("Directional Light");
    m_sceneObjects.push_back("Ground Plane");
    m_sceneObjects.push_back("Player");
    
    return true;
}

void SimpleHierarchyPanel::Update(float deltaTime) {
    // Simple hierarchy update
}

void SimpleHierarchyPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        // Toolbar with icon buttons
        if (ImGui::Button(ICON_FA_PLUS " Create")) {
            ImGui::OpenPopup("CreateObject");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Delete") && !m_selectedObject.empty()) {
            DeleteObject(m_selectedObject);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY " Duplicate") && !m_selectedObject.empty()) {
            CreateObject(m_selectedObject);
        }

        // Search filter bar
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##HierarchySearch", ICON_FA_SEARCH " Search objects...", m_searchFilter, sizeof(m_searchFilter));

        ImGui::Separator();

        // Object list
        ImGui::BeginChild("##ObjectList");
        for (size_t i = 0; i < m_sceneObjects.size(); ++i) {
            const auto& object = m_sceneObjects[i];

            // Apply search filter
            if (m_searchFilter[0] != '\0') {
                std::string lower = object;
                std::string filter = m_searchFilter;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
                if (lower.find(filter) == std::string::npos) continue;
            }

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;

            if (object == m_selectedObject) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }

            // Object icon based on type (FontAwesome)
            const char* icon = ICON_FA_CUBE;
            if (object.find("Camera") != std::string::npos) icon = ICON_FA_CAMERA;
            else if (object.find("Light") != std::string::npos) icon = ICON_FA_LIGHTBULB;
            else if (object.find("Player") != std::string::npos) icon = ICON_FA_CROSSHAIRS;
            else if (object.find("Plane") != std::string::npos || object.find("Ground") != std::string::npos) icon = ICON_FA_VECTOR_SQUARE;
            else if (object.find("Sphere") != std::string::npos) icon = ICON_FA_CIRCLE;

            ImGui::PushID(static_cast<int>(i));
            ImGui::TreeNodeEx("##obj", flags, "%s  %s", icon, object.c_str());

            if (ImGui::IsItemClicked()) {
                m_selectedObject = object;
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem("##ObjContext")) {
                m_selectedObject = object;
                if (ImGui::MenuItem(ICON_FA_COPY " Duplicate")) {
                    CreateObject(object);
                }
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete")) {
                    DeleteObject(object);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_EYE " Toggle Visibility")) {
                    // Toggle visibility placeholder
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Create object popup
        if (ImGui::BeginPopup("CreateObject")) {
            if (ImGui::MenuItem(ICON_FA_CUBE " Empty GameObject")) {
                CreateObject("Empty GameObject");
            }
            if (ImGui::BeginMenu(ICON_FA_SHAPES " 3D Object")) {
                if (ImGui::MenuItem(ICON_FA_CUBE " Cube")) CreateObject("Cube");
                if (ImGui::MenuItem(ICON_FA_CIRCLE " Sphere")) CreateObject("Sphere");
                if (ImGui::MenuItem(ICON_FA_VECTOR_SQUARE " Plane")) CreateObject("Plane");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Light")) {
                if (ImGui::MenuItem("Directional Light")) CreateObject("Directional Light");
                if (ImGui::MenuItem("Point Light")) CreateObject("Point Light");
                if (ImGui::MenuItem("Spot Light")) CreateObject("Spot Light");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_CROSSHAIRS " FPS")) {
                if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Player Spawn")) CreateObject("Player Spawn");
                if (ImGui::MenuItem(ICON_FA_FLAG " Capture Point")) CreateObject("Capture Point");
                if (ImGui::MenuItem(ICON_FA_SHIELD " Cover Point")) CreateObject("Cover Point");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(ICON_FA_CAMERA " Camera")) CreateObject("Camera");
            ImGui::EndPopup();
        }
    }
    EndPanel();
}

void SimpleHierarchyPanel::Shutdown() {
    std::cout << "Shutting down Simple Hierarchy panel\n";
}

bool SimpleHierarchyPanel::HandleEvent(const std::string& eventType, void* eventData) {
    return false;
}

void SimpleHierarchyPanel::CreateObject(const std::string& objectType) {
    std::string newObjectName = objectType + " " + std::to_string(m_sceneObjects.size() + 1);
    m_sceneObjects.push_back(newObjectName);
    m_selectedObject = newObjectName;
    std::cout << "Created object: " << newObjectName << "\n";
}

void SimpleHierarchyPanel::DeleteObject(const std::string& objectName) {
    auto it = std::find(m_sceneObjects.begin(), m_sceneObjects.end(), objectName);
    if (it != m_sceneObjects.end()) {
        m_sceneObjects.erase(it);
        if (m_selectedObject == objectName) {
            m_selectedObject.clear();
        }
        std::cout << "Deleted object: " << objectName << "\n";
    }
}

const std::string& SimpleHierarchyPanel::GetSelectedObject() const {
    return m_selectedObject;
}

void SimpleHierarchyPanel::SetSelectedObject(const std::string& objectName) {
    m_selectedObject = objectName;
}

} // namespace SparkEditor