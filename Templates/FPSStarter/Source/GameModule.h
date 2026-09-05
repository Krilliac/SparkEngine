#pragma once

#include <Spark/SparkSDK.h>

#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/WorldBasicRenderer.h"
#include "Input/InputManager.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct FPSStarterWeaponState
{
    uint32_t magazine = 8;
    uint32_t reserve = 24;
    float damage = 25.0f;
    float fireInterval = 0.2f;
    float reloadDuration = 1.0f;
};

struct FPSStarterPlayerState
{
    float health = 100.0f;
    uint32_t deaths = 0;
    uint32_t kills = 0;
    bool alive = true;
};

struct FPSStarterTargetState
{
    float health = 100.0f;
    bool destroyed = false;
};

struct FPSStarterCaptureTransition
{
    bool captureChanged = false;
    bool captureMouse = false;
    bool suppressFire = false;
};

class FPSStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "FPSStarter";
        info.version = "0.2.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    // [game thread] The engine owns context and every service returned by it.
    // A null context is the deterministic simulation/test mode.
    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_reloadHeld = false;
        m_resetHeld = false;
        m_captureToggleHeld = false;
        m_leftMouseHeld = false;
        m_mouseWasCaptured = false;
        m_suppressFireUntilMouseReleased = false;
        ResetRound();
        if (!context)
            return true;

        m_graphics = context->GetGraphics();
        m_input = context->GetInput();
        m_world = context->GetWorld();
        if (!m_world)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "FPSStarter requires an ECS world from IEngineContext");
            ClearRuntimeReferences();
            return false;
        }

        if (!LoadRuntimeScene())
        {
            CleanupOwnedEntities();
            ClearRuntimeReferences();
            return false;
        }

        CaptureSpawnState();
        const Transform* cameraTransform = GetTransform(m_cameraEntity);
        m_pitchDegrees = cameraTransform ? ClampPitch(cameraTransform->rotation.x) : 0.0f;
        m_yawDegrees = cameraTransform ? cameraTransform->rotation.y : 0.0f;
        if (m_graphics)
        {
            m_meshCache = std::make_unique<Spark::WorldMeshCache>();
            CreateHud();
        }
        if (m_input && m_graphics)
            m_input->CaptureMouse(true);
        if (m_input)
        {
            m_mouseWasCaptured = m_input->IsMouseCaptured();
            m_leftMouseHeld = m_input->IsMouseButtonDown(0);
            m_suppressFireUntilMouseReleased = m_leftMouseHeld;
        }
        m_runtimeActive = true;
        SyncRuntimeVisualState();
        return true;
    }

    // [game thread] Releases only state owned by this module.
    void OnUnload() override
    {
        if (m_input && m_input->IsMouseCaptured())
            m_input->CaptureMouse(false);
        m_mouseWasCaptured = false;
        m_suppressFireUntilMouseReleased = true;
        m_meshCache.reset();
        CleanupOwnedEntities();
        ClearRuntimeReferences();
    }

    // The transactional loader initializes a replacement image before unloading
    // this one. Both images cannot safely own the same scene IDs and mouse lease.
    bool SupportsHotReload() const override { return false; }

    // [game thread]
    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f)
            return;

        m_fireCooldown = std::max(0.0f, m_fireCooldown - deltaTime);
        if (m_reloadRemaining > 0.0f)
        {
            m_reloadRemaining = std::max(0.0f, m_reloadRemaining - deltaTime);
            if (m_reloadRemaining == 0.0f)
                FinishReload();
        }

        if (!m_player.alive)
        {
            m_respawnRemaining = std::max(0.0f, m_respawnRemaining - deltaTime);
            if (m_respawnRemaining == 0.0f)
                Respawn();
        }

        if (m_runtimeActive && m_input)
            UpdateRuntimeInput(deltaTime);
    }

    // [render thread; currently the runtime's main/game thread]
    void OnRender() override
    {
        if (!m_graphics)
            return;

        m_graphics->BeginFrame();
        if (m_runtimeActive && m_world && m_meshCache)
        {
            const Transform* cameraTransform = GetTransform(m_cameraEntity);
            const Camera* camera = GetComponent<Camera>(m_cameraEntity);
            if (cameraTransform && camera)
            {
                const DirectX::XMFLOAT3 forward = CameraForward(m_yawDegrees, m_pitchDegrees);
                const DirectX::XMVECTOR eye = DirectX::XMVectorSet(
                    cameraTransform->position.x, cameraTransform->position.y, cameraTransform->position.z, 1.0f);
                const DirectX::XMVECTOR at = DirectX::XMVectorSet(cameraTransform->position.x + forward.x,
                                                                  cameraTransform->position.y + forward.y,
                                                                  cameraTransform->position.z + forward.z, 1.0f);
                const DirectX::XMMATRIX view =
                    DirectX::XMMatrixLookAtLH(eye, at, DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
                const DirectX::XMMATRIX projection =
                    DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(camera->fov), CurrentAspectRatio(),
                                                      camera->nearPlane, camera->farPlane);
                Spark::RenderWorldBasic(*m_world, *m_graphics, *m_meshCache, view, projection, ProjectRootUtf8());
            }
        }
        m_graphics->EndFrame();
    }

    // [game thread]
    void OnResize(int width, int height) override
    {
        if (width > 0 && height > 0)
        {
            m_viewportWidth = width;
            m_viewportHeight = height;
        }
    }

    void OnPause() override
    {
        if (m_input && m_input->IsMouseCaptured())
            m_input->CaptureMouse(false);
        m_mouseWasCaptured = false;
        m_suppressFireUntilMouseReleased = true;
    }

    // Returns whether a shot was emitted. A miss still consumes ammunition and
    // starts the fire cooldown; hitTarget controls damage only.
    bool TryFire(bool hitTarget = true)
    {
        if (!m_player.alive || m_target.destroyed || m_fireCooldown > 0.0f || m_reloadRemaining > 0.0f ||
            m_weapon.magazine == 0)
            return false;

        --m_weapon.magazine;
        m_fireCooldown = m_weapon.fireInterval;
        if (!hitTarget)
            return true;

        m_target.health = std::max(0.0f, m_target.health - m_weapon.damage);
        if (m_target.health == 0.0f)
        {
            m_target.destroyed = true;
            ++m_player.kills;
            m_roundWon = true;
        }
        return true;
    }

    bool BeginReload()
    {
        if (!m_player.alive || m_reloadRemaining > 0.0f || m_weapon.magazine >= kMagazineCapacity ||
            m_weapon.reserve == 0)
            return false;
        m_reloadRemaining = m_weapon.reloadDuration;
        return true;
    }

    void DamagePlayer(float amount)
    {
        if (!m_player.alive || amount <= 0.0f)
            return;
        m_player.health = std::max(0.0f, m_player.health - amount);
        if (m_player.health == 0.0f)
        {
            m_player.alive = false;
            ++m_player.deaths;
            m_respawnRemaining = kRespawnDelay;
        }
    }

    void ResetRound()
    {
        m_player = {};
        m_target = {};
        m_weapon = {};
        m_fireCooldown = 0.0f;
        m_reloadRemaining = 0.0f;
        m_respawnRemaining = 0.0f;
        m_roundWon = false;
        RestoreSpawnState();
        SyncRuntimeVisualState();
    }

    [[nodiscard]] const FPSStarterPlayerState& GetPlayerState() const { return m_player; }
    [[nodiscard]] const FPSStarterTargetState& GetTargetState() const { return m_target; }
    [[nodiscard]] const FPSStarterWeaponState& GetWeaponState() const { return m_weapon; }
    [[nodiscard]] float GetReloadRemaining() const { return m_reloadRemaining; }
    [[nodiscard]] float GetRespawnRemaining() const { return m_respawnRemaining; }
    [[nodiscard]] bool HasWonRound() const { return m_roundWon; }
    [[nodiscard]] bool IsTargetUnderCrosshair() const { return IsTargetInCrosshair(); }

    // Deterministic helpers are public so the template's control contract can
    // be tested without constructing a platform window or synthesizing OS input.
    [[nodiscard]] static float ClampPitch(float pitchDegrees) { return std::clamp(pitchDegrees, -89.0f, 89.0f); }

    [[nodiscard]] static FPSStarterCaptureTransition ComputeCaptureTransition(bool mouseCaptured, bool mouseWasCaptured,
                                                                              bool leftMouseDown, bool leftMouseWasDown,
                                                                              bool escapeDown, bool escapeWasDown,
                                                                              bool suppressFire)
    {
        bool desiredCapture = mouseCaptured;
        if (mouseCaptured && !mouseWasCaptured && leftMouseDown)
            suppressFire = true;

        const bool escapePressed = escapeDown && !escapeWasDown;
        const bool leftMousePressed = leftMouseDown && !leftMouseWasDown;
        if (escapePressed)
        {
            desiredCapture = false;
            suppressFire = true;
        }
        else if (!mouseCaptured && leftMousePressed)
        {
            desiredCapture = true;
            suppressFire = true;
        }

        if (!leftMouseDown)
            suppressFire = false;
        return {desiredCapture != mouseCaptured, desiredCapture, suppressFire};
    }

    [[nodiscard]] static DirectX::XMFLOAT3 ComputePlanarMovement(float forwardAxis, float rightAxis, float yawDegrees,
                                                                 float speed, float deltaTime)
    {
        if (deltaTime <= 0.0f || speed <= 0.0f)
            return {};

        const float magnitude = std::sqrt(forwardAxis * forwardAxis + rightAxis * rightAxis);
        if (magnitude > 1.0f)
        {
            forwardAxis /= magnitude;
            rightAxis /= magnitude;
        }

        const float yaw = DegreesToRadians(yawDegrees);
        const float sinYaw = std::sin(yaw);
        const float cosYaw = std::cos(yaw);
        const float distance = speed * deltaTime;
        return {(sinYaw * forwardAxis + cosYaw * rightAxis) * distance, 0.0f,
                (cosYaw * forwardAxis - sinYaw * rightAxis) * distance};
    }

    [[nodiscard]] static bool RayIntersectsAabb(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
                                                const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax)
    {
        float tMin = 0.0f;
        float tMax = std::numeric_limits<float>::max();
        const float origins[3] = {origin.x, origin.y, origin.z};
        const float directions[3] = {direction.x, direction.y, direction.z};
        const float minimums[3] = {boundsMin.x, boundsMin.y, boundsMin.z};
        const float maximums[3] = {boundsMax.x, boundsMax.y, boundsMax.z};

        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(directions[axis]) < 1.0e-6f)
            {
                if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis])
                    return false;
                continue;
            }
            float nearT = (minimums[axis] - origins[axis]) / directions[axis];
            float farT = (maximums[axis] - origins[axis]) / directions[axis];
            if (nearT > farT)
                std::swap(nearT, farT);
            tMin = std::max(tMin, nearT);
            tMax = std::min(tMax, farT);
            if (tMin > tMax)
                return false;
        }
        return tMax >= 0.0f;
    }

  private:
    static constexpr uint32_t kInvalidEntity = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t kMagazineCapacity = 8;
    static constexpr float kRespawnDelay = 2.0f;
    static constexpr float kEyeHeight = 0.7f;
    static constexpr float kMouseSensitivity = 0.12f;
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr int kRuntimeSheetSize = 1254;
    static constexpr float kRuntimeSheetCellSize = 418.0f;
    static constexpr float kRuntimeSheetCellUv = 1.0f / 3.0f;

    [[nodiscard]] static float DegreesToRadians(float degrees) { return degrees * (kPi / 180.0f); }

    [[nodiscard]] static DirectX::XMFLOAT3 CameraForward(float yawDegrees, float pitchDegrees)
    {
        const float yaw = DegreesToRadians(yawDegrees);
        const float pitch = DegreesToRadians(pitchDegrees);
        const float cosPitch = std::cos(pitch);
        return {std::sin(yaw) * cosPitch, std::sin(pitch), std::cos(yaw) * cosPitch};
    }

    [[nodiscard]] static DirectX::XMFLOAT3 CameraRight(float yawDegrees)
    {
        const float yaw = DegreesToRadians(yawDegrees);
        return {std::cos(yaw), 0.0f, -std::sin(yaw)};
    }

    [[nodiscard]] static DirectX::XMFLOAT3 Cross(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    [[nodiscard]] static DirectX::XMFLOAT3 AddScaled(const DirectX::XMFLOAT3& origin,
                                                     const DirectX::XMFLOAT3& direction, float scale)
    {
        return {origin.x + direction.x * scale, origin.y + direction.y * scale, origin.z + direction.z * scale};
    }

    [[nodiscard]] static EntityID ToEntity(uint32_t entity) { return static_cast<EntityID>(entity); }
    [[nodiscard]] static uint32_t ToRawEntity(EntityID entity) { return static_cast<uint32_t>(entity); }

    template <typename T> T* GetComponent(uint32_t entity)
    {
        if (!m_world || entity == kInvalidEntity || !m_world->GetRegistry().valid(ToEntity(entity)))
            return nullptr;
        return m_world->GetComponent<T>(ToEntity(entity));
    }

    [[nodiscard]] Transform* GetTransform(uint32_t entity) { return GetComponent<Transform>(entity); }
    [[nodiscard]] const Transform* GetTransform(uint32_t entity) const
    {
        if (!m_world || entity == kInvalidEntity || !m_world->GetRegistry().valid(ToEntity(entity)))
            return nullptr;
        return m_world->GetComponent<Transform>(ToEntity(entity));
    }

    [[nodiscard]] static bool IsReflectedSceneFieldType(Spark::FieldType type)
    {
        switch (type)
        {
        case Spark::FieldType::Bool:
        case Spark::FieldType::Int:
        case Spark::FieldType::Float:
        case Spark::FieldType::Double:
        case Spark::FieldType::String:
        case Spark::FieldType::Vector2:
        case Spark::FieldType::Vector3:
        case Spark::FieldType::Vector4:
        case Spark::FieldType::Enum:
            return true;
        default:
            return false;
        }
    }

    /**
     * Append a successfully deserialized staging world to the engine-owned
     * world with fresh entity identifiers. ReflectedSceneSerializer preserves
     * serialized IDs and therefore rejects collisions in a non-empty World;
     * staging first keeps host entities untouched and lets this module remap
     * every scene entity and hierarchy edge explicitly.
     */
    bool AppendStagedWorld(World& stagedWorld)
    {
        auto& sourceRegistry = stagedWorld.GetRegistry();
        auto& sourceStorage = sourceRegistry.storage<entt::entity>();
        std::vector<EntityID> sourceEntities;
        sourceEntities.reserve(sourceStorage.size());
        for (auto&& [entity] : sourceStorage.each())
            sourceEntities.push_back(entity);

        std::unordered_map<uint32_t, uint32_t> entityRemap;
        entityRemap.reserve(sourceEntities.size());
        for (EntityID sourceEntity : sourceEntities)
        {
            const NameComponent* named = stagedWorld.GetComponent<NameComponent>(sourceEntity);
            const EntityID destinationEntity = m_world->CreateEntity(named ? named->name : "");
            const uint32_t destinationRaw = ToRawEntity(destinationEntity);
            entityRemap.emplace(ToRawEntity(sourceEntity), destinationRaw);
            m_ownedEntities.push_back(destinationRaw);
        }

        auto& factory = Spark::ComponentFactory::Get();
        const std::vector<std::string> componentTypes = factory.GetRegisteredNames();
        std::vector<std::string> skippedFields;
        for (EntityID sourceEntity : sourceEntities)
        {
            const auto destinationIt = entityRemap.find(ToRawEntity(sourceEntity));
            if (destinationIt == entityRemap.end())
                return false;
            const uint32_t sourceRaw = ToRawEntity(sourceEntity);
            const uint32_t destinationRaw = destinationIt->second;

            for (const std::string& type : componentTypes)
            {
                if (type == "NameComponent" || !factory.HasComponent(type, &stagedWorld, sourceRaw))
                    continue;
                if (!factory.AddComponent(type, m_world, destinationRaw))
                    return false;

                const void* sourceComponent = factory.GetComponentRaw(type, &stagedWorld, sourceRaw);
                void* destinationComponent = factory.GetComponentRaw(type, m_world, destinationRaw);
                const Spark::TypeInfo* typeInfo = Spark::TypeRegistry::Get().FindTypeByName(type);
                if (!sourceComponent || !destinationComponent || !typeInfo)
                    continue;

                for (const Spark::FieldInfo& field : typeInfo->fields)
                {
                    if (!field.serialized)
                        continue;
                    if (!IsReflectedSceneFieldType(field.type))
                    {
                        // A silently partial copy is indistinguishable from a faithful one, so
                        // say which field was dropped once per (component, field) per load.
                        std::string skipped = type + "." + field.fieldName;
                        if (std::ranges::find(skippedFields, skipped) == skippedFields.end())
                        {
                            SPARK_LOG_WARN(Spark::LogCategory::Game,
                                           "FPSStarter did not copy reflected field %s (field type %d is outside the "
                                           "scene-append whitelist); the appended copy differs from the scene",
                                           skipped.c_str(), static_cast<int>(field.type));
                            skippedFields.push_back(std::move(skipped));
                        }
                        continue;
                    }
                    if (!Spark::SetFieldFromString(destinationComponent, field,
                                                   Spark::GetFieldAsString(sourceComponent, field)))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Game, "FPSStarter could not copy reflected field %s.%s",
                                        type.c_str(), field.fieldName.c_str());
                        return false;
                    }
                }
            }
        }

        for (EntityID sourceEntity : sourceEntities)
        {
            const Transform* sourceTransform = stagedWorld.GetComponent<Transform>(sourceEntity);
            if (!sourceTransform || sourceTransform->parent == entt::null ||
                !sourceRegistry.valid(sourceTransform->parent))
            {
                continue;
            }

            const auto childIt = entityRemap.find(ToRawEntity(sourceEntity));
            const auto parentIt = entityRemap.find(ToRawEntity(sourceTransform->parent));
            if (childIt == entityRemap.end() || parentIt == entityRemap.end() ||
                !m_world->SetParent(ToEntity(childIt->second), ToEntity(parentIt->second)))
            {
                return false;
            }
        }
        return true;
    }

    bool LoadRuntimeScene()
    {
        const std::filesystem::path root = std::filesystem::current_path();
        const std::filesystem::path candidates[] = {root / "Startup.sparkscene", root / "Scenes" / "Arena.sparkscene"};

        for (const auto& candidate : candidates)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(candidate, ec) || ec)
                continue;

            World stagedWorld;
            if (!Spark::LoadWorld(stagedWorld, PathUtf8(candidate)))
                continue;
            if (!AppendStagedWorld(stagedWorld))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "FPSStarter could not append staged scene '%s'",
                                PathUtf8(candidate).c_str());
                CleanupOwnedEntities();
                continue;
            }
            if (!ResolveRequiredEntities())
            {
                CleanupOwnedEntities();
                continue;
            }

            m_projectRoot = root;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "FPSStarter loaded scene '%s' with %zu owned entities",
                           PathUtf8(candidate).c_str(), m_ownedEntities.size());
            return true;
        }

        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "FPSStarter could not load a valid arena contract from Startup.sparkscene or "
                        "Scenes/Arena.sparkscene in '%s'",
                        PathUtf8(root).c_str());
        return false;
    }

    bool ResolveRequiredEntities()
    {
        m_cameraEntity = FindNamedEntity("Main Camera");
        m_playerEntity = FindNamedEntity("Player");
        m_targetEntity = FindNamedEntity("Damageable Target");
        const bool valid = GetTransform(m_cameraEntity) && GetComponent<Camera>(m_cameraEntity) &&
                           GetTransform(m_playerEntity) && GetComponent<CharacterControllerComponent>(m_playerEntity) &&
                           GetTransform(m_targetEntity) && GetComponent<MeshRenderer>(m_targetEntity);
        if (!valid)
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "FPSStarter scene requires Main Camera, Player, and Damageable Target entities");
        return valid;
    }

    [[nodiscard]] uint32_t FindNamedEntity(const std::string& name)
    {
        if (!m_world)
            return kInvalidEntity;
        for (uint32_t rawEntity : m_ownedEntities)
        {
            const EntityID entity = ToEntity(rawEntity);
            if (!m_world->GetRegistry().valid(entity))
                continue;
            const NameComponent* named = m_world->GetComponent<NameComponent>(entity);
            if (named && named->name == name)
                return rawEntity;
        }
        return kInvalidEntity;
    }

    uint32_t CreateHudSprite(const char* name, const DirectX::XMFLOAT4& sourceRect, const DirectX::XMFLOAT4& color,
                             bool useSheet)
    {
        const EntityID entity = m_world->CreateEntity(name);
        m_ownedEntities.push_back(ToRawEntity(entity));
        m_world->AddComponent<Transform>(entity);
        SpriteRenderer& sprite = m_world->AddComponent<SpriteRenderer>(entity);
        sprite.texturePath = useSheet ? "Assets/fps_starter_runtime_sheet.png" : "";
        sprite.sourceRect = sourceRect;
        sprite.color = color;
        sprite.textureWidth = useSheet ? kRuntimeSheetSize : 100;
        sprite.textureHeight = useSheet ? kRuntimeSheetSize : 100;
        sprite.pixelsPerUnit = useSheet ? kRuntimeSheetCellSize : 100.0f;
        sprite.sortingLayer = 100;
        return ToRawEntity(entity);
    }

    void CreateHud()
    {
        // These cells mirror Assets/runtime_sheet.json's locked version-1 3x3 grid.
        m_hudCrosshair =
            CreateHudSprite("FPS HUD Crosshair", {2.0f * kRuntimeSheetCellUv, 0.0f, 1.0f, kRuntimeSheetCellUv},
                            {1.0f, 1.0f, 1.0f, 0.95f}, true);
        m_hudWeapon = CreateHudSprite("FPS HUD Weapon", {0.0f, 0.0f, kRuntimeSheetCellUv, kRuntimeSheetCellUv},
                                      {1.0f, 1.0f, 1.0f, 0.9f}, true);
        m_hudReload = CreateHudSprite(
            "FPS HUD Reload",
            {kRuntimeSheetCellUv, kRuntimeSheetCellUv, 2.0f * kRuntimeSheetCellUv, 2.0f * kRuntimeSheetCellUv},
            {1.0f, 1.0f, 1.0f, 0.95f}, true);
        m_hudAmmoBackground =
            CreateHudSprite("FPS HUD Ammo Background", {0.0f, 0.0f, 1.0f, 1.0f}, {0.02f, 0.04f, 0.07f, 0.82f}, false);
        m_hudAmmoFill =
            CreateHudSprite("FPS HUD Ammo Fill", {0.0f, 0.0f, 1.0f, 1.0f}, {0.1f, 0.8f, 1.0f, 0.95f}, false);
        m_hudTargetBackground =
            CreateHudSprite("FPS HUD Target Background", {0.0f, 0.0f, 1.0f, 1.0f}, {0.08f, 0.02f, 0.02f, 0.82f}, false);
        m_hudTargetFill =
            CreateHudSprite("FPS HUD Target Fill", {0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 0.25f, 0.08f, 0.95f}, false);
    }

    void UpdateRuntimeInput(float deltaTime)
    {
        const bool leftMouseDown = m_input->IsMouseButtonDown(0);
        bool mouseCaptured = m_input->IsMouseCaptured();

        const bool resetDown = m_input->IsKeyDown(VK_RETURN);
        if (resetDown && !m_resetHeld)
            ResetRound();
        m_resetHeld = resetDown;

        const bool captureToggleDown = m_input->IsKeyDown(VK_ESCAPE);
        const FPSStarterCaptureTransition capture =
            ComputeCaptureTransition(mouseCaptured, m_mouseWasCaptured, leftMouseDown, m_leftMouseHeld,
                                     captureToggleDown, m_captureToggleHeld, m_suppressFireUntilMouseReleased);
        if (capture.captureChanged)
            m_input->CaptureMouse(capture.captureMouse);
        mouseCaptured = m_input->IsMouseCaptured();
        m_suppressFireUntilMouseReleased = capture.suppressFire;
        m_captureToggleHeld = captureToggleDown;

        const bool reloadDown = m_input->IsKeyDown('R');
        if (reloadDown && !m_reloadHeld)
            BeginReload();
        m_reloadHeld = reloadDown;

        if (m_player.alive)
        {
            if (mouseCaptured)
            {
                const MousePoint mouse = m_input->GetMouseDelta();
                m_yawDegrees += static_cast<float>(mouse.x) * kMouseSensitivity;
                m_pitchDegrees = ClampPitch(m_pitchDegrees - static_cast<float>(mouse.y) * kMouseSensitivity);
            }

            float forwardAxis = 0.0f;
            float rightAxis = 0.0f;
            if (m_input->IsKeyDown('W'))
                forwardAxis += 1.0f;
            if (m_input->IsKeyDown('S'))
                forwardAxis -= 1.0f;
            if (m_input->IsKeyDown('D'))
                rightAxis += 1.0f;
            if (m_input->IsKeyDown('A'))
                rightAxis -= 1.0f;

            if (Transform* player = GetTransform(m_playerEntity))
            {
                const CharacterControllerComponent* controller =
                    GetComponent<CharacterControllerComponent>(m_playerEntity);
                const float speed = controller ? controller->moveSpeed : 5.0f;
                const DirectX::XMFLOAT3 movement =
                    ComputePlanarMovement(forwardAxis, rightAxis, m_yawDegrees, speed, deltaTime);
                player->position.x = std::clamp(player->position.x + movement.x, -24.0f, 24.0f);
                player->position.z = std::clamp(player->position.z + movement.z, -24.0f, 24.0f);
                player->rotation.y = m_yawDegrees;
            }
        }

        UpdateCameraAndHud();
        if (mouseCaptured && leftMouseDown && !m_suppressFireUntilMouseReleased)
            TryFire(IsTargetInCrosshair());
        SyncRuntimeVisualState();
        m_leftMouseHeld = leftMouseDown;
        m_mouseWasCaptured = mouseCaptured;
    }

    void UpdateCameraAndHud()
    {
        Transform* player = GetTransform(m_playerEntity);
        Transform* camera = GetTransform(m_cameraEntity);
        if (!player || !camera)
            return;

        camera->position = {player->position.x, player->position.y + kEyeHeight, player->position.z};
        camera->rotation = {m_pitchDegrees, m_yawDegrees, 0.0f};

        const DirectX::XMFLOAT3 forward = CameraForward(m_yawDegrees, m_pitchDegrees);
        const DirectX::XMFLOAT3 right = CameraRight(m_yawDegrees);
        const DirectX::XMFLOAT3 up = Cross(forward, right);
        PlaceHud(m_hudCrosshair, camera->position, forward, right, up, 0.25f, 0.0f, 0.0f, 0.035f, 0.035f);
        PlaceHud(m_hudWeapon, camera->position, forward, right, up, 0.42f, 0.12f, -0.105f, 0.20f, 0.20f);
        PlaceHud(m_hudReload, camera->position, forward, right, up, 0.30f, 0.0f, -0.075f, 0.055f, 0.055f);
        PlaceHud(m_hudAmmoBackground, camera->position, forward, right, up, 0.31f, 0.09f, -0.09f, 0.13f, 0.012f);
        PlaceHud(m_hudTargetBackground, camera->position, forward, right, up, 0.31f, 0.0f, 0.10f, 0.18f, 0.012f);

        const float ammoRatio = static_cast<float>(m_weapon.magazine) / static_cast<float>(kMagazineCapacity);
        const float targetRatio = std::clamp(m_target.health / 100.0f, 0.0f, 1.0f);
        PlaceHud(m_hudAmmoFill, camera->position, forward, right, up, 0.305f, 0.09f - 0.062f * (1.0f - ammoRatio),
                 -0.09f, 0.124f * ammoRatio, 0.007f);
        PlaceHud(m_hudTargetFill, camera->position, forward, right, up, 0.305f, -0.087f * (1.0f - targetRatio), 0.10f,
                 0.174f * targetRatio, 0.007f);
    }

    void PlaceHud(uint32_t entity, const DirectX::XMFLOAT3& cameraPosition, const DirectX::XMFLOAT3& forward,
                  const DirectX::XMFLOAT3& right, const DirectX::XMFLOAT3& up, float depth, float horizontal,
                  float vertical, float width, float height)
    {
        Transform* transform = GetTransform(entity);
        if (!transform)
            return;
        DirectX::XMFLOAT3 position = AddScaled(cameraPosition, forward, depth);
        position = AddScaled(position, right, horizontal);
        position = AddScaled(position, up, vertical);
        transform->position = position;
        transform->rotation = {m_pitchDegrees, m_yawDegrees, 0.0f};
        transform->scale = {std::max(width, 0.0001f), std::max(height, 0.0001f), 1.0f};
    }

    [[nodiscard]] bool IsTargetInCrosshair() const
    {
        const Transform* camera = GetTransform(m_cameraEntity);
        const Transform* target = GetTransform(m_targetEntity);
        if (!camera || !target || m_target.destroyed)
            return false;
        const float halfWidth = std::max(0.05f, std::abs(target->scale.x) * 0.5f);
        const float halfDepth = std::max(0.05f, std::abs(target->scale.z) * 0.5f);
        const float height = std::max(0.05f, std::abs(target->scale.y));
        // training_target.obj is authored with its pivot at the base, so its
        // vertical hit bounds must grow upward from the grounded transform.
        const DirectX::XMFLOAT3 boundsMin = {target->position.x - halfWidth, target->position.y,
                                             target->position.z - halfDepth};
        const DirectX::XMFLOAT3 boundsMax = {target->position.x + halfWidth, target->position.y + height,
                                             target->position.z + halfDepth};
        return RayIntersectsAabb(camera->position, CameraForward(m_yawDegrees, m_pitchDegrees), boundsMin, boundsMax);
    }

    void SyncRuntimeVisualState()
    {
        if (!m_runtimeActive)
            return;
        if (MeshRenderer* targetRenderer = GetComponent<MeshRenderer>(m_targetEntity))
            targetRenderer->visible = !m_target.destroyed;
        if (SpriteRenderer* reload = GetComponent<SpriteRenderer>(m_hudReload))
            reload->visible = m_reloadRemaining > 0.0f;
        if (SpriteRenderer* ammo = GetComponent<SpriteRenderer>(m_hudAmmoFill))
            ammo->visible = m_weapon.magazine > 0;
        if (SpriteRenderer* target = GetComponent<SpriteRenderer>(m_hudTargetFill))
            target->visible = !m_target.destroyed && m_target.health > 0.0f;
        UpdateCameraAndHud();
    }

    void CaptureSpawnState()
    {
        const Transform* player = GetTransform(m_playerEntity);
        const Transform* target = GetTransform(m_targetEntity);
        if (!player || !target)
            return;
        m_playerSpawnPosition = player->position;
        m_playerSpawnRotation = player->rotation;
        m_targetSpawnPosition = target->position;
        m_targetSpawnRotation = target->rotation;
        m_hasSpawnState = true;
    }

    void RestoreSpawnState()
    {
        if (!m_runtimeActive || !m_hasSpawnState)
            return;
        if (Transform* player = GetTransform(m_playerEntity))
        {
            player->position = m_playerSpawnPosition;
            player->rotation = m_playerSpawnRotation;
            m_yawDegrees = player->rotation.y;
            m_pitchDegrees = ClampPitch(player->rotation.x);
        }
        if (Transform* target = GetTransform(m_targetEntity))
        {
            target->position = m_targetSpawnPosition;
            target->rotation = m_targetSpawnRotation;
        }
    }

    void FinishReload()
    {
        const uint32_t needed = kMagazineCapacity - m_weapon.magazine;
        const uint32_t transferred = std::min(needed, m_weapon.reserve);
        m_weapon.magazine += transferred;
        m_weapon.reserve -= transferred;
    }

    void Respawn()
    {
        m_player.health = 100.0f;
        m_player.alive = true;
        m_weapon.magazine = kMagazineCapacity;
        if (m_hasSpawnState)
            if (Transform* player = GetTransform(m_playerEntity))
                player->position = m_playerSpawnPosition;
    }

    void CleanupOwnedEntities()
    {
        if (m_world)
        {
            for (auto it = m_ownedEntities.rbegin(); it != m_ownedEntities.rend(); ++it)
            {
                const EntityID entity = ToEntity(*it);
                if (m_world->GetRegistry().valid(entity))
                    m_world->DestroyEntity(entity);
            }
        }
        m_ownedEntities.clear();
        ResetEntityHandles();
    }

    void ResetEntityHandles()
    {
        m_cameraEntity = m_playerEntity = m_targetEntity = kInvalidEntity;
        m_hudCrosshair = m_hudWeapon = m_hudReload = kInvalidEntity;
        m_hudAmmoBackground = m_hudAmmoFill = kInvalidEntity;
        m_hudTargetBackground = m_hudTargetFill = kInvalidEntity;
    }

    void ClearRuntimeReferences()
    {
        m_runtimeActive = false;
        m_meshCache.reset();
        ResetEntityHandles();
        m_ownedEntities.clear();
        m_projectRoot.clear();
        m_hasSpawnState = false;
        m_reloadHeld = false;
        m_resetHeld = false;
        m_captureToggleHeld = false;
        m_leftMouseHeld = false;
        m_mouseWasCaptured = false;
        m_suppressFireUntilMouseReleased = false;
        m_graphics = nullptr;
        m_input = nullptr;
        m_world = nullptr;
        m_context = nullptr;
    }

    [[nodiscard]] float CurrentAspectRatio() const
    {
        int width = m_viewportWidth;
        int height = m_viewportHeight;
        if (m_graphics)
        {
            if (m_graphics->GetWindowWidth() > 0)
                width = static_cast<int>(m_graphics->GetWindowWidth());
            if (m_graphics->GetWindowHeight() > 0)
                height = static_cast<int>(m_graphics->GetWindowHeight());
        }
        return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f;
    }

    [[nodiscard]] static std::string PathUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    [[nodiscard]] std::string ProjectRootUtf8() const { return PathUtf8(m_projectRoot); }

    Spark::IEngineContext* m_context = nullptr; // non-owning
    GraphicsEngine* m_graphics = nullptr;       // non-owning; owned by engine context
    InputManager* m_input = nullptr;            // non-owning; owned by engine context
    World* m_world = nullptr;                   // non-owning; owned by engine context
    std::unique_ptr<Spark::WorldMeshCache> m_meshCache;
    std::filesystem::path m_projectRoot;
    std::vector<uint32_t> m_ownedEntities;
    DirectX::XMFLOAT3 m_playerSpawnPosition{};
    DirectX::XMFLOAT3 m_playerSpawnRotation{};
    DirectX::XMFLOAT3 m_targetSpawnPosition{};
    DirectX::XMFLOAT3 m_targetSpawnRotation{};

    FPSStarterPlayerState m_player;
    FPSStarterTargetState m_target;
    FPSStarterWeaponState m_weapon;
    float m_fireCooldown = 0.0f;
    float m_reloadRemaining = 0.0f;
    float m_respawnRemaining = 0.0f;
    float m_yawDegrees = 0.0f;
    float m_pitchDegrees = 0.0f;
    int m_viewportWidth = 1280;
    int m_viewportHeight = 720;
    bool m_roundWon = false;
    bool m_runtimeActive = false;
    bool m_reloadHeld = false;
    bool m_resetHeld = false;
    bool m_captureToggleHeld = false;
    bool m_leftMouseHeld = false;
    bool m_mouseWasCaptured = false;
    bool m_suppressFireUntilMouseReleased = false;
    bool m_hasSpawnState = false;

    uint32_t m_cameraEntity = kInvalidEntity;
    uint32_t m_playerEntity = kInvalidEntity;
    uint32_t m_targetEntity = kInvalidEntity;
    uint32_t m_hudCrosshair = kInvalidEntity;
    uint32_t m_hudWeapon = kInvalidEntity;
    uint32_t m_hudReload = kInvalidEntity;
    uint32_t m_hudAmmoBackground = kInvalidEntity;
    uint32_t m_hudAmmoFill = kInvalidEntity;
    uint32_t m_hudTargetBackground = kInvalidEntity;
    uint32_t m_hudTargetFill = kInvalidEntity;
};
