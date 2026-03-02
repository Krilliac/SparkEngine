/**
 * @file InspectorPanel.cpp
 * @brief Implementation of the Inspector panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor {

InspectorPanel::InspectorPanel() 
    : EditorPanel("Inspector", "inspector_panel") {
}

bool InspectorPanel::Initialize() {
    std::cout << "Initializing Inspector panel\n";
    return true;
}

void InspectorPanel::Update(float deltaTime) {
    // Update inspector panel logic
}

void InspectorPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        if (m_inspectedObject.empty()) {
            float avail = ImGui::GetContentRegionAvail().y;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.4f);
            float textWidth = ImGui::CalcTextSize(ICON_FA_MOUSE_POINTER " Select an object to inspect").x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f), ICON_FA_MOUSE_POINTER " Select an object to inspect");
        } else {
            RenderObjectProperties();
            ImGui::Spacing();
            RenderComponentList();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Full-width "Add Component" button
            float btnWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button(ICON_FA_PLUS " Add Component", ImVec2(btnWidth, 30))) {
                m_showAddComponentMenu = true;
            }

            if (m_showAddComponentMenu) {
                RenderAddComponentMenu();
            }
        }
    }
    EndPanel();
}

void InspectorPanel::Shutdown() {
    std::cout << "Shutting down Inspector panel\n";
}

bool InspectorPanel::HandleEvent(const std::string& eventType, void* eventData) {
    if (eventType == "ObjectSelected" && eventData) {
        SetInspectedObject(*static_cast<std::string*>(eventData));
        return true;
    }
    return false;
}

void InspectorPanel::SetInspectedObject(const std::string& objectId) {
    m_inspectedObject = objectId;
}

void InspectorPanel::RenderObjectProperties() {
    if (ImGui::CollapsingHeader(ICON_FA_CUBE " Object Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(4);

        char nameBuffer[256];
        strncpy_s(nameBuffer, m_inspectedObject.c_str(), sizeof(nameBuffer) - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer))) {
            m_inspectedObject = nameBuffer;
        }

        bool active = true;
        ImGui::Checkbox("Active", &active);
        ImGui::SameLine();
        bool isStatic = false;
        ImGui::Checkbox("Static", &isStatic);

        // Tag and Layer dropdowns
        const char* tags[] = { "Default", "Player", "Enemy", "Weapon", "Terrain", "Trigger" };
        static int currentTag = 0;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::Combo("Tag", &currentTag, tags, IM_ARRAYSIZE(tags));

        const char* layers[] = { "Default", "Ignore Raycast", "Water", "UI", "Ground", "Player" };
        static int currentLayer = 0;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::Combo("Layer", &currentLayer, layers, IM_ARRAYSIZE(layers));

        ImGui::Unindent(4);
    }
}

void InspectorPanel::RenderComponentList() {
    RenderTransformComponent();

    // Mesh Renderer
    if (ImGui::CollapsingHeader(ICON_FA_SHAPES " Mesh Renderer")) {
        ImGui::Indent(4);
        const char* meshes[] = { "Cube", "Sphere", "Cylinder", "Plane", "Custom" };
        static int currentMesh = 0;
        ImGui::Combo("Mesh", &currentMesh, meshes, IM_ARRAYSIZE(meshes));

        const char* materials[] = { "Default", "PBR Standard", "Unlit", "Terrain", "Water" };
        static int currentMaterial = 0;
        ImGui::Combo("Material", &currentMaterial, materials, IM_ARRAYSIZE(materials));

        static bool castShadows = true;
        ImGui::Checkbox("Cast Shadows", &castShadows);
        ImGui::SameLine();
        static bool receiveShadows = true;
        ImGui::Checkbox("Receive Shadows", &receiveShadows);
        ImGui::Unindent(4);
    }

    // Collider
    if (ImGui::CollapsingHeader(ICON_FA_VECTOR_SQUARE " Box Collider")) {
        ImGui::Indent(4);
        static bool isTrigger = false;
        ImGui::Checkbox("Is Trigger", &isTrigger);
        static float center[3] = {0.0f, 0.0f, 0.0f};
        ImGui::DragFloat3("Center", center, 0.1f);
        static float size[3] = {1.0f, 1.0f, 1.0f};
        ImGui::DragFloat3("Size", size, 0.1f);
        ImGui::Unindent(4);
    }

    // Rigidbody
    if (ImGui::CollapsingHeader(ICON_FA_GLOBE " Rigidbody")) {
        ImGui::Indent(4);
        static float mass = 1.0f;
        ImGui::DragFloat("Mass", &mass, 0.1f, 0.01f, 1000.0f);
        static float drag = 0.0f;
        ImGui::DragFloat("Drag", &drag, 0.01f, 0.0f, 100.0f);
        static float angularDrag = 0.05f;
        ImGui::DragFloat("Angular Drag", &angularDrag, 0.01f, 0.0f, 100.0f);
        static bool useGravity = true;
        ImGui::Checkbox("Use Gravity", &useGravity);
        static bool isKinematic = false;
        ImGui::Checkbox("Is Kinematic", &isKinematic);
        ImGui::Unindent(4);
    }
}

static void DrawVec3Control(const char* label, float* values, float resetValue, float speed) {
    ImVec4 xColor(0.9f, 0.2f, 0.2f, 1.0f);
    ImVec4 yColor(0.2f, 0.8f, 0.2f, 1.0f);
    ImVec4 zColor(0.2f, 0.4f, 0.9f, 1.0f);

    ImGui::PushID(label);
    ImGui::Text("%s", label);
    ImGui::SameLine(90);

    float width = (ImGui::GetContentRegionAvail().x - 60) / 3.0f;

    // X
    ImGui::PushStyleColor(ImGuiCol_Button, xColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    if (ImGui::Button("X", ImVec2(20, 20))) values[0] = resetValue;
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(width);
    ImGui::DragFloat("##X", &values[0], speed);
    ImGui::SameLine();

    // Y
    ImGui::PushStyleColor(ImGuiCol_Button, yColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.9f, 0.3f, 1.0f));
    if (ImGui::Button("Y", ImVec2(20, 20))) values[1] = resetValue;
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(width);
    ImGui::DragFloat("##Y", &values[1], speed);
    ImGui::SameLine();

    // Z
    ImGui::PushStyleColor(ImGuiCol_Button, zColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 1.0f, 1.0f));
    if (ImGui::Button("Z", ImVec2(20, 20))) values[2] = resetValue;
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(width);
    ImGui::DragFloat("##Z", &values[2], speed);

    ImGui::PopID();
}

void InspectorPanel::RenderTransformComponent() {
    if (ImGui::CollapsingHeader(ICON_FA_ARROWS_ALT " Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(4);

        static float position[3] = {0.0f, 0.0f, 0.0f};
        static float rotation[3] = {0.0f, 0.0f, 0.0f};
        static float scale[3] = {1.0f, 1.0f, 1.0f};

        DrawVec3Control("Position", position, 0.0f, 0.1f);
        DrawVec3Control("Rotation", rotation, 0.0f, 1.0f);
        DrawVec3Control("Scale", scale, 1.0f, 0.1f);

        ImGui::Unindent(4);
    }
}

void InspectorPanel::RenderAddComponentMenu() {
    if (m_showAddComponentMenu) {
        ImGui::OpenPopup("AddComponentMenu");
    }

    if (ImGui::BeginPopup("AddComponentMenu")) {
        ImGui::Text(ICON_FA_SEARCH " Search Components");
        ImGui::Separator();

        if (ImGui::BeginMenu(ICON_FA_SHAPES " Rendering")) {
            if (ImGui::MenuItem("Mesh Renderer")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Skinned Mesh Renderer")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Particle System")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Trail Renderer")) m_showAddComponentMenu = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_VECTOR_SQUARE " Physics")) {
            if (ImGui::MenuItem("Box Collider")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Sphere Collider")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Capsule Collider")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Rigidbody")) m_showAddComponentMenu = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_CODE " Scripting")) {
            if (ImGui::MenuItem("C++ Script")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Lua Script")) m_showAddComponentMenu = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_VOLUME_UP " Audio")) {
            if (ImGui::MenuItem("Audio Source")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Audio Listener")) m_showAddComponentMenu = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_CROSSHAIRS " FPS")) {
            if (ImGui::MenuItem("Weapon Component")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Health System")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Spawn Point")) m_showAddComponentMenu = false;
            if (ImGui::MenuItem("Damage Volume")) m_showAddComponentMenu = false;
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
}

} // namespace SparkEditor