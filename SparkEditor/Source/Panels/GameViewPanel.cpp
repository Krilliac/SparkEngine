/**
 * @file GameViewPanel.cpp
 * @brief Game viewport panel with full FPS HUD — shows player camera perspective
 * @author Spark Engine Team
 * @date 2025
 */

#include "GameViewPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/LogMacros.h"
#include "Engine/ECS/Components.h"
#include <imgui.h>
#include <iostream>
#include <cmath>
#include <cstdio>

namespace SparkEditor
{

    // Helper: clamp
    static float Clamp01(float v)
    {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    GameViewPanel::GameViewPanel() : EditorPanel("Game View", "game_view_panel") {}

    bool GameViewPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Game View panel with FPS HUD");

        // Pre-populate scoreboard for preview
        m_scoreboardEntries = {{"Player1", 15, 8, 2200, 32, true},      {"FragMaster", 18, 5, 2650, 45, false},
                               {"SniperElite", 12, 9, 1800, 28, false}, {"RocketJock", 10, 12, 1500, 55, false},
                               {"NovaCombat", 8, 7, 1200, 40, false},   {"GhostRecon", 14, 6, 2100, 38, false}};

        return true;
    }

    void GameViewPanel::Update(float deltaTime)
    {
        m_lastDeltaTime = deltaTime;
        m_totalTime += deltaTime;

        // Simulate health regeneration (slow)
        if (m_health < m_maxHealth && m_health > 30.0f)
        {
            m_health = std::min(m_maxHealth, m_health + 1.0f * deltaTime);
        }

        // Simulate stamina recovery
        m_stamina = std::min(100.0f, m_stamina + 5.0f * deltaTime);

        // Simulate ammo usage periodically
        if (m_ammo > 0 && fmod(m_totalTime, 4.0f) < deltaTime)
        {
            m_ammo = std::max(0, m_ammo - 1);
        }
        // Auto reload when empty
        if (m_ammo <= 0 && !m_isReloading)
        {
            m_isReloading = true;
            m_reloadProgress = 0.0f;
        }
        if (m_isReloading)
        {
            m_reloadProgress += deltaTime / 2.0f; // 2 sec reload
            if (m_reloadProgress >= 1.0f)
            {
                m_isReloading = false;
                m_reloadProgress = 0.0f;
                m_ammo = m_maxAmmo;
            }
        }

        // Spawn kill feed entries periodically
        m_killFeedSpawnTimer += deltaTime;
        if (m_killFeedSpawnTimer > 6.0f)
        {
            m_killFeedSpawnTimer = 0.0f;
            static const char* names[] = {"Player1",    "FragMaster", "SniperElite",
                                          "RocketJock", "NovaCombat", "GhostRecon"};
            static const char* weapons[] = {"Rifle", "Shotgun", "Sniper", "Rocket", "Pistol"};
            int ki = ((int)(m_totalTime * 3.7f)) % 6;
            int vi = ((int)(m_totalTime * 2.3f) + 1) % 6;
            int wi = ((int)(m_totalTime * 1.1f)) % 5;
            if (ki != vi)
            {
                SimKillFeedEntry entry;
                entry.killer = names[ki];
                entry.victim = names[vi];
                entry.weapon = weapons[wi];
                entry.headshot = (wi == 2); // Sniper = headshot
                entry.timer = 5.0f;
                m_killFeed.push_front(entry);
                while (m_killFeed.size() > 5)
                    m_killFeed.pop_back();
            }
        }

        // Update kill feed timers
        auto it = m_killFeed.begin();
        while (it != m_killFeed.end())
        {
            it->timer -= deltaTime;
            if (it->timer <= 0.0f)
            {
                it = m_killFeed.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Spawn damage indicators periodically
        m_damageSpawnTimer += deltaTime;
        if (m_damageSpawnTimer > 8.0f)
        {
            m_damageSpawnTimer = 0.0f;
            SimDamageIndicator dmg;
            dmg.angle = fmod(m_totalTime * 1.7f, 6.2832f);
            dmg.intensity = 0.6f + 0.4f * sinf(m_totalTime);
            dmg.timer = 1.5f;
            dmg.maxTime = 1.5f;
            m_damageIndicators.push_back(dmg);
            m_health = std::max(15.0f, m_health - 15.0f);

            // Hit marker
            m_hitMarkerTimer = 0.3f;
            m_hitMarkerHeadshot = false;
        }

        // Update damage indicators
        auto dit = m_damageIndicators.begin();
        while (dit != m_damageIndicators.end())
        {
            dit->timer -= deltaTime;
            if (dit->timer <= 0.0f)
            {
                dit = m_damageIndicators.erase(dit);
            }
            else
            {
                ++dit;
            }
        }

        // Update hit marker
        if (m_hitMarkerTimer > 0.0f)
        {
            m_hitMarkerTimer -= deltaTime;
        }

        // Low health vignette
        float healthPct = m_health / m_maxHealth;
        if (healthPct < 0.3f)
        {
            m_vignetteIntensity = 1.0f - (healthPct / 0.3f);
            m_vignettePulse += deltaTime * 3.0f;
        }
        else
        {
            m_vignetteIntensity = 0.0f;
            m_vignettePulse = 0.0f;
        }

        // Weapon switch timer
        if (m_weaponSwitchTimer > 0.0f)
        {
            m_weaponSwitchTimer -= deltaTime;
        }

        // Simulate player rotation for minimap
        m_playerAngle += deltaTime * 0.3f;

        // Objective progress simulation
        m_objectiveProgress = 0.5f + 0.3f * sinf(m_totalTime * 0.2f);

        // Interaction prompt pulse
        m_interactionPulse += deltaTime * 2.0f;
    }

    void GameViewPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            HandleInput(m_lastDeltaTime);
            RenderGameContent();
        }
        else
        {
            // Panel is collapsed — release cursor if captured
            if (m_isCursorCaptured)
            {
                m_isCursorCaptured = false;
            }
        }
        EndPanel();
    }

    void GameViewPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Game View panel");
#ifdef _WIN32
        m_dsv.Reset();
        m_depthTexture.Reset();
        m_srv.Reset();
        m_rtv.Reset();
        m_renderTarget.Reset();
        m_context.Reset();
        m_device.Reset();
#endif
    }

