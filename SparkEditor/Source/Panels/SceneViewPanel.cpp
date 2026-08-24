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
#include "../Core/EditorUI.h"
#include "../CommandHistory.h"
#include "../Gizmos/SceneEditTools.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
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

        // The first project/world swap is initiated from a modal.  Without an
        // explicit one-shot focus request ImGui may display the dockspace
        // background while neither Scene View nor Game View is the selected
        // tab.  That surface is easily mistaken for a failed black render
        // target (and no grid/diagnostic overlays appear because BeginPanel()
        // returns false).  Selecting the tab is exactly what a manual click
        // did in the live reproduction.  m_hasRenderedActiveFrame prevents
        // subsequent scene/play-mode swaps from stealing focus.
        if (m_requestFocusOnNextRender)
            ImGui::SetNextWindowFocus();

        if (BeginPanel())
        {
            m_requestFocusOnNextRender = false;
            m_hasRenderedActiveFrame = true;
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

                    const ImVec2 gridMin = ImGui::GetItemRectMin();
                    const ImVec2 gridMax = ImGui::GetItemRectMax();
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    if (m_showGrid)
                    {
                        constexpr float spacing = 40.0f;
                        for (float x = gridMin.x; x < gridMax.x; x += spacing)
                            drawList->AddLine({x, gridMin.y}, {x, gridMax.y}, IM_COL32(130, 140, 155, 24));
                        for (float y = gridMin.y; y < gridMax.y; y += spacing)
                            drawList->AddLine({gridMin.x, y}, {gridMax.x, y}, IM_COL32(130, 140, 155, 24));
                    }

#ifdef _WIN32
                    if (!m_graphics || !m_world || m_lastRenderStats.drawn == 0)
                    {
                        std::string diagnostic;
                        if (!m_graphics)
                            diagnostic = "Viewport renderer unavailable";
                        else if (!m_world)
                            diagnostic = "No scene World attached";
                        else if (m_lastRenderStats.candidates == 0)
                            diagnostic = "No visible MeshRenderer entities";
                        else
                            diagnostic = "Mesh candidates found, but no geometry was drawable";
                        const ImVec2 textSize = ImGui::CalcTextSize(diagnostic.c_str());
                        const ImVec2 textPos{gridMin.x + (viewportSize.x - textSize.x) * 0.5f, gridMin.y + 18.0f};
                        drawList->AddRectFilled({textPos.x - 8, textPos.y - 4},
                                                {textPos.x + textSize.x + 8, textPos.y + textSize.y + 4},
                                                IM_COL32(15, 18, 24, 210), 4.0f);
                        drawList->AddText(textPos, IM_COL32(245, 190, 80, 255), diagnostic.c_str());
                    }
#endif
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

                // The last submitted item is the scene image (or the fallback
                // InvisibleButton, which covers the same rect) — capture its
                // rect and hover state for input routing and the gizmo overlay.
                const ImVec2 imagePos = ImGui::GetItemRectMin();
                const bool viewportHovered = ImGui::IsItemHovered();

                // Handle input in scene view
                if (viewportHovered)
                {
                    HandleInput();
                    HandleEditShortcuts();
                }

#ifdef _WIN32
                // W9: translate gizmo for the selected World entity, drawn as
                // an ImGui overlay on top of the rendered scene. Called even
                // when the viewport isn't hovered so an in-flight drag keeps
                // tracking the mouse outside the panel.
                if (m_showGizmos)
                {
                    RenderTranslateGizmo(imagePos.x, imagePos.y, viewportSize.x, viewportSize.y, viewportHovered);
                }
#else
                (void)imagePos;
#endif

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

        // Grid snap toggle + size — applied to gizmo translation drags (W9).
        {
            const bool snapActive = m_snapEnabled;
            if (snapActive)
                ImGui::PushStyleColor(ImGuiCol_Button, accentBlue);
            if (ImGui::Button(ICON_FA_MAGNET, btnDim))
                m_snapEnabled = !m_snapEnabled;
            if (snapActive)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Grid Snap (gizmo translation)");
            ImGui::SameLine();

            constexpr float kSnapSizes[] = {0.25f, 0.5f, 1.0f, 2.0f};
            const char* kSnapLabels[] = {"0.25", "0.5", "1", "2"};
            int snapIndex = 2;
            for (int i = 0; i < 4; i++)
            {
                if (m_snapSize == kSnapSizes[i])
                    snapIndex = i;
            }
            ImGui::SetNextItemWidth(52);
            if (ImGui::BeginCombo("##SnapSize", kSnapLabels[snapIndex]))
            {
                for (int i = 0; i < 4; i++)
                {
                    const bool selected = (i == snapIndex);
                    if (ImGui::Selectable(kSnapLabels[i], selected))
                        m_snapSize = kSnapSizes[i];
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Snap Size (m)");
        }

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        // Scene-edit actions on the current World selection (W9).
        {
            const bool hasSelection = GetSelectedWorldEntity() != entt::null;
            if (!hasSelection)
                ImGui::BeginDisabled();
            if (ImGui::Button(ICON_FA_COPY, btnDim))
                DuplicateSelectedEntity();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Duplicate (Ctrl+D)");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_DOWN, btnDim))
                AlignSelectedEntityToGround();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Align to Ground (End)");
            if (!hasSelection)
                ImGui::EndDisabled();
        }

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
            if (m_graphics && m_world && m_renderTextureHeight > 0)
            {
                using namespace DirectX;

                // Camera matrices shared with the gizmo overlay so the
                // overlay projects with exactly the matrices the scene
                // renders with (see ComputeCameraMatrices).
                XMMATRIX view;
                XMMATRIX proj;
                ComputeCameraMatrices(view, proj);

                m_lastRenderStats = Spark::RenderWorldBasic(*m_world, *m_graphics, m_meshCache, view, proj);
            }

            // Restore the editor's render target so ImGui renders into the window.
            m_context->OMSetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.Get());
            if (prevViewportCount)
                m_context->RSSetViewports(1, &prevViewport);
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

        // Pan the orbit focus point along the already-computed forward/right/up
        // vectors, so WASD/QE actually fly the camera through the scene instead
        // of collapsing into a single-axis zoom on m_cameraDistance.
        m_cameraTarget.x += dx;
        m_cameraTarget.y += dy;
        m_cameraTarget.z += dz;
    }

    // ========================================================================
    // W9 scene-edit tools
    // ========================================================================

    ::EntityID SceneViewPanel::GetSelectedWorldEntity() const
    {
        if (!m_world || !m_selectionSink)
        {
            return entt::null;
        }
        const ::EntityID selected = m_selectionSink->GetSelectedEntity();
        if (selected == entt::null || !m_world->GetRegistry().valid(selected))
        {
            return entt::null;
        }
        return selected;
    }

    void SceneViewPanel::HandleEditShortcuts()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
        {
            return;
        }

        // F — frame the selected entity in the viewport camera. Plain F only:
        // Ctrl+F belongs to the editor-wide search shortcut.
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            FocusSelectedEntity();
        }

        // Ctrl+D — duplicate the selection.
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        {
            DuplicateSelectedEntity();
        }

        // End — align the selection to the surface below (Unreal convention).
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))
        {
            AlignSelectedEntityToGround();
        }
    }

    void SceneViewPanel::FocusSelectedEntity()
    {
        const ::EntityID selected = GetSelectedWorldEntity();
        if (selected == entt::null)
        {
            return;
        }
        entt::registry& registry = m_world->GetRegistry();
        const ::Transform* transform = registry.try_get<::Transform>(selected);
        if (!transform)
        {
            return;
        }

        using namespace DirectX;
        const XMVECTOR worldPos = XMVector3TransformCoord(XMVectorZero(), transform->GetWorldMatrix(registry));
        XMStoreFloat3(&m_cameraTarget, worldPos);

        // Frame the unit-cube proxy: ~3x the largest scale axis, clamped to
        // the zoom range HandleInput() enforces. Camera state is not a
        // document mutation, so focusing is intentionally NOT routed through
        // CommandHistory (it would dirty the scene and pollute the undo
        // stack).
        const float maxScale =
            std::max({std::abs(transform->scale.x), std::abs(transform->scale.y), std::abs(transform->scale.z)});
        m_cameraDistance = std::clamp(maxScale * 3.0f, 2.0f, 50.0f);
    }

    void SceneViewPanel::DuplicateSelectedEntity()
    {
        const ::EntityID selected = GetSelectedWorldEntity();
        if (selected == entt::null)
        {
            return;
        }
        const ::EntityID copy = SceneEditTools::DuplicateEntity(*m_world, selected);
        if (copy != entt::null && m_selectionSink)
        {
            m_selectionSink->SetSelectedEntity(copy);
        }
    }

    void SceneViewPanel::AlignSelectedEntityToGround()
    {
        const ::EntityID selected = GetSelectedWorldEntity();
        if (selected == entt::null)
        {
            return;
        }
        SceneEditTools::AlignEntityToGround(*m_world, selected);
    }

