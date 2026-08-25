#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <cmath>

struct ThirdPersonStarterState
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float verticalVelocity = 0.0f;
    float orbitYawDegrees = 0.0f;
    float orbitPitchDegrees = 25.0f;
    float orbitDistance = 8.0f;
    float elapsedSeconds = 0.0f;
    bool grounded = true;
    bool sprinting = false;
    bool pickupCollected = false;
    bool goalReached = false;
};

class ThirdPersonStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "ThirdPersonStarter";
        info.version = "0.2.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetSceneDefaults();
        ResetEntityHandles();
        m_jumpHeld = false;
        m_interactHeld = false;
        m_restartHeld = false;
        m_cameraResetHeld = false;
        m_sprintInput = false;

        if (!m_runtime.Load(context, "ThirdPersonStarter", {"Startup.sparkscene", "Scenes/Adventure.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_cameraEntity = scene.Find("Main Camera");
                                m_playerEntity = scene.Find("Player");
                                m_pickupEntity = scene.Find("Adventure Pickup");
                                m_goalEntity = scene.Find("Goal Beacon");
                                return scene.Get<Transform>(m_cameraEntity) && scene.Get<Camera>(m_cameraEntity) &&
                                       scene.Get<Transform>(m_playerEntity) &&
                                       scene.Get<MeshRenderer>(m_playerEntity) &&
                                       scene.Get<Transform>(m_pickupEntity) &&
                                       scene.Get<MeshRenderer>(m_pickupEntity) && scene.Get<Transform>(m_goalEntity) &&
                                       scene.Get<MeshRenderer>(m_goalEntity);
                            }))
        {
            ResetEntityHandles();
            m_context = nullptr;
            return false;
        }

        if (m_runtime.IsActive())
            CaptureSceneState();
        ResetAdventure();
        if (m_runtime.IsActive() && m_runtime.GetGraphics())
        {
            m_hudEntity = m_runtime.CreateSprite("ThirdPerson Objective HUD", "Assets/third_person_runtime_sheet.png",
                                                 Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0));
        }
        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetEntityHandles();
        m_sprintInput = false;
        m_context = nullptr;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        UpdateRuntimeInput(deltaTime);
        m_state.elapsedSeconds += deltaTime;
        if (!m_state.grounded)
        {
            m_state.verticalVelocity -= 18.0f * deltaTime;
            m_state.y += m_state.verticalVelocity * deltaTime;
            if (m_state.y <= m_groundY)
            {
                m_state.y = m_groundY;
                m_state.verticalVelocity = 0.0f;
                m_state.grounded = true;
            }
        }
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    void Move(float xAxis, float zAxis, float deltaTime)
    {
        if (!std::isfinite(xAxis) || !std::isfinite(zAxis) || !std::isfinite(deltaTime) || deltaTime <= 0.0f ||
            m_state.goalReached)
            return;

        xAxis = std::clamp(xAxis, -1.0f, 1.0f);
        zAxis = std::clamp(zAxis, -1.0f, 1.0f);
        NormalizeAxes(xAxis, zAxis);
        const float yaw = DirectX::XMConvertToRadians(m_state.orbitYawDegrees);
        const float worldX = std::cos(yaw) * xAxis + std::sin(yaw) * zAxis;
        const float worldZ = std::cos(yaw) * zAxis - std::sin(yaw) * xAxis;
        const float previousX = m_state.x;
        const float previousZ = m_state.z;
        const float moveSpeed = m_sprintInput ? 8.0f : 5.0f;
        m_state.sprinting = m_sprintInput && (std::abs(worldX) > 0.0001f || std::abs(worldZ) > 0.0001f);
        m_state.x = std::clamp(m_state.x + worldX * moveSpeed * deltaTime, -20.0f, 20.0f);
        m_state.z = std::clamp(m_state.z + worldZ * moveSpeed * deltaTime, -20.0f, 20.0f);
        const float movedX = m_state.x - previousX;
        const float movedZ = m_state.z - previousZ;
        if (std::abs(movedX) > 0.0001f || std::abs(movedZ) > 0.0001f)
            m_playerYawDegrees = std::atan2(movedX, movedZ) * kRadiansToDegrees;
    }

    bool Jump()
    {
        if (!m_state.grounded || m_state.goalReached)
            return false;
        m_state.grounded = false;
        m_state.verticalVelocity = 7.0f;
        return true;
    }

    void Orbit(float yawDeltaDegrees, float pitchDeltaDegrees, float zoomDelta)
    {
        if (!std::isfinite(yawDeltaDegrees) || !std::isfinite(pitchDeltaDegrees) || !std::isfinite(zoomDelta))
            return;
        m_state.orbitYawDegrees += yawDeltaDegrees;
        m_state.orbitPitchDegrees = std::clamp(m_state.orbitPitchDegrees + pitchDeltaDegrees, -10.0f, 75.0f);
        m_state.orbitDistance = std::clamp(m_state.orbitDistance + zoomDelta, 3.0f, 14.0f);
    }

    void SetSprintInput(bool sprinting) { m_sprintInput = sprinting; }

    void ResetCamera()
    {
        m_state.orbitYawDegrees = m_orbitSpawnYawDegrees;
        m_state.orbitPitchDegrees = m_orbitSpawnPitchDegrees;
        m_state.orbitDistance = m_orbitSpawnDistance;
    }

    bool TryCollectPickup()
    {
        if (m_state.pickupCollected || DistanceSquared(m_pickupPosition.x, m_pickupPosition.z) > 2.25f)
            return false;
        m_state.pickupCollected = true;
        return true;
    }

    bool TryReachGoal()
    {
        if (m_state.goalReached || !m_state.pickupCollected ||
            DistanceSquared(m_goalPosition.x, m_goalPosition.z) > 4.0f)
        {
            return false;
        }
        m_state.goalReached = true;
        m_state.sprinting = false;
        m_sprintInput = false;
        return true;
    }

    void ResetAdventure()
    {
        m_state = {};
        m_state.x = m_playerSpawnPosition.x;
        m_state.y = m_playerSpawnPosition.y;
        m_state.z = m_playerSpawnPosition.z;
        m_state.orbitYawDegrees = m_orbitSpawnYawDegrees;
        m_state.orbitPitchDegrees = m_orbitSpawnPitchDegrees;
        m_state.orbitDistance = m_orbitSpawnDistance;
        m_sprintInput = false;
        m_groundY = m_playerSpawnPosition.y;
        m_playerYawDegrees = m_playerSpawnRotation.y;
        SyncRuntimeState();
    }

    [[nodiscard]] const ThirdPersonStarterState& GetState() const { return m_state; }

  private:
    static constexpr float kRadiansToDegrees = 57.29577951308232f;

    static void NormalizeAxes(float& xAxis, float& zAxis)
    {
        const float magnitude = std::sqrt(xAxis * xAxis + zAxis * zAxis);
        if (magnitude > 1.0f)
        {
            xAxis /= magnitude;
            zAxis /= magnitude;
        }
    }

    void UpdateRuntimeInput(float deltaTime)
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        float xAxis = 0.0f;
        float zAxis = 0.0f;
        if (input->IsKeyDown('D'))
            xAxis += 1.0f;
        if (input->IsKeyDown('A'))
            xAxis -= 1.0f;
        if (input->IsKeyDown('W'))
            zAxis += 1.0f;
        if (input->IsKeyDown('S'))
            zAxis -= 1.0f;
        SetSprintInput(input->IsKeyDown(VK_SHIFT));
        Move(xAxis, zAxis, deltaTime);

        if (input->IsMouseButtonDown(1))
        {
            const MousePoint delta = input->GetMouseDelta();
            Orbit(static_cast<float>(delta.x) * 0.12f, static_cast<float>(delta.y) * 0.12f, 0.0f);
        }
        if (input->IsKeyDown('Z'))
            Orbit(0.0f, 0.0f, -6.0f * deltaTime);
        if (input->IsKeyDown('X'))
            Orbit(0.0f, 0.0f, 6.0f * deltaTime);

        const bool cameraResetDown = input->IsKeyDown('C');
        if (cameraResetDown && !m_cameraResetHeld)
            ResetCamera();
        m_cameraResetHeld = cameraResetDown;

        const bool jumpDown = input->IsKeyDown(VK_SPACE);
        if (jumpDown && !m_jumpHeld)
            Jump();
        m_jumpHeld = jumpDown;

        const bool interactDown = input->IsKeyDown('E');
        if (interactDown && !m_interactHeld)
        {
            if (!TryCollectPickup())
                TryReachGoal();
        }
        m_interactHeld = interactDown;

        const bool restartDown = input->IsKeyDown('R');
        if (restartDown && !m_restartHeld)
            ResetAdventure();
        m_restartHeld = restartDown;
    }

    void CaptureSceneState()
    {
        if (const Transform* player = m_runtime.Get<Transform>(m_playerEntity))
        {
            m_playerSpawnPosition = player->position;
            m_playerSpawnRotation = player->rotation;
        }
        if (const Transform* pickup = m_runtime.Get<Transform>(m_pickupEntity))
        {
            m_pickupPosition = pickup->position;
            m_pickupBaseRotation = pickup->rotation;
            m_pickupBaseScale = pickup->scale;
        }
        if (const Transform* goal = m_runtime.Get<Transform>(m_goalEntity))
        {
            m_goalPosition = goal->position;
            m_goalBaseRotation = goal->rotation;
            m_goalBaseScale = goal->scale;
        }

        const Transform* camera = m_runtime.Get<Transform>(m_cameraEntity);
        if (!camera)
            return;
        const float offsetX = camera->position.x - m_playerSpawnPosition.x;
        const float offsetY = camera->position.y - m_playerSpawnPosition.y;
        const float offsetZ = camera->position.z - m_playerSpawnPosition.z;
        const float horizontalDistance = std::sqrt(offsetX * offsetX + offsetZ * offsetZ);
        const float distance = std::sqrt(horizontalDistance * horizontalDistance + offsetY * offsetY);
        if (distance > 0.001f)
        {
            m_orbitSpawnDistance = std::clamp(distance, 3.0f, 14.0f);
            m_orbitSpawnPitchDegrees =
                std::clamp(std::atan2(offsetY, horizontalDistance) * kRadiansToDegrees, -10.0f, 75.0f);
            m_orbitSpawnYawDegrees = std::atan2(-offsetX, -offsetZ) * kRadiansToDegrees;
        }
    }

    void SyncRuntimeState()
    {
        if (Transform* player = m_runtime.Get<Transform>(m_playerEntity))
        {
            player->position = {m_state.x, m_state.y, m_state.z};
            player->rotation.y = m_playerYawDegrees;
        }

        if (Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            const float yaw = DirectX::XMConvertToRadians(m_state.orbitYawDegrees);
            const float pitch = DirectX::XMConvertToRadians(m_state.orbitPitchDegrees);
            const float horizontalDistance = std::cos(pitch) * m_state.orbitDistance;
            camera->position = {m_state.x - std::sin(yaw) * horizontalDistance,
                                m_state.y + std::sin(pitch) * m_state.orbitDistance,
                                m_state.z - std::cos(yaw) * horizontalDistance};
            camera->rotation = {m_state.orbitPitchDegrees, m_state.orbitYawDegrees, 0.0f};
        }

        if (Transform* pickupTransform = m_runtime.Get<Transform>(m_pickupEntity))
        {
            pickupTransform->position.y = m_pickupPosition.y + std::sin(m_state.elapsedSeconds * 2.2f) * 0.10f;
            pickupTransform->rotation = m_pickupBaseRotation;
            pickupTransform->rotation.y = std::fmod(m_pickupBaseRotation.y + m_state.elapsedSeconds * 65.0f, 360.0f);
            pickupTransform->scale = m_pickupBaseScale;
        }
        if (MeshRenderer* pickup = m_runtime.Get<MeshRenderer>(m_pickupEntity))
        {
            pickup->visible = !m_state.pickupCollected;
            pickup->emissive = 0.18f;
        }
        if (Transform* goalTransform = m_runtime.Get<Transform>(m_goalEntity))
        {
            const float pulse = 1.0f + std::sin(m_state.elapsedSeconds * 1.8f) * 0.035f;
            goalTransform->position = m_goalPosition;
            goalTransform->rotation = m_goalBaseRotation;
            goalTransform->scale = {m_goalBaseScale.x * pulse, m_goalBaseScale.y * pulse, m_goalBaseScale.z * pulse};
        }
        if (MeshRenderer* goal = m_runtime.Get<MeshRenderer>(m_goalEntity))
        {
            goal->visible = true;
            goal->emissive = m_state.goalReached ? 0.45f : (m_state.pickupCollected ? 0.20f : 0.05f);
        }

        if (SpriteRenderer* hud = m_runtime.Get<SpriteRenderer>(m_hudEntity))
        {
            if (m_state.goalReached)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 2);
                hud->color = {0.3f, 1.0f, 0.55f, 0.95f};
            }
            else if (m_state.pickupCollected)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 0);
                hud->color = {0.6f, 0.85f, 1.0f, 0.95f};
            }
            else
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0);
                const float proximityPulse = DistanceSquared(m_pickupPosition.x, m_pickupPosition.z) <= 2.25f
                                                 ? 0.85f + std::sin(m_state.elapsedSeconds * 6.0f) * 0.15f
                                                 : 1.0f;
                hud->color = {proximityPulse, 1.0f, 1.0f, 0.95f};
            }
        }
        m_runtime.PlaceHud(m_cameraEntity, m_hudEntity, -0.13f, 0.09f, 0.32f, 0.07f, 0.07f);
    }

    [[nodiscard]] float DistanceSquared(float x, float z) const
    {
        const float dx = m_state.x - x;
        const float dz = m_state.z - z;
        return dx * dx + dz * dz;
    }

    void ResetSceneDefaults()
    {
        m_playerSpawnPosition = {0.0f, 0.0f, 0.0f};
        m_playerSpawnRotation = {};
        m_pickupPosition = {3.0f, 0.5f, 2.0f};
        m_goalPosition = {7.0f, 1.0f, 5.0f};
        m_pickupBaseRotation = {};
        m_goalBaseRotation = {};
        m_pickupBaseScale = {0.5f, 0.5f, 0.5f};
        m_goalBaseScale = {1.35f, 1.35f, 1.35f};
        m_orbitSpawnYawDegrees = 0.0f;
        m_orbitSpawnPitchDegrees = 25.0f;
        m_orbitSpawnDistance = 8.0f;
        m_groundY = 0.0f;
    }

    void ResetEntityHandles()
    {
        m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_pickupEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_goalEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    ThirdPersonStarterState m_state;
    DirectX::XMFLOAT3 m_playerSpawnPosition{};
    DirectX::XMFLOAT3 m_playerSpawnRotation{};
    DirectX::XMFLOAT3 m_pickupPosition{3.0f, 0.5f, 2.0f};
    DirectX::XMFLOAT3 m_goalPosition{7.0f, 1.0f, 5.0f};
    DirectX::XMFLOAT3 m_pickupBaseRotation{};
    DirectX::XMFLOAT3 m_goalBaseRotation{};
    DirectX::XMFLOAT3 m_pickupBaseScale{0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT3 m_goalBaseScale{1.35f, 1.35f, 1.35f};
    float m_groundY = 0.0f;
    float m_orbitSpawnYawDegrees = 0.0f;
    float m_orbitSpawnPitchDegrees = 25.0f;
    float m_orbitSpawnDistance = 8.0f;
    float m_playerYawDegrees = 0.0f;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_pickupEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_goalEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_jumpHeld = false;
    bool m_interactHeld = false;
    bool m_restartHeld = false;
    bool m_cameraResetHeld = false;
    bool m_sprintInput = false;
};