#ifdef _WIN32
    void GameViewPanel::SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_device = device;
        m_context = context;
        CreateRenderTexture(640, 360);
    }

    void GameViewPanel::CreateRenderTexture(int width, int height)
    {
        if (!m_device || width <= 0 || height <= 0 ||
            (width == m_renderTextureWidth && height == m_renderTextureHeight && m_srv))
            return;

        m_dsv.Reset();
        m_depthTexture.Reset();
        m_srv.Reset();
        m_rtv.Reset();
        m_renderTarget.Reset();

        D3D11_TEXTURE2D_DESC colorDesc{};
        colorDesc.Width = static_cast<UINT>(width);
        colorDesc.Height = static_cast<UINT>(height);
        colorDesc.MipLevels = 1;
        colorDesc.ArraySize = 1;
        colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorDesc.SampleDesc.Count = 1;
        colorDesc.Usage = D3D11_USAGE_DEFAULT;
        colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(m_device->CreateTexture2D(&colorDesc, nullptr, &m_renderTarget)) ||
            FAILED(m_device->CreateRenderTargetView(m_renderTarget.Get(), nullptr, &m_rtv)) ||
            FAILED(m_device->CreateShaderResourceView(m_renderTarget.Get(), nullptr, &m_srv)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "GameViewPanel: failed to create color render target");
            return;
        }

        D3D11_TEXTURE2D_DESC depthDesc{};
        depthDesc.Width = static_cast<UINT>(width);
        depthDesc.Height = static_cast<UINT>(height);
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthTexture)) ||
            FAILED(m_device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, &m_dsv)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "GameViewPanel: failed to create depth target");
            return;
        }

        m_renderTextureWidth = width;
        m_renderTextureHeight = height;
    }

    bool GameViewPanel::RenderWorldToTexture(int width, int height)
    {
        CreateRenderTexture(width, height);
        if (!m_context || !m_graphics || !m_world || !m_rtv || !m_dsv)
            return false;

        auto cameras = m_world->GetRegistry().view<::Transform, ::Camera>();
        EntityID cameraEntity = entt::null;
        for (EntityID entity : cameras)
        {
            if (cameraEntity == entt::null)
                cameraEntity = entity;
            if (cameras.get<::Camera>(entity).isMainCamera)
            {
                cameraEntity = entity;
                break;
            }
        }
        if (cameraEntity == entt::null)
            return false;

        const auto& transform = cameras.get<::Transform>(cameraEntity);
        const auto& camera = cameras.get<::Camera>(cameraEntity);
        using namespace DirectX;
        const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(XMConvertToRadians(transform.rotation.x),
                                                               XMConvertToRadians(transform.rotation.y),
                                                               XMConvertToRadians(transform.rotation.z));
        const XMVECTOR eye = XMLoadFloat3(&transform.position);
        const XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
        const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rotation);
        const XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorAdd(eye, forward), up);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const XMMATRIX proj =
            XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.fov), aspect, camera.nearPlane, camera.farPlane);

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> previousRTV;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> previousDSV;
        m_context->OMGetRenderTargets(1, previousRTV.GetAddressOf(), previousDSV.GetAddressOf());
        UINT previousViewportCount = 1;
        D3D11_VIEWPORT previousViewport{};
        m_context->RSGetViewports(&previousViewportCount, &previousViewport);

        ID3D11RenderTargetView* target = m_rtv.Get();
        m_context->OMSetRenderTargets(1, &target, m_dsv.Get());
        const float clear[4] = {0.08f, 0.1f, 0.15f, 1.0f};
        m_context->ClearRenderTargetView(m_rtv.Get(), clear);
        m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);
        Spark::RenderWorldBasic(*m_world, *m_graphics, m_meshCache, view, proj);

        m_context->OMSetRenderTargets(1, previousRTV.GetAddressOf(), previousDSV.Get());
        if (previousViewportCount)
            m_context->RSSetViewports(1, &previousViewport);
        return true;
    }