#ifdef _WIN32
    void SceneViewPanel::ComputeCameraMatrices(DirectX::XMMATRIX& view, DirectX::XMMATRIX& proj) const
    {
        using namespace DirectX;

        // Spherical orbit camera: derive the eye from the same yaw/pitch
        // forward-vector formula UpdateCamera() uses, so right-drag orbit
        // (HandleInput), wheel zoom (HandleInput), and WASD/QE pan
        // (UpdateCamera) all visibly move this view.
        const float cosP = cosf(m_cameraPitch);
        const float forwardX = cosP * sinf(m_cameraYaw);
        const float forwardY = -sinf(m_cameraPitch);
        const float forwardZ = cosP * cosf(m_cameraYaw);

        const XMVECTOR at = XMVectorSet(m_cameraTarget.x, m_cameraTarget.y, m_cameraTarget.z, 1.0f);
        const XMVECTOR eye =
            XMVectorSet(m_cameraTarget.x - forwardX * m_cameraDistance, m_cameraTarget.y - forwardY * m_cameraDistance,
                        m_cameraTarget.z - forwardZ * m_cameraDistance, 1.0f);
        const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        view = XMMatrixLookAtLH(eye, at, up);

        const float aspect = (m_renderTextureHeight > 0)
                                 ? static_cast<float>(m_renderTextureWidth) / static_cast<float>(m_renderTextureHeight)
                                 : 1.0f;
        proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 6000.0f);
    }

    namespace
    {
        float PointToSegmentDistance(const ImVec2& point, const ImVec2& a, const ImVec2& b)
        {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float lenSq = dx * dx + dy * dy;
            if (lenSq < 1e-6f)
            {
                const float ex = point.x - a.x;
                const float ey = point.y - a.y;
                return std::sqrt(ex * ex + ey * ey);
            }
            const float t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / lenSq, 0.0f, 1.0f);
            const float ex = point.x - (a.x + t * dx);
            const float ey = point.y - (a.y + t * dy);
            return std::sqrt(ex * ex + ey * ey);
        }
    } // namespace

    void SceneViewPanel::RenderTranslateGizmo(float imgX, float imgY, float imgW, float imgH, bool viewportHovered)
    {
        using namespace DirectX;

        // Translation-only for W9 — Rotate/Scale toolbar modes keep their
        // existing (no-op) behavior in the viewport.
        if (m_gizmoMode != GizmoMode::Move || imgW <= 0.0f || imgH <= 0.0f)
        {
            m_gizmoDragging = false;
            m_gizmoDragAxis = -1;
            return;
        }

        const ::EntityID selected = GetSelectedWorldEntity();
        if (m_gizmoDragging && selected != m_gizmoDragEntity)
        {
            // Selection changed mid-drag (e.g. hierarchy click) — abandon the
            // drag without committing a command.
            m_gizmoDragging = false;
            m_gizmoDragAxis = -1;
        }
        if (selected == entt::null)
        {
            return;
        }

        entt::registry& registry = m_world->GetRegistry();
        ::Transform* transform = registry.try_get<::Transform>(selected);
        if (!transform)
        {
            return;
        }

        XMMATRIX view;
        XMMATRIX proj;
        ComputeCameraMatrices(view, proj);

        const XMVECTOR worldPosV = XMVector3TransformCoord(XMVectorZero(), transform->GetWorldMatrix(registry));
        XMFLOAT3 worldPos;
        XMStoreFloat3(&worldPos, worldPosV);

        auto project = [&](const XMFLOAT3& p, ImVec2& out) -> bool
        {
            const XMVECTOR s =
                XMVector3Project(XMLoadFloat3(&p), imgX, imgY, imgW, imgH, 0.0f, 1.0f, proj, view, XMMatrixIdentity());
            XMFLOAT3 sp;
            XMStoreFloat3(&sp, s);
            out = ImVec2(sp.x, sp.y);
            return sp.z > 0.0f && sp.z < 1.0f;
        };

        ImVec2 center;
        if (!project(worldPos, center))
        {
            return; // behind the camera — nothing sensible to draw
        }

        constexpr float kAxisPixels = 70.0f; // on-screen axis length
        constexpr float kArrowPixels = 9.0f; // arrowhead size
        constexpr float kHitPixels = 8.0f;   // hover pick threshold
        const XMFLOAT3 kAxisDirs[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        const ImU32 kAxisColors[3] = {IM_COL32(230, 80, 80, 255), IM_COL32(80, 220, 80, 255),
                                      IM_COL32(90, 110, 255, 255)};

        ImVec2 ends[3];
        ImVec2 screenDirs[3];
        float worldPerPixel[3] = {0.0f, 0.0f, 0.0f};
        int hover = -1;
        float bestDist = kHitPixels;
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        for (int i = 0; i < 3; i++)
        {
            const XMFLOAT3 tip = {worldPos.x + kAxisDirs[i].x, worldPos.y + kAxisDirs[i].y,
                                  worldPos.z + kAxisDirs[i].z};
            ImVec2 tipScreen;
            project(tip, tipScreen);
            const float dx = tipScreen.x - center.x;
            const float dy = tipScreen.y - center.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-3f)
            {
                // Axis points straight at the camera — not draggable this frame.
                ends[i] = center;
                screenDirs[i] = ImVec2(0.0f, 0.0f);
                continue;
            }
            screenDirs[i] = ImVec2(dx / len, dy / len);
            worldPerPixel[i] = 1.0f / len; // the 1m tip offset spans 'len' pixels
            ends[i] = ImVec2(center.x + screenDirs[i].x * kAxisPixels, center.y + screenDirs[i].y * kAxisPixels);

            const float dist = PointToSegmentDistance(mouse, center, ends[i]);
            if (dist < bestDist)
            {
                bestDist = dist;
                hover = i;
            }
        }

        if (!m_gizmoDragging)
        {
            m_gizmoHoverAxis = viewportHovered ? hover : -1;
        }

        // Draw the axes into the window draw list (clipped to the panel).
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (int i = 0; i < 3; i++)
        {
            if (screenDirs[i].x == 0.0f && screenDirs[i].y == 0.0f)
            {
                continue;
            }
            const bool active = m_gizmoDragging && m_gizmoDragAxis == i;
            const bool hovered = !m_gizmoDragging && m_gizmoHoverAxis == i;
            const ImU32 color =
                active ? IM_COL32(255, 230, 60, 255) : (hovered ? IM_COL32(255, 255, 255, 255) : kAxisColors[i]);
            drawList->AddLine(center, ends[i], color, 2.5f);
            const float nx = screenDirs[i].x;
            const float ny = screenDirs[i].y;
            const ImVec2 arrowA(ends[i].x - nx * kArrowPixels + ny * kArrowPixels * 0.5f,
                                ends[i].y - ny * kArrowPixels - nx * kArrowPixels * 0.5f);
            const ImVec2 arrowB(ends[i].x - nx * kArrowPixels - ny * kArrowPixels * 0.5f,
                                ends[i].y - ny * kArrowPixels + nx * kArrowPixels * 0.5f);
            drawList->AddTriangleFilled(ends[i], arrowA, arrowB, color);
        }
        drawList->AddCircleFilled(center, 3.5f, IM_COL32(255, 255, 255, 200));

        // Begin drag on left-press over a hovered axis.
        if (!m_gizmoDragging && viewportHovered && m_gizmoHoverAxis >= 0 &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_gizmoDragging = true;
            m_gizmoDragAxis = m_gizmoHoverAxis;
            m_gizmoDragEntity = selected;
            m_dragStartLocalPos = transform->position;
            m_dragStartWorldPos = worldPos;
            m_dragStartMouseX = mouse.x;
            m_dragStartMouseY = mouse.y;
        }

        if (!m_gizmoDragging)
        {
            return;
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const int axis = m_gizmoDragAxis;
            if (axis >= 0 && axis < 3 && worldPerPixel[axis] > 0.0f)
            {
                // Total mouse delta since drag start, projected onto the
                // axis' current screen direction → world units along the
                // (world-aligned) axis.
                const float mouseDx = mouse.x - m_dragStartMouseX;
                const float mouseDy = mouse.y - m_dragStartMouseY;
                const float alongPixels = mouseDx * screenDirs[axis].x + mouseDy * screenDirs[axis].y;
                const float worldDelta = alongPixels * worldPerPixel[axis];

                XMFLOAT3 targetWorld = m_dragStartWorldPos;
                float* coord = (&targetWorld.x) + axis;
                *coord += worldDelta;

                // Grid snap: quantise the ABSOLUTE coordinate along the
                // dragged axis so entities land on the grid rather than at
                // grid-sized offsets from where they started.
                if (m_snapEnabled && m_snapSize > 0.0f)
                {
                    *coord = std::round(*coord / m_snapSize) * m_snapSize;
                }

                const XMFLOAT3 worldDeltaVec = {targetWorld.x - m_dragStartWorldPos.x,
                                                targetWorld.y - m_dragStartWorldPos.y,
                                                targetWorld.z - m_dragStartWorldPos.z};
                const XMFLOAT3 localDelta =
                    SceneEditTools::WorldDeltaToParentLocal(registry, *transform, worldDeltaVec);

                // Live preview mutation; the single undo step is committed on
                // release (same snapshot-at-drag-begin pattern GizmoSystem
                // uses for the legacy SceneFile path).
                transform->position = {m_dragStartLocalPos.x + localDelta.x, m_dragStartLocalPos.y + localDelta.y,
                                       m_dragStartLocalPos.z + localDelta.z};
            }
        }
        else
        {
            // Release — commit the whole drag as one undoable command,
            // capturing the entity id (never a component pointer).
            const XMFLOAT3 endPos = transform->position;
            const bool moved = endPos.x != m_dragStartLocalPos.x || endPos.y != m_dragStartLocalPos.y ||
                               endPos.z != m_dragStartLocalPos.z;
            if (moved)
            {
                ::World* worldPtr = m_world; // safe: SwapWorld clears the history before freeing the World
                const ::EntityID id = m_gizmoDragEntity;
                const XMFLOAT3 oldPos = m_dragStartLocalPos;
                Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                    [worldPtr, id, endPos]()
                    {
                        entt::registry& reg = worldPtr->GetRegistry();
                        if (!reg.valid(id))
                        {
                            return;
                        }
                        if (::Transform* t = reg.try_get<::Transform>(id))
                        {
                            t->position = endPos;
                        }
                    },
                    [worldPtr, id, oldPos]()
                    {
                        entt::registry& reg = worldPtr->GetRegistry();
                        if (!reg.valid(id))
                        {
                            return;
                        }
                        if (::Transform* t = reg.try_get<::Transform>(id))
                        {
                            t->position = oldPos;
                        }
                    },
                    "Move Entity"));
            }
            m_gizmoDragging = false;
            m_gizmoDragAxis = -1;
            m_gizmoDragEntity = entt::null;
        }
    }
#endif

} // namespace SparkEditor
