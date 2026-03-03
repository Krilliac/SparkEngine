/**
 * @file HierarchyPanel.cpp
 * @brief Implementation of the Hierarchy panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "HierarchyPanel.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor {

// File-scope state for rename and clipboard operations
static std::string m_clipboardObject;
static std::string m_renamingTarget;
static bool        m_renameRequested = false;
static char        m_renameInputBuffer[256] = {};

HierarchyPanel::HierarchyPanel() 
    : EditorPanel("Hierarchy", "hierarchy_panel") {
}

bool HierarchyPanel::Initialize() {
    std::cout << "Initializing Hierarchy panel\n";
    
    // Add some test objects for demonstration
    m_sceneObjects.push_back("Main Camera");
    m_sceneObjects.push_back("Directional Light");
    m_sceneObjects.push_back("Player");
    m_sceneObjects.push_back("Ground");
    m_sceneObjects.push_back("Environment");
    
    return true;
}

void HierarchyPanel::Update(float deltaTime) {
    // Update hierarchy panel logic
}

void HierarchyPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        // Hierarchy toolbar
        if (ImGui::Button("Create")) {
            ImGui::OpenPopup("CreateObject");
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            if (!m_selectedObject.empty()) {
                DeleteObject(m_selectedObject);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            if (!m_selectedObject.empty()) {
                DuplicateObject(m_selectedObject);
            }
        }
        
        ImGui::Separator();
        
        // Search filter
        static char searchBuffer[256] = "";
        ImGui::InputTextWithHint("##Search", "Search objects...", searchBuffer, sizeof(searchBuffer));
        
        ImGui::Separator();
        
        // Object tree
        RenderObjectTree();
        
        // Context menus
        RenderContextMenu();
        
        // Create object popup
        if (ImGui::BeginPopup("CreateObject")) {
            if (ImGui::MenuItem("Empty GameObject")) {
                CreateNewObject("Empty");
            }
            if (ImGui::BeginMenu("3D Object")) {
                if (ImGui::MenuItem("Cube")) CreateNewObject("Cube");
                if (ImGui::MenuItem("Sphere")) CreateNewObject("Sphere");
                if (ImGui::MenuItem("Cylinder")) CreateNewObject("Cylinder");
                if (ImGui::MenuItem("Plane")) CreateNewObject("Plane");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Light")) {
                if (ImGui::MenuItem("Directional Light")) CreateNewObject("Directional Light");
                if (ImGui::MenuItem("Point Light")) CreateNewObject("Point Light");
                if (ImGui::MenuItem("Spot Light")) CreateNewObject("Spot Light");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Camera")) CreateNewObject("Camera");
            ImGui::EndPopup();
        }

        // Rename popup
        if (m_renameRequested) {
            ImGui::OpenPopup("RenameObject");
            m_renameRequested = false;
        }

        if (ImGui::BeginPopup("RenameObject")) {
            ImGui::Text("Rename: %s", m_renamingTarget.c_str());
            ImGui::Separator();

            bool confirmed = ImGui::InputText("##RenameInput", m_renameInputBuffer,
                                              sizeof(m_renameInputBuffer),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            // Auto-focus the input the first time the popup opens
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere(-1);
            }

            if (confirmed || ImGui::Button("OK")) {
                std::string newName(m_renameInputBuffer);
                if (!newName.empty() && newName != m_renamingTarget) {
                    auto it = std::find(m_sceneObjects.begin(), m_sceneObjects.end(), m_renamingTarget);
                    if (it != m_sceneObjects.end()) {
                        *it = newName;
                        if (m_selectedObject == m_renamingTarget) {
                            m_selectedObject = newName;
                        }
                        std::cout << "Renamed object: " << m_renamingTarget << " -> " << newName << std::endl;
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
    EndPanel();
}

void HierarchyPanel::Shutdown() {
    std::cout << "Shutting down Hierarchy panel\n";
}

bool HierarchyPanel::HandleEvent(const std::string& eventType, void* eventData) {
    return false;
}

void HierarchyPanel::SetSelectedObject(const std::string& objectId) {
    m_selectedObject = objectId;
}

void HierarchyPanel::RenderObjectTree() {
    for (const auto& object : m_sceneObjects) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        
        if (object == m_selectedObject) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        
        // Object icon
        const char* icon = "??"; // Default icon
        if (object.find("Camera") != std::string::npos) icon = "??";
        else if (object.find("Light") != std::string::npos) icon = "??";
        else if (object.find("Player") != std::string::npos) icon = "??";
        
        ImGui::TreeNodeEx(object.c_str(), flags, "%s %s", icon, object.c_str());
        
        if (ImGui::IsItemClicked()) {
            SetSelectedObject(object);
        }
        
        // Context menu
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            m_contextMenuTarget = object;
            m_showContextMenu = true;
        }
    }
}

void HierarchyPanel::RenderContextMenu() {
    if (m_showContextMenu) {
        ImGui::OpenPopup("ObjectContextMenu");
        m_showContextMenu = false;
    }
    
    if (ImGui::BeginPopup("ObjectContextMenu")) {
        ImGui::Text("Object: %s", m_contextMenuTarget.c_str());
        ImGui::Separator();
        
        if (ImGui::MenuItem("Rename")) {
            m_renamingTarget = m_contextMenuTarget;
            m_renameRequested = true;
            snprintf(m_renameInputBuffer, sizeof(m_renameInputBuffer), "%s", m_contextMenuTarget.c_str());
        }
        if (ImGui::MenuItem("Duplicate")) {
            DuplicateObject(m_contextMenuTarget);
        }
        if (ImGui::MenuItem("Delete")) {
            DeleteObject(m_contextMenuTarget);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy")) {
            m_clipboardObject = m_contextMenuTarget;
            std::cout << "Copied object to clipboard: " << m_contextMenuTarget << std::endl;
        }
        if (ImGui::MenuItem("Paste", nullptr, false, !m_clipboardObject.empty())) {
            // Duplicate the previously-copied entity into the scene
            if (!m_clipboardObject.empty()) {
                DuplicateObject(m_clipboardObject);
                std::cout << "Pasted object from clipboard: " << m_clipboardObject << std::endl;
            }
        }
        
        ImGui::EndPopup();
    }
}

void HierarchyPanel::CreateNewObject(const std::string& type) {
    std::string newObjectName = type + " " + std::to_string(m_sceneObjects.size() + 1);
    m_sceneObjects.push_back(newObjectName);
    SetSelectedObject(newObjectName);
    std::cout << "Created new object: " << newObjectName << std::endl;
}

void HierarchyPanel::DeleteObject(const std::string& objectId) {
    auto it = std::find(m_sceneObjects.begin(), m_sceneObjects.end(), objectId);
    if (it != m_sceneObjects.end()) {
        m_sceneObjects.erase(it);
        if (m_selectedObject == objectId) {
            m_selectedObject.clear();
        }
        std::cout << "Deleted object: " << objectId << std::endl;
    }
}

void HierarchyPanel::DuplicateObject(const std::string& objectId) {
    auto it = std::find(m_sceneObjects.begin(), m_sceneObjects.end(), objectId);
    if (it != m_sceneObjects.end()) {
        std::string duplicatedName = objectId + " (Copy)";
        m_sceneObjects.push_back(duplicatedName);
        SetSelectedObject(duplicatedName);
        std::cout << "Duplicated object: " << objectId << " as " << duplicatedName << std::endl;
    }
}

} // namespace SparkEditor