#endif

    bool GameViewPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void GameViewPanel::SetWorld(World* world)
    {
        m_world = world;
        if (!m_world)
            return;
        auto cameras = m_world->GetRegistry().view<::Transform, ::Camera>();
        for (EntityID entity : cameras)
        {
            const auto& camera = cameras.get<::Camera>(entity);
            if (!camera.isMainCamera && cameras.size_hint() > 1)
                continue;
            const auto& transform = cameras.get<::Transform>(entity);
            m_cameraPosX = transform.position.x;
            m_cameraPosY = transform.position.y;
            m_cameraPosZ = transform.position.z;
            m_cameraPitch = DirectX::XMConvertToRadians(transform.rotation.x);
            m_cameraYaw = DirectX::XMConvertToRadians(transform.rotation.y);
            break;
        }
    }

    void GameViewPanel::RenderToolbar()
    {
        const char* resLabels[] = {"Free", "1920x1080", "1280x720", "2560x1440", "3840x2160"};
        ImGui::SetNextItemWidth(100);
        int resIdx = (int)m_resolution;
        if (ImGui::Combo("##Resolution", &resIdx, resLabels, IM_ARRAYSIZE(resLabels)))
        {
            m_resolution = (Resolution)resIdx;
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Game View resolution changed to: %s", resLabels[resIdx]);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Output Resolution");

        ImGui::SameLine();
        ImGui::Checkbox("Lock AR", &m_lockAspectRatio);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lock Aspect Ratio (16:9)");

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        ImGui::Checkbox(ICON_FA_CROSSHAIRS " HUD", &m_showHUD);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_MAP " Map", &m_showMinimap);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_CHART_BAR " Stats", &m_showStats);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_SHIELD " Score", &m_showScoreboard);

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        // Simulate actions
        if (ImGui::Button(ICON_FA_BOLT " Damage"))
        {
            m_health = std::max(5.0f, m_health - 25.0f);
            SimDamageIndicator dmg;
            dmg.angle = fmod(m_totalTime * 2.1f, 6.2832f);
            dmg.intensity = 0.8f;
            dmg.timer = 1.5f;
            dmg.maxTime = 1.5f;
            m_damageIndicators.push_back(dmg);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Simulate taking damage");
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_CROSSHAIRS " Hit"))
        {
            m_hitMarkerTimer = 0.3f;
            m_hitMarkerHeadshot = (((int)(m_totalTime * 10)) % 3 == 0);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Simulate hit marker");

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        if (ImGui::Button(m_maximized ? ICON_FA_COMPRESS : ICON_FA_EXPAND))
        {
            m_maximized = !m_maximized;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(m_maximized ? "Restore" : "Maximize");

        ImGui::Separator();
    }

    ImVec2 GameViewPanel::GetConstrainedViewportSize(const ImVec2& available) const
    {
        float targetW = available.x;
        float targetH = available.y;

        switch (m_resolution)
        {
        case Resolution::R1920x1080:
            targetW = 1920.0f;
            targetH = 1080.0f;
            break;
        case Resolution::R1280x720:
            targetW = 1280.0f;
            targetH = 720.0f;
            break;
        case Resolution::R2560x1440:
            targetW = 2560.0f;
            targetH = 1440.0f;
            break;
        case Resolution::R3840x2160:
            targetW = 3840.0f;
            targetH = 2160.0f;
            break;
        case Resolution::Free:
        default:
            if (m_lockAspectRatio)
            {
                float availAR = available.x / available.y;
                if (availAR > m_aspectRatio)
                {
                    targetW = available.y * m_aspectRatio;
                    targetH = available.y;
                }
                else
                {
                    targetW = available.x;
                    targetH = available.x / m_aspectRatio;
                }
            }
            return ImVec2(targetW, targetH);
        }

        // For fixed resolutions, scale down to fit available space
        float scaleX = available.x / targetW;
        float scaleY = available.y / targetH;
        float scale = std::min(scaleX, scaleY);
        if (scale > 1.0f)
            scale = 1.0f;

        return ImVec2(targetW * scale, targetH * scale);
    }

    void GameViewPanel::HandleInput(float deltaTime)
    {
        ImGuiIO& io = ImGui::GetIO();
        bool isWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        bool isWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

        // Click inside the game view to capture cursor
        if (m_isPlaying && isWindowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_isCursorCaptured)
        {
            // Only capture if the click is below the toolbar area (not on toolbar widgets)
            if (!io.WantCaptureMouse || isWindowFocused)
            {
                m_isCursorCaptured = true;
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Game View cursor captured");
            }
        }

        // Escape releases cursor capture
        if (m_isCursorCaptured && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_isCursorCaptured = false;
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Game View cursor released (Escape)");
        }

        // Release capture if window loses focus
        if (m_isCursorCaptured && !isWindowFocused)
        {
            m_isCursorCaptured = false;
        }

        if (!m_isCursorCaptured)
            return;

        // --- Mouse look ---
        ImVec2 mouseDelta = io.MouseDelta;
        m_cameraYaw += mouseDelta.x * m_mouseSensitivity;
        m_cameraPitch -= mouseDelta.y * m_mouseSensitivity;

        // Clamp pitch to avoid gimbal lock
        if (m_cameraPitch > 1.5f)
            m_cameraPitch = 1.5f;
        if (m_cameraPitch < -1.5f)
            m_cameraPitch = -1.5f;

        // Update player angle for minimap
        m_playerAngle = m_cameraYaw;

        // --- WASD movement ---
        float cosP = cosf(m_cameraPitch);
        float forwardX = cosP * sinf(m_cameraYaw);
        float forwardY = sinf(m_cameraPitch);
        float forwardZ = cosP * cosf(m_cameraYaw);

        float rightX = cosf(m_cameraYaw);
        float rightZ = -sinf(m_cameraYaw);

        float speed = m_moveSpeed * deltaTime;

        // Shift to sprint
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift))
        {
            speed *= 2.0f;
            m_stamina = std::max(0.0f, m_stamina - 20.0f * deltaTime);
        }

        float dx = 0.0f, dy = 0.0f, dz = 0.0f;

        if (ImGui::IsKeyDown(ImGuiKey_W))
        {
            dx += forwardX * speed;
            dy += forwardY * speed;
            dz += forwardZ * speed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S))
        {
            dx -= forwardX * speed;
            dy -= forwardY * speed;
            dz -= forwardZ * speed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_A))
        {
            dx -= rightX * speed;
            dz -= rightZ * speed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D))
        {
            dx += rightX * speed;
            dz += rightZ * speed;
        }

        // Space / Ctrl for up / down
        if (ImGui::IsKeyDown(ImGuiKey_Space))
        {
            dy += speed;
        }
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
        {
            dy -= speed;
        }

        m_cameraPosX += dx;
        m_cameraPosY += dy;
        m_cameraPosZ += dz;

        if (m_world)
        {
            auto cameras = m_world->GetRegistry().view<::Transform, ::Camera>();
            for (EntityID entity : cameras)
            {
                const auto& camera = cameras.get<::Camera>(entity);
                if (!camera.isMainCamera && cameras.size_hint() > 1)
                    continue;
                auto& transform = cameras.get<::Transform>(entity);
                transform.position = {m_cameraPosX, m_cameraPosY, m_cameraPosZ};
                transform.rotation.x = DirectX::XMConvertToDegrees(m_cameraPitch);
                transform.rotation.y = DirectX::XMConvertToDegrees(m_cameraYaw);
                break;
            }
        }

        // Simulate reload on R key
        if (ImGui::IsKeyPressed(ImGuiKey_R) && !m_isReloading && m_ammo < m_maxAmmo)
        {
            m_isReloading = true;
            m_reloadProgress = 0.0f;
        }

        // Simulate weapon switch on 1-5 keys
        for (int i = 0; i < 5; i++)
        {
            ImGuiKey key = static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_1) + i);
            if (ImGui::IsKeyPressed(key))
            {
                m_currentWeaponSlot = i + 1;
                static const char* weaponNames[] = {"Pistol", "Shotgun", "Assault Rifle", "Sniper Rifle",
                                                    "Rocket Launcher"};
                m_currentWeaponName = weaponNames[i];
                m_weaponSwitchTimer = 2.0f;
                m_weaponSwitchName = m_currentWeaponName;
                m_weaponSwitchSlot = i + 1;
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Game View weapon switched to slot %d: %s", i + 1,
                                weaponNames[i]);
            }
        }

        // Tab toggles scoreboard
        if (ImGui::IsKeyDown(ImGuiKey_Tab))
        {
            m_showScoreboard = true;
        }
        else
        {
            m_showScoreboard = false;
        }

        // Simulate firing on left click
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !m_isReloading && m_ammo > 0)
        {
            if (fmod(m_totalTime, 0.12f) < deltaTime)
            {
                m_ammo = std::max(0, m_ammo - 1);
                m_hitMarkerTimer = 0.15f;
                m_hitMarkerHeadshot = false;
            }
        }
    }

    void GameViewPanel::RenderGameContent()
    {
        ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.x <= 0 || available.y <= 0)
            return;

        SPARK_LOG_ONCE(Spark::LogLevel::Info, Spark::LogCategory::Editor, "Game View rendering first frame (%.0fx%.0f)",
                       available.x, available.y);

        ImVec2 viewportSize = GetConstrainedViewportSize(available);

        // Center the viewport if constrained
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float offsetX = (available.x - viewportSize.x) * 0.5f;
        float offsetY = (available.y - viewportSize.y) * 0.5f;
        ImVec2 pos(cursorPos.x + offsetX, cursorPos.y + offsetY);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Letterbox bars if viewport is smaller than available area
        if (offsetX > 0 || offsetY > 0)
        {
            drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + available.x, cursorPos.y + available.y),
                                    IM_COL32(0, 0, 0, 255));
        }

        bool renderedWorld = false;
