/**
 * @file TemplateRuntime.h
 * @brief Small runtime bridge for installed-SDK game templates.
 *
 * TemplateRuntimeScene appends a reflected scene to the engine-owned World,
 * exposes non-owning input/graphics access, renders that owned scene, and
 * removes only the entities it created. It is intentionally a value owned by
 * one game module; the engine continues to own every service and subsystem.
 *
 * Modules using this bridge pin entity handles and must return false from
 * IModule::SupportsHotReload(). All methods are game-thread methods; Render()
 * currently runs on the same main thread as the runtime render callback.
 */

#pragma once

#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/WorldBasicRenderer.h"
#include "Input/InputManager.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/LogMacros.h"
#include <Spark/IEngineContext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Spark::Templates
{
    /** Outcome of the most recent TemplateRuntimeScene::Load call. */
    enum class TemplateLoadResult
    {
        Deterministic, ///< No engine context: construction-only test seam, nothing was loaded.
        Loaded,        ///< A candidate scene deserialized, appended, and satisfied the contract.
        Failed         ///< A context was supplied but no candidate scene could be loaded.
    };

    class TemplateRuntimeScene final
    {
      public:
        static constexpr uint32_t InvalidEntity = std::numeric_limits<uint32_t>::max();
        static constexpr float RuntimeSheetSize = 1254.0f;
        static constexpr float RuntimeSheetCellSize = 418.0f;
        static constexpr float RuntimeSheetCellUv = RuntimeSheetCellSize / RuntimeSheetSize;

        TemplateRuntimeScene() = default;
        ~TemplateRuntimeScene() { Unload(); }

        TemplateRuntimeScene(const TemplateRuntimeScene&) = delete;
        TemplateRuntimeScene& operator=(const TemplateRuntimeScene&) = delete;
        TemplateRuntimeScene(TemplateRuntimeScene&&) = delete;
        TemplateRuntimeScene& operator=(TemplateRuntimeScene&&) = delete;

        /**
         * [game thread] Load the first candidate that deserializes and satisfies
         * the caller's scene contract. A null context is the deterministic test
         * seam: it loads nothing and still returns true, so callers that need to
         * distinguish "constructed" from "loaded" must read LastLoadResult() or
         * IsActive() instead of the bool.
         */
        template <typename Contract>
        bool Load(IEngineContext* context, std::string_view moduleName,
                  std::initializer_list<std::filesystem::path> relativeCandidates, Contract&& contract)
        {
            Unload();
            m_context = context;
            m_moduleName.assign(moduleName);
            if (!context)
                return true;

            m_graphics = context->GetGraphics();
            m_input = context->GetInput();
            m_world = context->GetWorld();
            if (!m_world)
            {
                SPARK_LOG_ERROR(LogCategory::Game, "%s requires an ECS world from IEngineContext",
                                m_moduleName.c_str());
                ClearReferences();
                m_lastLoadResult = TemplateLoadResult::Failed;
                return false;
            }

            std::error_code ec;
            m_projectRoot = std::filesystem::current_path(ec);
            if (ec)
            {
                SPARK_LOG_ERROR(LogCategory::Game, "%s could not resolve its project working directory",
                                m_moduleName.c_str());
                ClearReferences();
                m_lastLoadResult = TemplateLoadResult::Failed;
                return false;
            }

            for (const std::filesystem::path& relative : relativeCandidates)
            {
                if (!IsSafeRelativePath(relative))
                    continue;
                const std::filesystem::path candidate = m_projectRoot / relative;
                ec.clear();
                if (!std::filesystem::is_regular_file(candidate, ec) || ec)
                    continue;

                World stagedWorld;
                if (!LoadWorld(stagedWorld, PathUtf8(candidate)))
                    continue;
                if (!AppendStagedWorld(stagedWorld))
                {
                    CleanupOwnedEntities();
                    continue;
                }
                if (!contract(*this))
                {
                    CleanupOwnedEntities();
                    continue;
                }

                if (m_graphics)
                    m_meshCache = std::make_unique<WorldMeshCache>();
                m_runtimeActive = true;
                m_lastLoadResult = TemplateLoadResult::Loaded;
                SPARK_LOG_INFO(LogCategory::Game, "%s loaded scene '%s' with %zu owned entities", m_moduleName.c_str(),
                               PathUtf8(candidate).c_str(), m_ownedEntities.size());
                return true;
            }

            SPARK_LOG_ERROR(LogCategory::Game, "%s could not load a valid runtime scene from '%s'",
                            m_moduleName.c_str(), PathUtf8(m_projectRoot).c_str());
            CleanupOwnedEntities();
            ClearReferences();
            m_lastLoadResult = TemplateLoadResult::Failed;
            return false;
        }

        /** [game thread] Release only module-owned entities and references. */
        void Unload()
        {
            m_meshCache.reset();
            CleanupOwnedEntities();
            ClearReferences();
            m_lastLoadResult = TemplateLoadResult::Deterministic;
        }

        /** [render thread; currently the runtime main thread] */
        void Render(uint32_t cameraEntity)
        {
            if (!m_graphics)
                return;

            m_graphics->BeginFrame();
            if (m_runtimeActive && m_world && m_meshCache)
            {
                const Transform* transform = Get<Transform>(cameraEntity);
                const Camera* camera = Get<Camera>(cameraEntity);
                if (transform && camera)
                {
                    const DirectX::XMFLOAT3 forward = Forward(transform->rotation.y, transform->rotation.x);
                    const DirectX::XMVECTOR eye =
                        DirectX::XMVectorSet(transform->position.x, transform->position.y, transform->position.z, 1.0f);
                    const DirectX::XMVECTOR at =
                        DirectX::XMVectorSet(transform->position.x + forward.x, transform->position.y + forward.y,
                                             transform->position.z + forward.z, 1.0f);
                    const DirectX::XMMATRIX view =
                        DirectX::XMMatrixLookAtLH(eye, at, DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
                    const DirectX::XMMATRIX projection =
                        DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(camera->fov),
                                                          CurrentAspectRatio(), camera->nearPlane, camera->farPlane);
                    RenderWorldBasic(*m_world, *m_graphics, *m_meshCache, view, projection, PathUtf8(m_projectRoot));
                }
            }
            m_graphics->EndFrame();
        }

        /** [game thread] */
        void Resize(int width, int height)
        {
            if (width > 0)
                m_viewportWidth = width;
            if (height > 0)
                m_viewportHeight = height;
        }

        [[nodiscard]] bool IsActive() const { return m_runtimeActive; }
        [[nodiscard]] TemplateLoadResult LastLoadResult() const { return m_lastLoadResult; }
        [[nodiscard]] World* GetWorld() { return m_world; }
        [[nodiscard]] const World* GetWorld() const { return m_world; }
        [[nodiscard]] GraphicsEngine* GetGraphics() { return m_graphics; }
        [[nodiscard]] InputManager* GetInput() { return m_input; }
        [[nodiscard]] const std::filesystem::path& GetProjectRoot() const { return m_projectRoot; }

        [[nodiscard]] uint32_t Find(std::string_view name) const
        {
            if (!m_world)
                return InvalidEntity;
            for (uint32_t rawEntity : m_ownedEntities)
            {
                const EntityID entity = ToEntity(rawEntity);
                if (!m_world->GetRegistry().valid(entity))
                    continue;
                const NameComponent* named = m_world->GetComponent<NameComponent>(entity);
                if (named && named->name == name)
                    return rawEntity;
            }
            return InvalidEntity;
        }

        template <typename T> [[nodiscard]] T* Get(uint32_t rawEntity)
        {
            if (!m_world || rawEntity == InvalidEntity || !m_world->GetRegistry().valid(ToEntity(rawEntity)))
                return nullptr;
            return m_world->GetComponent<T>(ToEntity(rawEntity));
        }

        template <typename T> [[nodiscard]] const T* Get(uint32_t rawEntity) const
        {
            if (!m_world || rawEntity == InvalidEntity || !m_world->GetRegistry().valid(ToEntity(rawEntity)))
                return nullptr;
            return m_world->GetComponent<T>(ToEntity(rawEntity));
        }

        [[nodiscard]] uint32_t CreateSprite(std::string_view name, std::string texturePath,
                                            const DirectX::XMFLOAT4& sourceRect,
                                            const DirectX::XMFLOAT4& color = {1.0f, 1.0f, 1.0f, 1.0f})
        {
            if (!m_world)
                return InvalidEntity;
            const EntityID entity = m_world->CreateEntity(std::string(name));
            const uint32_t rawEntity = ToRawEntity(entity);
            m_ownedEntities.push_back(rawEntity);
            m_world->AddComponent<Transform>(entity);
            SpriteRenderer& sprite = m_world->AddComponent<SpriteRenderer>(entity);
            sprite.texturePath = std::move(texturePath);
            sprite.sourceRect = sourceRect;
            sprite.color = color;
            sprite.textureWidth = static_cast<int>(RuntimeSheetSize);
            sprite.textureHeight = static_cast<int>(RuntimeSheetSize);
            sprite.pixelsPerUnit = RuntimeSheetCellSize;
            sprite.sortingLayer = 100;
            return rawEntity;
        }

        [[nodiscard]] static DirectX::XMFLOAT4 SheetCell(uint32_t column, uint32_t row)
        {
            column = std::min(column, 2u);
            row = std::min(row, 2u);
            const float left = static_cast<float>(column) * RuntimeSheetCellUv;
            const float top = static_cast<float>(row) * RuntimeSheetCellUv;
            return {left, top, left + RuntimeSheetCellUv, top + RuntimeSheetCellUv};
        }

        /** [game thread] Place a sprite in camera-relative 2.5D HUD space. */
        void PlaceHud(uint32_t cameraEntity, uint32_t spriteEntity, float horizontal, float vertical, float depth,
                      float width, float height)
        {
            const Transform* camera = Get<Transform>(cameraEntity);
            Transform* sprite = Get<Transform>(spriteEntity);
            if (!camera || !sprite)
                return;

            const DirectX::XMFLOAT3 forward = Forward(camera->rotation.y, camera->rotation.x);
            const DirectX::XMFLOAT3 right = Right(camera->rotation.y);
            const DirectX::XMFLOAT3 up = Cross(forward, right);
            DirectX::XMFLOAT3 position = AddScaled(camera->position, forward, depth);
            position = AddScaled(position, right, horizontal);
            position = AddScaled(position, up, vertical);
            sprite->position = position;
            sprite->rotation = camera->rotation;
            sprite->scale = {std::max(width, 0.0001f), std::max(height, 0.0001f), 1.0f};
        }

      private:
        [[nodiscard]] static uint32_t ToRawEntity(EntityID entity) { return static_cast<uint32_t>(entity); }
        [[nodiscard]] static EntityID ToEntity(uint32_t rawEntity) { return static_cast<EntityID>(rawEntity); }

        [[nodiscard]] static bool IsSafeRelativePath(const std::filesystem::path& path)
        {
            if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
                return false;
            return std::ranges::none_of(path, [](const std::filesystem::path& part) { return part == ".."; });
        }

        [[nodiscard]] static bool IsReflectedSceneFieldType(FieldType type)
        {
            switch (type)
            {
            case FieldType::Bool:
            case FieldType::Int:
            case FieldType::Float:
            case FieldType::Double:
            case FieldType::String:
            case FieldType::Vector2:
            case FieldType::Vector3:
            case FieldType::Vector4:
            case FieldType::Enum:
                return true;
            default:
                return false;
            }
        }

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

            auto& factory = ComponentFactory::Get();
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
                    const TypeInfo* typeInfo = TypeRegistry::Get().FindTypeByName(type);
                    if (!sourceComponent || !destinationComponent || !typeInfo)
                        continue;
                    for (const FieldInfo& field : typeInfo->fields)
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
                                SPARK_LOG_WARN(LogCategory::Game,
                                               "%s did not copy reflected field %s (field type %d is outside the "
                                               "scene-append whitelist); the appended copy differs from the scene",
                                               m_moduleName.c_str(), skipped.c_str(), static_cast<int>(field.type));
                                skippedFields.push_back(std::move(skipped));
                            }
                            continue;
                        }
                        if (!SetFieldFromString(destinationComponent, field, GetFieldAsString(sourceComponent, field)))
                        {
                            SPARK_LOG_ERROR(LogCategory::Game, "%s could not copy reflected field %s.%s",
                                            m_moduleName.c_str(), type.c_str(), field.fieldName.c_str());
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
                    continue;

                const auto childIt = entityRemap.find(ToRawEntity(sourceEntity));
                const auto parentIt = entityRemap.find(ToRawEntity(sourceTransform->parent));
                if (childIt == entityRemap.end() || parentIt == entityRemap.end() ||
                    !m_world->SetParent(ToEntity(childIt->second), ToEntity(parentIt->second)))
                    return false;
            }
            return true;
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
        }

        void ClearReferences()
        {
            m_runtimeActive = false;
            m_meshCache.reset();
            m_ownedEntities.clear();
            m_projectRoot.clear();
            m_moduleName.clear();
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

        [[nodiscard]] static DirectX::XMFLOAT3 Forward(float yawDegrees, float pitchDegrees)
        {
            const float yaw = DirectX::XMConvertToRadians(yawDegrees);
            const float pitch = DirectX::XMConvertToRadians(pitchDegrees);
            const float cosPitch = std::cos(pitch);
            return {std::sin(yaw) * cosPitch, -std::sin(pitch), std::cos(yaw) * cosPitch};
        }

        [[nodiscard]] static DirectX::XMFLOAT3 Right(float yawDegrees)
        {
            const float yaw = DirectX::XMConvertToRadians(yawDegrees);
            return {std::cos(yaw), 0.0f, -std::sin(yaw)};
        }

        [[nodiscard]] static DirectX::XMFLOAT3 Cross(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
        {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }

        [[nodiscard]] static DirectX::XMFLOAT3 AddScaled(const DirectX::XMFLOAT3& value,
                                                         const DirectX::XMFLOAT3& direction, float scale)
        {
            return {value.x + direction.x * scale, value.y + direction.y * scale, value.z + direction.z * scale};
        }

        [[nodiscard]] static std::string PathUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        IEngineContext* m_context = nullptr; // non-owning; engine lifetime
        GraphicsEngine* m_graphics = nullptr;
        InputManager* m_input = nullptr;
        World* m_world = nullptr;
        std::unique_ptr<WorldMeshCache> m_meshCache;
        std::filesystem::path m_projectRoot;
        std::string m_moduleName;
        std::vector<uint32_t> m_ownedEntities;
        int m_viewportWidth = 1280;
        int m_viewportHeight = 720;
        bool m_runtimeActive = false;
        TemplateLoadResult m_lastLoadResult = TemplateLoadResult::Deterministic;
    };
} // namespace Spark::Templates
