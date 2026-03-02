/**
 * @file SceneViewPanel.cpp
 * @brief Implementation of the Scene View panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneViewPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor {

SceneViewPanel::SceneViewPanel() 
    : EditorPanel("Scene View", "scene_view_panel") {
}

bool SceneViewPanel::Initialize() {
    std::cout << "Initializing Scene View panel\n";
    return true;
}

void SceneViewPanel::Update(float deltaTime) {
    UpdateCamera(deltaTime);
}

void SceneViewPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        RenderToolbar();
        
        // Get available space for scene rendering
        ImVec2 viewportSize = ImGui::GetContentRegionAvail();
        
        if (viewportSize.x > 0 && viewportSize.y > 0) {
            // Update render texture size if needed
            if (m_renderTextureWidth != (int)viewportSize.x || m_renderTextureHeight != (int)viewportSize.y) {
                m_renderTextureWidth = (int)viewportSize.x;
                m_renderTextureHeight = (int)viewportSize.y;
                // TODO: Recreate render texture with new size
            }
            
            RenderSceneContent();
            
            // Display scene texture
            if (m_srv) {
                ImGui::Image((void*)m_srv.Get(), viewportSize);
            } else {
                // Placeholder when no render texture is available
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 pos = ImGui::GetCursorScreenPos();
                
                // Draw a simple grid pattern as placeholder
                drawList->AddRectFilled(pos, ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y), 
                                       IM_COL32(50, 50, 50, 255));
                
                // Draw grid lines
                for (int i = 0; i < 20; ++i) {
                    float x = pos.x + (i * viewportSize.x / 20.0f);
                    float y = pos.y + (i * viewportSize.y / 20.0f);
                    
                    drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + viewportSize.y), 
                                     IM_COL32(70, 70, 70, 255));
                    drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + viewportSize.x, y), 
                                     IM_COL32(70, 70, 70, 255));
                }
                
                // Center text
                ImVec2 textSize = ImGui::CalcTextSize("Scene View");
                ImVec2 textPos = ImVec2(pos.x + (viewportSize.x - textSize.x) * 0.5f, 
                                       pos.y + (viewportSize.y - textSize.y) * 0.5f);
                drawList->AddText(textPos, IM_COL32(150, 150, 150, 255), "Scene View");
                
                // Advance cursor and add dummy item to grow window boundaries
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + viewportSize.y));
                ImGui::Dummy(ImVec2(viewportSize.x, 0)); // Add dummy item to satisfy ImGui requirements
            }
            
            // Handle input in scene view
            if (ImGui::IsItemHovered()) {
                HandleInput();
            }
        }
    }
    EndPanel();
}

void SceneViewPanel::Shutdown() {
    std::cout << "Shutting down Scene View panel\n";
}

bool SceneViewPanel::HandleEvent(const std::string& eventType, void* eventData) {
    return false;
}

void SceneViewPanel::SetDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;
    
    // Create render texture and related resources
    CreateRenderTexture(512, 512);
}

void SceneViewPanel::RenderToolbar() {
    ImVec4 accentBlue(0.176f, 0.549f, 0.941f, 1.0f);
    float btnSize = 24.0f;
    ImVec2 btnDim(btnSize, btnSize);

    // Transform tool buttons with icons
    auto GizmoButton = [&](const char* icon, GizmoMode mode, const char* tooltip) {
        bool active = (m_gizmoMode == mode);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, accentBlue);
        if (ImGui::Button(icon, btnDim)) m_gizmoMode = mode;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
    };

    GizmoButton(ICON_FA_ARROWS_ALT, GizmoMode::Move, "Move (W)");
    GizmoButton(ICON_FA_SYNC_ALT, GizmoMode::Rotate, "Rotate (E)");
    GizmoButton(ICON_FA_EXPAND, GizmoMode::Scale, "Scale (R)");

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Render mode dropdown
    const char* renderModeNames[] = { "Shaded", "Wireframe", "Unlit", "Normals", "Depth" };
    ImGui::SetNextItemWidth(90);
    if (ImGui::BeginCombo("##RenderMode", renderModeNames[(int)m_renderMode])) {
        for (int i = 0; i < 5; i++) {
            bool selected = ((int)m_renderMode == i);
            if (ImGui::Selectable(renderModeNames[i], selected))
                m_renderMode = (RenderMode)i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render Mode");

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Toggles
    ImGui::Checkbox(ICON_FA_GRID " Grid", &m_showGrid);
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_CUBE " Gizmos", &m_showGizmos);

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Camera speed
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("##CamSpeed", &m_cameraSpeed, 0.1f, 0.1f, 50.0f, ICON_FA_CAMERA " %.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera Speed");

    ImGui::Separator();
}

void SceneViewPanel::RenderSceneContent() {
    if (!m_device || !m_context) return;
    
    // Set up render target
    if (m_rtv) {
        ID3D11RenderTargetView* targets[] = { m_rtv.Get() };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        
        // Clear render target
        float clearColor[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
        m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
        
        // Set viewport
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(m_renderTextureWidth);
        viewport.Height = static_cast<float>(m_renderTextureHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);
        
        // TODO: Render actual scene content here
        // For now, just render a simple colored background
        
        // Restore main render target
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }
}

void SceneViewPanel::CreateRenderTexture(int width, int height) {
    if (!m_device) return;
    
    // Release existing resources
    m_renderTarget.Reset();
    m_rtv.Reset();
    m_srv.Reset();
    
    // Create render texture
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = 0;
    textureDesc.MiscFlags = 0;
    
    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, &m_renderTarget);
    if (FAILED(hr)) {
        std::cout << "Failed to create render texture\n";
        return;
    }
    
    // Create render target view
    hr = m_device->CreateRenderTargetView(m_renderTarget.Get(), nullptr, &m_rtv);
    if (FAILED(hr)) {
        std::cout << "Failed to create render target view\n";
        return;
    }
    
    // Create shader resource view
    hr = m_device->CreateShaderResourceView(m_renderTarget.Get(), nullptr, &m_srv);
    if (FAILED(hr)) {
        std::cout << "Failed to create shader resource view\n";
        return;
    }
    
    m_renderTextureWidth = width;
    m_renderTextureHeight = height;
    
    std::cout << "Created render texture: " << width << "x" << height << "\n";
}

void SceneViewPanel::HandleInput() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Camera controls
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        ImVec2 delta = io.MouseDelta;
        m_cameraYaw += delta.x * 0.01f;
        m_cameraPitch += delta.y * 0.01f;
        
        // Clamp pitch
        if (m_cameraPitch > 1.5f) m_cameraPitch = 1.5f;
        if (m_cameraPitch < -1.5f) m_cameraPitch = -1.5f;
    }
    
    // Zoom with mouse wheel
    if (io.MouseWheel != 0.0f) {
        m_cameraDistance -= io.MouseWheel * 0.5f;
        if (m_cameraDistance < 1.0f) m_cameraDistance = 1.0f;
        if (m_cameraDistance > 50.0f) m_cameraDistance = 50.0f;
    }
}

void SceneViewPanel::UpdateCamera(float deltaTime) {
    // TODO: Update camera based on controls
}

} // namespace SparkEditor