#ifdef _WIN32
        renderedWorld = RenderWorldToTexture(std::max(1, static_cast<int>(viewportSize.x)),
                                             std::max(1, static_cast<int>(viewportSize.y)));
        if (renderedWorld && m_srv)
        {
            ImGui::SetCursorScreenPos(pos);
            ImGui::Image(reinterpret_cast<ImTextureID>(m_srv.Get()), viewportSize);
        }
#endif

        // Keep a useful preview when a scene has no main camera or graphics is unavailable.
        if (!renderedWorld)
        {
            drawList->AddRectFilled(pos, ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y),
                                    IM_COL32(20, 22, 28, 255));

            // Simulated sky gradient
            float horizonY = viewportSize.y * 0.55f;
            for (int y = 0; y < (int)horizonY; y++)
            {
                float t = (float)y / horizonY;
                int r = (int)(15 + t * 35);
                int g = (int)(25 + t * 50);
                int b = (int)(50 + t * 60);
                drawList->AddLine(ImVec2(pos.x, pos.y + y), ImVec2(pos.x + viewportSize.x, pos.y + y),
                                  IM_COL32(r, g, b, 255));
            }

            // Ground
            float groundY = pos.y + horizonY;
            drawList->AddRectFilled(ImVec2(pos.x, groundY), ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y),
                                    IM_COL32(32, 38, 28, 255));

            // Perspective grid on ground
            float cx = pos.x + viewportSize.x * 0.5f;
            for (int i = -15; i <= 15; i++)
            {
                float spread = (float)i * viewportSize.x * 0.06f;
                float topX = cx + spread * 0.1f;
                float bottomX = cx + spread;
                drawList->AddLine(ImVec2(topX, groundY), ImVec2(bottomX, pos.y + viewportSize.y),
                                  IM_COL32(45, 52, 38, 180));
            }
            for (int i = 1; i <= 8; i++)
            {
                float t = (float)i / 8.0f;
                float y = groundY + t * (viewportSize.y - horizonY);
                float alpha = 180.0f * (1.0f - t * 0.5f);
                drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + viewportSize.x, y),
                                  IM_COL32(45, 52, 38, (int)alpha));
            }

            // Some distant structures for atmosphere
            float structY = groundY - 5.0f;
            for (int i = 0; i < 5; i++)
            {
                float sx = pos.x + viewportSize.x * (0.1f + i * 0.2f);
                float sw = 30.0f + (float)((i * 17) % 40);
                float sh = 15.0f + (float)((i * 23) % 30);
                drawList->AddRectFilled(ImVec2(sx, structY - sh), ImVec2(sx + sw, structY), IM_COL32(40, 45, 50, 150));
            }
        }

        // "Game View" center text (subtle) — show capture hint when not captured
        if (!m_isCursorCaptured)
        {
            const char* label =
                m_isPlaying ? ICON_FA_GAMEPAD " Click to enter Game View" : ICON_FA_PLAY " Press F5 to play";
            ImVec2 textSize = ImGui::CalcTextSize(label);
            ImVec2 textPos(pos.x + (viewportSize.x - textSize.x) * 0.5f, pos.y + viewportSize.y * 0.45f);
            drawList->AddText(textPos, IM_COL32(80, 100, 120, 140), label);

            const char* hint = "Press ESC to release cursor";
            ImVec2 hintSize = ImGui::CalcTextSize(hint);
            ImVec2 hintPos(pos.x + (viewportSize.x - hintSize.x) * 0.5f, pos.y + viewportSize.y * 0.45f + 20.0f);
            drawList->AddText(hintPos, IM_COL32(80, 100, 120, 80), hint);
        }

        // FPS HUD overlay
        if (m_showHUD)
        {
            RenderFPSHUD();
        }

        // Advance cursor — use full available area so the panel occupies space properly
        ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + available.y));
        ImGui::Dummy(ImVec2(available.x, 0));
    }

    void GameViewPanel::RenderFPSHUD()
    {
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        float pad = 14.0f;
        float toolbarH = 40.0f;

        float contentTop = winPos.y + toolbarH;
        float contentH = winSize.y - toolbarH;
        float contentCX = winPos.x + winSize.x * 0.5f;
        float contentCY = contentTop + contentH * 0.5f;

        // === Crosshair ===
        if (m_showCrosshair)
        {
            RenderCrosshair(contentCX, contentCY);
        }

        // === Hit Marker ===
        if (m_hitMarkerTimer > 0.0f)
        {
            RenderHitMarker(contentCX, contentCY);
        }

        // === Low Health Vignette ===
        if (m_vignetteIntensity > 0.01f)
        {
            RenderLowHealthVignette(winPos.x, contentTop, winSize.x, contentH);
        }

        // === Damage Indicators ===
        if (m_showDamageIndicators && !m_damageIndicators.empty())
        {
            RenderDamageIndicators(contentCX, contentCY, std::min(winSize.x, contentH) * 0.3f);
        }

        // === Health & Armor Bars (bottom-left) ===
        float barAreaW = 240.0f;
        float barBaseX = winPos.x + pad;
        float barBaseY = winPos.y + winSize.y - pad;
        RenderHealthArmorBars(barBaseX, barBaseY, barAreaW);

        // === Ammo Display (bottom-right) ===
        float ammoX = winPos.x + winSize.x - pad;
        float ammoY = winPos.y + winSize.y - pad;
        RenderAmmoDisplay(ammoX, ammoY);

        // === Weapon Info (bottom-right, above ammo) ===
        RenderWeaponInfo(ammoX, ammoY - 58.0f);

        // === Minimap (top-right) ===
        if (m_showMinimap)
        {
            float mapSize = std::min(140.0f, winSize.x * 0.15f);
            RenderMinimap(winPos.x + winSize.x - pad - mapSize, contentTop + pad, mapSize);
        }

        // === Kill Feed (top-right, below minimap) ===
        if (m_showKillFeed && !m_killFeed.empty())
        {
            float feedY = contentTop + pad + (m_showMinimap ? 160.0f : 0.0f);
            RenderKillFeed(winPos.x + winSize.x - pad, feedY);
        }

        // === Objective Bar (top-center) ===
        RenderObjectiveBar(contentCX - 150.0f, contentTop + pad, 300.0f);

        // === Interaction Prompt (center-bottom) ===
        m_showInteraction = ((int)(m_totalTime * 0.5f) % 3 == 0);
        if (m_showInteraction)
        {
            RenderInteractionPrompt(contentCX, contentCY + contentH * 0.25f);
        }

        // === Scoreboard Overlay ===
        if (m_showScoreboard)
        {
            float sbW = std::min(500.0f, winSize.x * 0.7f);
            float sbH = std::min(300.0f, contentH * 0.6f);
            RenderScoreboard(contentCX, contentCY, sbW, sbH);
        }

        // === Stats overlay (top-left) ===
        if (m_showStats)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            char stats[128];
            snprintf(stats, sizeof(stats),
                     "HP: %.0f/%.0f  ARM: %.0f  STM: %.0f\nAmmo: %d/%d (+%d)\nPos: %.1f, %.1f, %.1f", m_health,
                     m_maxHealth, m_armor, m_stamina, m_ammo, m_maxAmmo, m_reserveAmmo, m_cameraPosX, m_cameraPosY,
                     m_cameraPosZ);
            ImVec2 statPos(winPos.x + pad, contentTop + pad);
            dl->AddRectFilled(statPos, ImVec2(statPos.x + 220, statPos.y + 54), IM_COL32(0, 0, 0, 160), 4.0f);
            dl->AddText(ImVec2(statPos.x + 6, statPos.y + 4), IM_COL32(200, 220, 200, 220), stats);
        }
    }

    void GameViewPanel::RenderCrosshair(float cx, float cy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float size = 14.0f;
        float gap = 4.0f;
        float thick = 2.0f;
        ImU32 col = IM_COL32(255, 255, 255, 200);
        ImU32 colShadow = IM_COL32(0, 0, 0, 80);

        // Dynamic spread from movement simulation
        float spread = 2.0f * sinf(m_totalTime * 1.5f);
        spread = std::max(0.0f, spread);
        float g = gap + spread;

        // Shadow (offset)
        float off = 1.0f;
        dl->AddLine(ImVec2(cx - size + off, cy + off), ImVec2(cx - g + off, cy + off), colShadow, thick);
        dl->AddLine(ImVec2(cx + g + off, cy + off), ImVec2(cx + size + off, cy + off), colShadow, thick);
        dl->AddLine(ImVec2(cx + off, cy - size + off), ImVec2(cx + off, cy - g + off), colShadow, thick);
        dl->AddLine(ImVec2(cx + off, cy + g + off), ImVec2(cx + off, cy + size + off), colShadow, thick);

        // Main crosshair
        dl->AddLine(ImVec2(cx - size, cy), ImVec2(cx - g, cy), col, thick);
        dl->AddLine(ImVec2(cx + g, cy), ImVec2(cx + size, cy), col, thick);
        dl->AddLine(ImVec2(cx, cy - size), ImVec2(cx, cy - g), col, thick);
        dl->AddLine(ImVec2(cx, cy + g), ImVec2(cx, cy + size), col, thick);

        // Center dot
        dl->AddCircleFilled(ImVec2(cx, cy), 1.5f, col, 8);
    }

    void GameViewPanel::RenderHealthArmorBars(float baseX, float baseY, float width)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float barH = 14.0f;
        float spacing = 4.0f;

        // === Armor bar (above health) ===
        float armorY = baseY - barH * 2 - spacing * 3 - 16.0f;
        float armorPct = Clamp01(m_armor / m_maxArmor);
        if (m_armor > 0)
        {
            // Background
            dl->AddRectFilled(ImVec2(baseX, armorY), ImVec2(baseX + width, armorY + barH), IM_COL32(0, 0, 0, 160),
                              3.0f);
            // Border
            dl->AddRect(ImVec2(baseX, armorY), ImVec2(baseX + width, armorY + barH), IM_COL32(60, 70, 90, 200), 3.0f);
            // Fill
            dl->AddRectFilled(ImVec2(baseX + 2, armorY + 2),
                              ImVec2(baseX + 2 + (width - 4) * armorPct, armorY + barH - 2),
                              IM_COL32(45, 140, 240, 220), 2.0f);
            // Icon + text
            char armorTxt[32];
            snprintf(armorTxt, sizeof(armorTxt), ICON_FA_SHIELD " %.0f", m_armor);
            dl->AddText(ImVec2(baseX + 4, armorY - 1), IM_COL32(100, 170, 255, 240), armorTxt);
        }

        // === Health bar ===
        float healthY = armorY + barH + spacing;
        float healthPct = Clamp01(m_health / m_maxHealth);

        ImU32 healthCol = healthPct > 0.5f    ? IM_COL32(76, 175, 80, 230)
                          : healthPct > 0.25f ? IM_COL32(245, 166, 35, 230)
                                              : IM_COL32(229, 57, 53, 230);

        // Background
        dl->AddRectFilled(ImVec2(baseX, healthY), ImVec2(baseX + width, healthY + barH), IM_COL32(0, 0, 0, 160), 3.0f);
        // Border
        dl->AddRect(ImVec2(baseX, healthY), ImVec2(baseX + width, healthY + barH), IM_COL32(60, 70, 60, 200), 3.0f);
        // Fill with gradient effect
        dl->AddRectFilled(ImVec2(baseX + 2, healthY + 2),
                          ImVec2(baseX + 2 + (width - 4) * healthPct, healthY + barH - 2), healthCol, 2.0f);
        // Highlight stripe
        dl->AddRectFilled(ImVec2(baseX + 2, healthY + 2), ImVec2(baseX + 2 + (width - 4) * healthPct, healthY + 5),
                          IM_COL32(255, 255, 255, 30), 2.0f);

        // Health text
        char healthTxt[32];
        snprintf(healthTxt, sizeof(healthTxt), ICON_FA_HEART " %.0f / %.0f", m_health, m_maxHealth);
        dl->AddText(ImVec2(baseX + 4, healthY - 1), IM_COL32(255, 255, 255, 240), healthTxt);

        // === Stamina bar (thin, below health) ===
        float stamY = healthY + barH + spacing;
        float stamPct = Clamp01(m_stamina / 100.0f);
        float stamH = 4.0f;
        dl->AddRectFilled(ImVec2(baseX, stamY), ImVec2(baseX + width, stamY + stamH), IM_COL32(0, 0, 0, 100), 2.0f);
        dl->AddRectFilled(ImVec2(baseX + 1, stamY + 1), ImVec2(baseX + 1 + (width - 2) * stamPct, stamY + stamH - 1),
                          IM_COL32(255, 200, 50, 180), 1.0f);
    }

    void GameViewPanel::RenderAmmoDisplay(float baseX, float baseY)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Ammo panel background
        float panelW = 160.0f;
        float panelH = 50.0f;
        float px = baseX - panelW;
        float py = baseY - panelH;

        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + panelW, py + panelH), IM_COL32(0, 0, 0, 160), 4.0f);
        dl->AddRect(ImVec2(px, py), ImVec2(px + panelW, py + panelH), IM_COL32(80, 85, 90, 150), 4.0f);

        // Ammo icon
        dl->AddText(ImVec2(px + 8, py + 6), IM_COL32(200, 200, 200, 200), ICON_FA_CROSSHAIRS);

        // Current ammo
        char ammoMain[16];
        snprintf(ammoMain, sizeof(ammoMain), "%d", m_ammo);
        ImU32 ammoCol = m_ammo <= 5    ? IM_COL32(229, 57, 53, 255)
                        : m_ammo <= 10 ? IM_COL32(245, 166, 35, 255)
                                       : IM_COL32(255, 255, 255, 255);
        dl->AddText(ImVec2(px + 28, py + 4), ammoCol, ammoMain);

        // Separator
        dl->AddText(ImVec2(px + 62, py + 4), IM_COL32(120, 120, 120, 200), "/");

        // Max ammo
        char ammoMax[16];
        snprintf(ammoMax, sizeof(ammoMax), "%d", m_maxAmmo);
        dl->AddText(ImVec2(px + 72, py + 4), IM_COL32(180, 180, 180, 200), ammoMax);

        // Reserve ammo
        char reserveTxt[32];
        snprintf(reserveTxt, sizeof(reserveTxt), "+%d reserve", m_reserveAmmo);
        dl->AddText(ImVec2(px + 8, py + 22), IM_COL32(140, 140, 140, 180), reserveTxt);

        // Reload progress bar
        if (m_isReloading)
        {
            float reloadY = py + panelH - 6;
            dl->AddRectFilled(ImVec2(px + 4, reloadY), ImVec2(px + panelW - 4, reloadY + 4), IM_COL32(0, 0, 0, 150),
                              2.0f);
            dl->AddRectFilled(ImVec2(px + 5, reloadY + 1),
                              ImVec2(px + 5 + (panelW - 10) * m_reloadProgress, reloadY + 3),
                              IM_COL32(245, 166, 35, 230), 1.0f);
            dl->AddText(ImVec2(px + 8, py + 34), IM_COL32(245, 166, 35, 220), "RELOADING...");
        }

        // Firing mode indicator
        if (!m_isReloading)
        {
            dl->AddText(ImVec2(px + 100, py + 22), IM_COL32(100, 100, 100, 150), "AUTO");
        }
    }

    void GameViewPanel::RenderMinimap(float x, float y, float size)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Minimap background (circular)
        float cx = x + size * 0.5f;
        float cy = y + size * 0.5f;
        float radius = size * 0.5f;

        dl->AddCircleFilled(ImVec2(cx, cy), radius, IM_COL32(15, 20, 25, 200), 32);
        dl->AddCircle(ImVec2(cx, cy), radius, IM_COL32(60, 70, 80, 220), 32, 2.0f);
        dl->AddCircle(ImVec2(cx, cy), radius * 0.5f, IM_COL32(40, 50, 60, 100), 32, 1.0f);

        // Grid lines
        dl->AddLine(ImVec2(cx - radius, cy), ImVec2(cx + radius, cy), IM_COL32(40, 50, 60, 80));
        dl->AddLine(ImVec2(cx, cy - radius), ImVec2(cx, cy + radius), IM_COL32(40, 50, 60, 80));

        // Cardinal directions
        dl->AddText(ImVec2(cx - 3, y + 2), IM_COL32(200, 200, 200, 150), "N");
        dl->AddText(ImVec2(cx - 3, y + size - 14), IM_COL32(150, 150, 150, 100), "S");

        // Player triangle (center, rotates)
        float pAngle = m_playerAngle;
        float triSize = 6.0f;
        ImVec2 p1(cx + sinf(pAngle) * triSize, cy - cosf(pAngle) * triSize);
        ImVec2 p2(cx + sinf(pAngle + 2.4f) * triSize * 0.6f, cy - cosf(pAngle + 2.4f) * triSize * 0.6f);
        ImVec2 p3(cx + sinf(pAngle - 2.4f) * triSize * 0.6f, cy - cosf(pAngle - 2.4f) * triSize * 0.6f);
        dl->AddTriangleFilled(p1, p2, p3, IM_COL32(45, 140, 240, 240));
        dl->AddTriangle(p1, p2, p3, IM_COL32(100, 180, 255, 255), 1.5f);

        // Simulated blips
        float blipData[][3] = {
            {0.3f, 0.2f, 1.0f},
            {-0.2f, 0.4f, 1.0f},
            {0.1f, -0.3f, 2.0f},
            {-0.4f, -0.1f, 3.0f},
        };
        ImU32 blipColors[] = {
            IM_COL32(229, 57, 53, 200),
            IM_COL32(229, 57, 53, 200),
            IM_COL32(76, 175, 80, 200),
            IM_COL32(245, 200, 35, 200),
        };

        for (int i = 0; i < 4; i++)
        {
            float bx = cx + blipData[i][0] * radius * 0.8f;
            float by = cy + blipData[i][1] * radius * 0.8f;
            float dist = sqrtf(blipData[i][0] * blipData[i][0] + blipData[i][1] * blipData[i][1]);
            if (dist < 1.0f)
            {
                dl->AddCircleFilled(ImVec2(bx, by), 3.0f, blipColors[i], 8);
                if (i == 3)
                {
                    float pulse = 0.5f + 0.5f * sinf(m_totalTime * 3.0f);
                    dl->AddCircle(ImVec2(bx, by), 5.0f + pulse * 3.0f, IM_COL32(245, 200, 35, (int)(100 * pulse)), 12,
                                  1.0f);
                }
            }
        }

        // Compass bearing at top
        char bearing[8];
        int degrees = ((int)(m_playerAngle * 57.2958f) % 360 + 360) % 360;
        snprintf(bearing, sizeof(bearing),
                 "%d"
                 "\xc2\xb0",
                 degrees);
        ImVec2 bearingSize = ImGui::CalcTextSize(bearing);
        dl->AddText(ImVec2(cx - bearingSize.x * 0.5f, y - 14), IM_COL32(180, 190, 200, 180), bearing);
    }

    void GameViewPanel::RenderDamageIndicators(float cx, float cy, float radius)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (const auto& dmg : m_damageIndicators)
        {
            float alpha = Clamp01(dmg.timer / dmg.maxTime);
            float a = dmg.angle;

            float arcLen = 0.4f;
            int segments = 8;
            ImU32 col = IM_COL32(229, 57, 53, (int)(alpha * dmg.intensity * 200));

            float innerR = radius * 0.7f;
            float outerR = radius;

            for (int s = 0; s < segments; s++)
            {
                float t1 = a - arcLen * 0.5f + arcLen * s / segments;
                float t2 = a - arcLen * 0.5f + arcLen * (s + 1) / segments;

                ImVec2 p1(cx + cosf(t1) * innerR, cy + sinf(t1) * innerR);
                ImVec2 p2(cx + cosf(t2) * innerR, cy + sinf(t2) * innerR);
                ImVec2 p3(cx + cosf(t2) * outerR, cy + sinf(t2) * outerR);
                ImVec2 p4(cx + cosf(t1) * outerR, cy + sinf(t1) * outerR);

                dl->AddQuadFilled(p1, p2, p3, p4, col);
            }
        }
    }

    void GameViewPanel::RenderKillFeed(float x, float y)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float entryH = 20.0f;
        float yOff = 0.0f;

        for (const auto& entry : m_killFeed)
        {
            float alpha = Clamp01(entry.timer / 5.0f);
            if (entry.timer < 1.0f)
                alpha = entry.timer;

            char feedText[128];
            snprintf(feedText, sizeof(feedText), "%s %s%s %s", entry.killer.c_str(),
                     entry.headshot ? ICON_FA_BULLSEYE " " : "", entry.weapon.c_str(), entry.victim.c_str());

            ImVec2 textSize = ImGui::CalcTextSize(feedText);
            float bgX = x - textSize.x - 16;
            float bgY = y + yOff;

            dl->AddRectFilled(ImVec2(bgX, bgY), ImVec2(bgX + textSize.x + 12, bgY + entryH),
                              IM_COL32(0, 0, 0, (int)(140 * alpha)), 3.0f);

            if (entry.headshot)
            {
                dl->AddRectFilled(ImVec2(bgX, bgY), ImVec2(bgX + 3, bgY + entryH),
                                  IM_COL32(229, 57, 53, (int)(200 * alpha)), 3.0f);
            }

            ImU32 textCol = IM_COL32(220, 220, 220, (int)(220 * alpha));
            dl->AddText(ImVec2(bgX + 6, bgY + 2), textCol, feedText);

            yOff += entryH + 2.0f;
        }
    }

    void GameViewPanel::RenderHitMarker(float cx, float cy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float alpha = Clamp01(m_hitMarkerTimer / 0.3f);
        float size = 10.0f + (1.0f - alpha) * 4.0f;
        float thick = 2.0f;

        ImU32 col = m_hitMarkerHeadshot ? IM_COL32(229, 57, 53, (int)(255 * alpha))
                                        : IM_COL32(255, 255, 255, (int)(255 * alpha));

        float inner = 3.0f;

        // X-shaped hit marker
        dl->AddLine(ImVec2(cx - size, cy - size), ImVec2(cx - inner, cy - inner), col, thick);
        dl->AddLine(ImVec2(cx + inner, cy - inner), ImVec2(cx + size, cy - size), col, thick);
        dl->AddLine(ImVec2(cx - size, cy + size), ImVec2(cx - inner, cy + inner), col, thick);
        dl->AddLine(ImVec2(cx + inner, cy + inner), ImVec2(cx + size, cy + size), col, thick);

        if (m_hitMarkerHeadshot && alpha > 0.5f)
        {
            const char* hsText = "HEADSHOT";
            ImVec2 hsSize = ImGui::CalcTextSize(hsText);
            dl->AddText(ImVec2(cx - hsSize.x * 0.5f, cy - size - 18), IM_COL32(229, 57, 53, (int)(255 * alpha)),
                        hsText);
        }
    }

    void GameViewPanel::RenderWeaponInfo(float x, float y)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float panelW = 160.0f;
        float panelH = 24.0f;
        float px = x - panelW;

        dl->AddRectFilled(ImVec2(px, y), ImVec2(px + panelW, y + panelH), IM_COL32(0, 0, 0, 100), 3.0f);

        char slotTxt[32];
        snprintf(slotTxt, sizeof(slotTxt), "[%d] %s", m_currentWeaponSlot, m_currentWeaponName.c_str());
        dl->AddText(ImVec2(px + 6, y + 4), IM_COL32(180, 190, 200, 200), slotTxt);

        // Weapon slot dots (5 slots)
        float dotStartX = px + panelW - 60;
        for (int i = 1; i <= 5; i++)
        {
            float dx = dotStartX + (float)(i - 1) * 12.0f;
            float dy = y + panelH * 0.5f;
            ImU32 dotCol = (i == m_currentWeaponSlot) ? IM_COL32(45, 140, 240, 220) : IM_COL32(80, 80, 80, 150);
            dl->AddCircleFilled(ImVec2(dx, dy), 3.0f, dotCol, 8);
        }
    }

    void GameViewPanel::RenderObjectiveBar(float x, float y, float width)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float barH = 22.0f;

        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + barH), IM_COL32(0, 0, 0, 140), 4.0f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + barH), IM_COL32(60, 70, 80, 150), 4.0f);

        float fillW = (width - 4) * Clamp01(m_objectiveProgress);
        dl->AddRectFilled(ImVec2(x + 2, y + 2), ImVec2(x + 2 + fillW, y + barH - 2), IM_COL32(45, 140, 240, 160), 2.0f);

        char objText[64];
        snprintf(objText, sizeof(objText), ICON_FA_BULLSEYE " %s — %.0f%%", m_objectiveText.c_str(),
                 m_objectiveProgress * 100.0f);
        ImVec2 textSize = ImGui::CalcTextSize(objText);
        dl->AddText(ImVec2(x + (width - textSize.x) * 0.5f, y + 3), IM_COL32(255, 255, 255, 220), objText);
    }

    void GameViewPanel::RenderScoreboard(float cx, float cy, float width, float height)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float x = cx - width * 0.5f;
        float y = cy - height * 0.5f;

        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), IM_COL32(10, 12, 18, 230), 6.0f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height), IM_COL32(45, 140, 240, 150), 6.0f, 0, 2.0f);

        const char* title = "SCOREBOARD";
        ImVec2 titleSize = ImGui::CalcTextSize(title);
        dl->AddText(ImVec2(cx - titleSize.x * 0.5f, y + 8), IM_COL32(45, 140, 240, 255), title);

        float headerY = y + 30;
        float colName = x + 12;
        float colK = x + width * 0.5f;
        float colD = x + width * 0.6f;
        float colScore = x + width * 0.72f;
        float colPing = x + width * 0.87f;

        dl->AddText(ImVec2(colName, headerY), IM_COL32(150, 160, 170, 200), "Player");
        dl->AddText(ImVec2(colK, headerY), IM_COL32(150, 160, 170, 200), "K");
        dl->AddText(ImVec2(colD, headerY), IM_COL32(150, 160, 170, 200), "D");
        dl->AddText(ImVec2(colScore, headerY), IM_COL32(150, 160, 170, 200), "Score");
        dl->AddText(ImVec2(colPing, headerY), IM_COL32(150, 160, 170, 200), "Ping");

        dl->AddLine(ImVec2(x + 8, headerY + 16), ImVec2(x + width - 8, headerY + 16), IM_COL32(60, 70, 80, 150));

        float rowY = headerY + 22;
        float rowH = 20.0f;

        for (size_t i = 0; i < m_scoreboardEntries.size() && rowY + rowH < y + height - 4; i++)
        {
            const auto& e = m_scoreboardEntries[i];

            if (e.isLocal)
            {
                dl->AddRectFilled(ImVec2(x + 4, rowY - 2), ImVec2(x + width - 4, rowY + rowH - 2),
                                  IM_COL32(45, 140, 240, 40), 2.0f);
            }

            ImU32 textCol = e.isLocal ? IM_COL32(45, 180, 240, 255) : IM_COL32(200, 210, 220, 220);

            dl->AddText(ImVec2(colName, rowY), textCol, e.name.c_str());

            char buf[16];
            snprintf(buf, sizeof(buf), "%d", e.kills);
            dl->AddText(ImVec2(colK, rowY), textCol, buf);

            snprintf(buf, sizeof(buf), "%d", e.deaths);
            dl->AddText(ImVec2(colD, rowY), textCol, buf);

            snprintf(buf, sizeof(buf), "%d", e.score);
            dl->AddText(ImVec2(colScore, rowY), textCol, buf);

            ImU32 pingCol = e.ping < 50    ? IM_COL32(76, 175, 80, 200)
                            : e.ping < 100 ? IM_COL32(245, 166, 35, 200)
                                           : IM_COL32(229, 57, 53, 200);
            snprintf(buf, sizeof(buf), "%dms", e.ping);
            dl->AddText(ImVec2(colPing, rowY), pingCol, buf);

            rowY += rowH;
        }
    }

    void GameViewPanel::RenderLowHealthVignette(float x, float y, float w, float h)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float pulse = 0.7f + 0.3f * sinf(m_vignettePulse);
        float intensity = m_vignetteIntensity * pulse;
        int alpha = (int)(intensity * 80);

        float edgeW = w * 0.15f;

        // Left edge
        for (int i = 0; i < (int)edgeW; i++)
        {
            float t = 1.0f - (float)i / edgeW;
            dl->AddLine(ImVec2(x + i, y), ImVec2(x + i, y + h), IM_COL32(229, 57, 53, (int)(alpha * t * t)));
        }
        // Right edge
        for (int i = 0; i < (int)edgeW; i++)
        {
            float t = 1.0f - (float)i / edgeW;
            dl->AddLine(ImVec2(x + w - i, y), ImVec2(x + w - i, y + h), IM_COL32(229, 57, 53, (int)(alpha * t * t)));
        }
        // Top edge
        float edgeH = h * 0.1f;
        for (int i = 0; i < (int)edgeH; i++)
        {
            float t = 1.0f - (float)i / edgeH;
            dl->AddLine(ImVec2(x, y + i), ImVec2(x + w, y + i), IM_COL32(229, 57, 53, (int)(alpha * t * t)));
        }
        // Bottom edge
        for (int i = 0; i < (int)edgeH; i++)
        {
            float t = 1.0f - (float)i / edgeH;
            dl->AddLine(ImVec2(x, y + h - i), ImVec2(x + w, y + h - i), IM_COL32(229, 57, 53, (int)(alpha * t * t)));
        }
    }

    void GameViewPanel::RenderInteractionPrompt(float cx, float cy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float pulse = 0.7f + 0.3f * sinf(m_interactionPulse);
        int alpha = (int)(220 * pulse);

        const char* promptText = "[E] Pick up Shotgun";
        ImVec2 textSize = ImGui::CalcTextSize(promptText);

        float bgW = textSize.x + 20;
        float bgH = textSize.y + 12;
        float bgX = cx - bgW * 0.5f;
        float bgY = cy - bgH * 0.5f;

        dl->AddRectFilled(ImVec2(bgX, bgY), ImVec2(bgX + bgW, bgY + bgH), IM_COL32(0, 0, 0, (int)(140 * pulse)), 4.0f);
        dl->AddRect(ImVec2(bgX, bgY), ImVec2(bgX + bgW, bgY + bgH), IM_COL32(200, 200, 200, (int)(100 * pulse)), 4.0f);

        dl->AddText(ImVec2(cx - textSize.x * 0.5f, bgY + 6), IM_COL32(255, 255, 255, alpha), promptText);
    }

} // namespace SparkEditor
