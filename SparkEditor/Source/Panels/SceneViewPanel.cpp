/**
 * @file SceneViewPanel.cpp
 * @brief Implementation of the Scene View panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneViewPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    SceneViewPanel::SceneViewPanel() : EditorPanel("Scene View", "scene_view_panel") {}

    bool SceneViewPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Scene View panel\n";
        return true;
    }

    void SceneViewPanel::Update(float deltaTime)
    {
        UpdateCamera(deltaTime);
    }

    void SceneViewPanel::Render()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();

            // Get available space for scene rendering
            ImVec2 viewportSize = ImGui::GetContentRegionAvail();

            if (viewportSize.x > 0 && viewportSize.y > 0)
            {
                // Update render texture size if needed
                if (m_renderTextureWidth != (int)viewportSize.x || m_renderTextureHeight != (int)viewportSize.y)
                {
                    m_renderTextureWidth = (int)viewportSize.x;
                    m_renderTextureHeight = (int)viewportSize.y;
                    // Recreate render texture to match the new ImGui panel size
                    CreateRenderTexture(m_renderTextureWidth, m_renderTextureHeight);
                }

                RenderSceneContent();

                // Display scene texture
                if (m_srv)
                {
                    ImGui::Image((void*)m_srv.Get(), viewportSize);
                }
                else
                {
                    // Placeholder when no render texture is available
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2 pos = ImGui::GetCursorScreenPos();

                    // Draw a simple grid pattern as placeholder
                    drawList->AddRectFilled(pos, ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y),
                                            IM_COL32(50, 50, 50, 255));

                    // Draw grid lines
                    for (int i = 0; i < 20; ++i)
                    {
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

                    // Use InvisibleButton so the area registers as an interactive item
                    // for hover detection and input handling
                    ImGui::SetCursorScreenPos(pos);
                    ImGui::InvisibleButton("##SceneViewArea", viewportSize);
                }

                // Handle input in scene view
                if (ImGui::IsItemHovered())
                {
                    HandleInput();
                }
            }
        }
        EndPanel();
    }

    void SceneViewPanel::Shutdown()
    {
        std::cout << "Shutting down Scene View panel\n";
    }

    bool SceneViewPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void SceneViewPanel::SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
#ifdef _WIN32
        m_device = device;
        m_context = context;

        // Create render texture and related resources
        CreateRenderTexture(512, 512);
#else
        (void)device;
        (void)context;
#endif
    }

    void SceneViewPanel::RenderToolbar()
    {
        ImVec4 accentBlue(0.176f, 0.549f, 0.941f, 1.0f);
        float btnSize = 24.0f;
        ImVec2 btnDim(btnSize, btnSize);

        // Transform tool buttons with icons
        auto GizmoButton = [&](const char* icon, GizmoMode mode, const char* tooltip)
        {
            bool active = (m_gizmoMode == mode);
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, accentBlue);
            if (ImGui::Button(icon, btnDim))
                m_gizmoMode = mode;
            if (active)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine();
        };

        GizmoButton(ICON_FA_ARROWS_ALT, GizmoMode::Move, "Move (W)");
        GizmoButton(ICON_FA_SYNC_ALT, GizmoMode::Rotate, "Rotate (E)");
        GizmoButton(ICON_FA_EXPAND, GizmoMode::Scale, "Scale (R)");

        ImGui::Text("|");
        ImGui::SameLine();

        // Render mode dropdown
        const char* renderModeNames[] = {"Shaded", "Wireframe", "Unlit", "Normals", "Depth"};
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("##RenderMode", renderModeNames[(int)m_renderMode]))
        {
            for (int i = 0; i < 5; i++)
            {
                bool selected = ((int)m_renderMode == i);
                if (ImGui::Selectable(renderModeNames[i], selected))
                    m_renderMode = (RenderMode)i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Render Mode");

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        // Toggles
        ImGui::Checkbox(ICON_FA_GRID " Grid", &m_showGrid);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_CUBE " Gizmos", &m_showGizmos);

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        // Camera speed
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##CamSpeed", &m_cameraSpeed, 0.1f, 0.1f, 50.0f, ICON_FA_CAMERA " %.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Camera Speed");

        ImGui::Separator();
    }

    void SceneViewPanel::RenderSceneContent()
    {
#ifdef _WIN32
        if (!m_device || !m_context)
            return;

        // Set up render target
        if (m_rtv)
        {
            ID3D11RenderTargetView* targets[] = {m_rtv.Get()};
            m_context->OMSetRenderTargets(1, targets, nullptr);

            // Clear render target
            float clearColor[4] = {0.2f, 0.2f, 0.2f, 1.0f};
            m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);

            // Set viewport
            D3D11_VIEWPORT viewport = {};
            viewport.Width = static_cast<float>(m_renderTextureWidth);
            viewport.Height = static_cast<float>(m_renderTextureHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &viewport);

            // Render a skybox-style gradient background as scene placeholder
            {
                D3D11_VIEWPORT upperVP = viewport;
                upperVP.Height = viewport.Height * 0.5f;
                m_context->RSSetViewports(1, &upperVP);
                float skyColor[4] = {0.4f, 0.6f, 0.9f, 1.0f};
                m_context->ClearRenderTargetView(m_rtv.Get(), skyColor);

                m_context->RSSetViewports(1, &viewport);
            }

            // Restore main render target
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }
#endif
    }

    void SceneViewPanel::CreateRenderTexture(int width, int height)
    {
#ifdef _WIN32
        if (!m_device)
            return;

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
        if (FAILED(hr))
        {
            std::cout << "Failed to create render texture\n";
            return;
        }

        // Create render target view
        hr = m_device->CreateRenderTargetView(m_renderTarget.Get(), nullptr, &m_rtv);
        if (FAILED(hr))
        {
            std::cout << "Failed to create render target view\n";
            return;
        }

        // Create shader resource view
        hr = m_device->CreateShaderResourceView(m_renderTarget.Get(), nullptr, &m_srv);
        if (FAILED(hr))
        {
            std::cout << "Failed to create shader resource view\n";
            return;
        }

        m_renderTextureWidth = width;
        m_renderTextureHeight = height;

        std::cout << "Created render texture: " << width << "x" << height << "\n";
#else
        (void)width;
        (void)height;
#endif
    }

    void SceneViewPanel::HandleInput()
    {
        ImGuiIO& io = ImGui::GetIO();

        // Camera controls
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            ImVec2 delta = io.MouseDelta;
            m_cameraYaw += delta.x * 0.01f;
            m_cameraPitch += delta.y * 0.01f;

            // Clamp pitch
            if (m_cameraPitch > 1.5f)
                m_cameraPitch = 1.5f;
            if (m_cameraPitch < -1.5f)
                m_cameraPitch = -1.5f;
        }

        // Zoom with mouse wheel
        if (io.MouseWheel != 0.0f)
        {
            m_cameraDistance -= io.MouseWheel * 0.5f;
            if (m_cameraDistance < 1.0f)
                m_cameraDistance = 1.0f;
            if (m_cameraDistance > 50.0f)
                m_cameraDistance = 50.0f;
        }
    }

    void SceneViewPanel::UpdateCamera(float deltaTime)
    {
        // Only process WASD movement when the right mouse button is held inside the scene view
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            return;
        }

        ImGuiIO& io = ImGui::GetIO();

        // Compute forward and right vectors from yaw/pitch
        float cosP = cosf(m_cameraPitch);
        float forwardX = cosP * sinf(m_cameraYaw);
        float forwardY = -sinf(m_cameraPitch);
        float forwardZ = cosP * cosf(m_cameraYaw);

        float rightX = cosf(m_cameraYaw);
        float rightY = 0.0f;
        float rightZ = -sinf(m_cameraYaw);

        float moveSpeed = m_cameraSpeed * deltaTime;

        // Shift to boost speed
        if (io.KeyShift)
        {
            moveSpeed *= 2.0f;
        }

        float dx = 0.0f, dy = 0.0f, dz = 0.0f;

        // W / S -- forward / backward
        if (ImGui::IsKeyDown(ImGuiKey_W))
        {
            dx += forwardX * moveSpeed;
            dy += forwardY * moveSpeed;
            dz += forwardZ * moveSpeed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S))
        {
            dx -= forwardX * moveSpeed;
            dy -= forwardY * moveSpeed;
            dz -= forwardZ * moveSpeed;
        }

        // A / D -- strafe left / right
        if (ImGui::IsKeyDown(ImGuiKey_A))
        {
            dx -= rightX * moveSpeed;
            dy -= rightY * moveSpeed;
            dz -= rightZ * moveSpeed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D))
        {
            dx += rightX * moveSpeed;
            dy += rightY * moveSpeed;
            dz += rightZ * moveSpeed;
        }

        // Q / E -- down / up
        if (ImGui::IsKeyDown(ImGuiKey_Q))
        {
            dy -= moveSpeed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_E))
        {
            dy += moveSpeed;
        }

        // Apply the accumulated delta to the camera distance (orbit-style proxy).
        // A full camera position vector would be stored separately in a production
        // implementation; here we adjust the orbit distance as an approximation.
        m_cameraDistance -= (dx + dy + dz) * 0.1f;
        if (m_cameraDistance < 1.0f)
            m_cameraDistance = 1.0f;
        if (m_cameraDistance > 50.0f)
            m_cameraDistance = 50.0f;
    }

} // namespace SparkEditor