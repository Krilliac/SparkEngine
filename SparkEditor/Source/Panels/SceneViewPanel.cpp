/**
 * @file SceneViewPanel.cpp
 * @brief Implementation of the Scene View panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneViewPanel.h"
#include "SelectionManager.h"
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

        // Subscribe to the editor-wide SelectionManager so the scene view
        // tracks which entity has the selection highlight / gizmo.
        m_selectionMgrCallbackId = SelectionManager::GetInstance().OnSelectionChanged(
            [this](const SelectionChangedEvent& event)
            {
                if (event.current.empty())
                    m_selectedEntityId = 0;
                else
                    m_selectedEntityId = event.current.back();
            });

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
                    ImGui::Image(static_cast<void*>(m_srv.Get()), viewportSize);
                }
                else
                {
                    // Fallback scene view when no render texture is available
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImVec2 maxPos(pos.x + viewportSize.x, pos.y + viewportSize.y);

                    // Dark background
                    drawList->AddRectFilled(pos, maxPos, IM_COL32(40, 42, 48, 255));

                    // Grid configuration
                    constexpr float gridSpacingSmall = 32.0f;
                    constexpr float gridSpacingLarge = gridSpacingSmall * 4.0f;
                    float cx = pos.x + viewportSize.x * 0.5f;
                    float cy = pos.y + viewportSize.y * 0.5f;

                    // Minor grid lines
                    for (float x = fmodf(cx, gridSpacingSmall); x < viewportSize.x; x += gridSpacingSmall)
                    {
                        drawList->AddLine(ImVec2(pos.x + x, pos.y), ImVec2(pos.x + x, maxPos.y),
                                          IM_COL32(55, 58, 65, 255));
                    }
                    for (float y = fmodf(cy, gridSpacingSmall); y < viewportSize.y; y += gridSpacingSmall)
                    {
                        drawList->AddLine(ImVec2(pos.x, pos.y + y), ImVec2(maxPos.x, pos.y + y),
                                          IM_COL32(55, 58, 65, 255));
                    }

                    // Major grid lines
                    for (float x = fmodf(cx, gridSpacingLarge); x < viewportSize.x; x += gridSpacingLarge)
                    {
                        drawList->AddLine(ImVec2(pos.x + x, pos.y), ImVec2(pos.x + x, maxPos.y),
                                          IM_COL32(70, 75, 85, 255));
                    }
                    for (float y = fmodf(cy, gridSpacingLarge); y < viewportSize.y; y += gridSpacingLarge)
                    {
                        drawList->AddLine(ImVec2(pos.x, pos.y + y), ImVec2(maxPos.x, pos.y + y),
                                          IM_COL32(70, 75, 85, 255));
                    }

                    // Origin axes
                    drawList->AddLine(ImVec2(cx, pos.y), ImVec2(cx, maxPos.y), IM_COL32(80, 180, 80, 120), 1.5f);
                    drawList->AddLine(ImVec2(pos.x, cy), ImVec2(maxPos.x, cy), IM_COL32(180, 80, 80, 120), 1.5f);

                    // Origin crosshair
                    drawList->AddCircle(ImVec2(cx, cy), 6.0f, IM_COL32(200, 200, 200, 100), 12);

                    // "No render target" label
                    const char* label = "Scene View — No Render Target";
                    ImVec2 textSize = ImGui::CalcTextSize(label);
                    ImVec2 textPos(pos.x + (viewportSize.x - textSize.x) * 0.5f,
                                   pos.y + (viewportSize.y - textSize.y) * 0.5f);
                    drawList->AddRectFilled(ImVec2(textPos.x - 8, textPos.y - 4),
                                            ImVec2(textPos.x + textSize.x + 8, textPos.y + textSize.y + 4),
                                            IM_COL32(30, 30, 30, 180), 4.0f);
                    drawList->AddText(textPos, IM_COL32(180, 180, 180, 255), label);

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

                // Render collaborative peer overlays on top of scene
                RenderPeerOverlays();
            }
        }
        EndPanel();
    }

    void SceneViewPanel::RenderPeerOverlays()
    {
        if (!m_collabSession || !m_collabSession->IsConnected())
            return;

        auto peers = m_collabSession->GetConnectedPeers();
        if (peers.size() <= 1)
            return;

        PeerID localId = m_collabSession->GetLocalPeerID();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 windowPos = ImGui::GetWindowPos();
        float yOffset = 40.0f; // Below toolbar

        for (const auto& peer : peers)
        {
            if (peer.id == localId)
                continue;

            ImU32 color = IM_COL32(static_cast<int>(peer.color.r * 255), static_cast<int>(peer.color.g * 255),
                                   static_cast<int>(peer.color.b * 255), 200);

            // Draw peer name tag with selection info in top-right corner
            std::string label = peer.userName;
            if (!peer.selectedNode.empty())
            {
                label += " [" + peer.selectedNode + "]";
            }

            ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            float padding = 4.0f;
            float tagX = windowPos.x + ImGui::GetWindowWidth() - textSize.x - padding * 3.0f;
            float tagY = windowPos.y + yOffset;

            // Background pill
            drawList->AddRectFilled(ImVec2(tagX - padding, tagY - padding),
                                    ImVec2(tagX + textSize.x + padding, tagY + textSize.y + padding),
                                    IM_COL32(0, 0, 0, 150), 4.0f);

            // Colored left border
            drawList->AddRectFilled(ImVec2(tagX - padding, tagY - padding),
                                    ImVec2(tagX - padding + 3.0f, tagY + textSize.y + padding), color, 2.0f);

            // Text
            drawList->AddText(ImVec2(tagX, tagY), color, label.c_str());

            yOffset += textSize.y + padding * 3.0f;
        }
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
            // RenderSceneContent runs inside ImGui's frame build on the editor's
            // shared immediate context. Save the currently-bound render target so
            // we can restore it — otherwise ImGui's RenderDrawData later in the
            // frame draws into our texture (or no target at all) and the whole
            // editor window renders as only the clear color (a blue screen).
            ComPtr<ID3D11RenderTargetView> prevRTV;
            ComPtr<ID3D11DepthStencilView> prevDSV;
            m_context->OMGetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.GetAddressOf());
            UINT prevViewportCount = 1;
            D3D11_VIEWPORT prevViewport{};
            m_context->RSGetViewports(&prevViewportCount, prevViewportCount ? &prevViewport : nullptr);

            ID3D11RenderTargetView* targets[] = {m_rtv.Get()};
            m_context->OMSetRenderTargets(1, targets, m_dsv.Get());

            // Clear render target + depth
            const float darkClear[4] = {0.12f, 0.13f, 0.15f, 1.0f};
            m_context->ClearRenderTargetView(m_rtv.Get(), darkClear);
            if (m_dsv)
                m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

            // Set viewport
            D3D11_VIEWPORT viewport = {};
            viewport.Width = static_cast<float>(m_renderTextureWidth);
            viewport.Height = static_cast<float>(m_renderTextureHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &viewport);

            // Render the ECS World via the shared basic-shader path, if the
            // editor has an attached GraphicsEngine. Falls back to a plain
            // dark-clear viewport (above) when the graphics device hasn't
            // been wired up yet.
            if (m_graphics && m_renderTextureHeight > 0)
            {
                EnsureDemoWorld();

                using namespace DirectX;
                const XMVECTOR eye = XMVectorSet(0.0f, 3.0f, -6.0f, 1.0f);
                const XMVECTOR at = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
                const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                const XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
                const float aspect =
                    static_cast<float>(m_renderTextureWidth) / static_cast<float>(m_renderTextureHeight);
                const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 6000.0f);

                Spark::RenderWorldBasic(m_demoWorld, *m_graphics, m_meshCache, view, proj);
            }

            // Restore the editor's render target so ImGui renders into the window.
            m_context->OMSetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.Get());
            if (prevViewportCount)
                m_context->RSSetViewports(1, &prevViewport);
        }
#endif
    }

#ifdef _WIN32
    void SceneViewPanel::EnsureDemoWorld()
    {
        if (m_demoWorldPopulated)
            return;
        m_demoWorldPopulated = true;

        EntityID e = m_demoWorld.CreateEntity("DemoSoldier");
        m_demoWorld.AddComponent<Transform>(e); // identity at origin
        MeshRenderer& mr = m_demoWorld.AddComponent<MeshRenderer>(e);
        // Non-empty path required — WorldMeshCache::GetOrLoad early-returns on
        // an empty path. If this asset isn't found under the editor's working
        // directory, LoadOrPlaceholderMesh falls back to a procedural unit
        // cube, which is still valid geometry proving the render path works.
        mr.meshPath = "Assets/Models/MMOFPS/characters/soldier.obj";
    }
#endif

    void SceneViewPanel::CreateRenderTexture(int width, int height)
    {
#ifdef _WIN32
        if (!m_device)
            return;

        // Release existing resources
        m_renderTarget.Reset();
        m_rtv.Reset();
        m_srv.Reset();
        m_depthTexture.Reset();
        m_dsv.Reset();

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
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "SceneViewPanel: CreateTexture2D failed (HR=0x%08X)",
                            static_cast<unsigned>(hr));
            return;
        }

        // Create render target view
        hr = m_device->CreateRenderTargetView(m_renderTarget.Get(), nullptr, &m_rtv);
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "SceneViewPanel: CreateRenderTargetView failed (HR=0x%08X)",
                            static_cast<unsigned>(hr));
            return;
        }

        // Create shader resource view
        hr = m_device->CreateShaderResourceView(m_renderTarget.Get(), nullptr, &m_srv);
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "SceneViewPanel: CreateShaderResourceView failed (HR=0x%08X)",
                            static_cast<unsigned>(hr));
            return;
        }

        // Depth buffer sized to match the color RT — needed for
        // Spark::RenderWorldBasic() to depth-test the world geometry.
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.SampleDesc.Quality = 0;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        depthDesc.CPUAccessFlags = 0;
        depthDesc.MiscFlags = 0;

        hr = m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthTexture);
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "SceneViewPanel: CreateTexture2D (depth) failed (HR=0x%08X)",
                            static_cast<unsigned>(hr));
            return;
        }

        hr = m_device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, &m_dsv);
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "SceneViewPanel: CreateDepthStencilView failed (HR=0x%08X)",
                            static_cast<unsigned>(hr));
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