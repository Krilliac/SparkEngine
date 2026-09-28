/**
 * @file EditorDocument.cpp
 * @brief Snapshot-based undoable mutations of the edited scene document
 * @author Spark Engine Team
 * @date 2026
 */

#include "EditorDocument.h"

#include "../CommandHistory.h"
#include "Core/Reflection.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SparkEditor
{

    namespace
    {
        /// GameObject-menu entries that are built purely from registered
        /// component types (everything except Empty, primitives, Camera and lights).
        const std::unordered_map<std::string, std::vector<std::string>>& MenuComponentTypes()
        {
            static const std::unordered_map<std::string, std::vector<std::string>> componentMap = {
                {"Sprite", {"SpriteRenderer"}},
                {"Animated Sprite", {"SpriteRenderer", "SpriteAnimator"}},
                {"Tilemap", {"TilemapComponent"}},
                {"Camera 2D", {"Camera2D"}},
                {"Parallax Background", {"ParallaxBackground"}},
                {"Nine-Slice Sprite", {"NineSliceSprite"}},
                {"Trigger Volume", {"TriggerVolumeComponent"}},
                {"Post-Process Volume", {"PostProcessVolumeComponent"}},
                {"Fog Volume", {"FogVolumeComponent"}},
                {"Audio Reverb Zone", {"AudioReverbZoneComponent"}},
                {"Wind Zone", {"WindZoneComponent"}},
                {"Cinematic Trigger", {"CinematicTriggerComponent"}},
                {"Area Boundary", {"AreaBoundaryComponent"}},
                {"Reflection Probe", {"ReflectionProbeComponent"}},
                {"Light Probe", {"LightProbeComponent"}},
                {"Water Plane", {"WaterPlaneComponent"}},
                {"Spawn Point", {"SpawnPointComponent"}},
                {"NavMesh Obstacle", {"NavObstacleComponent"}},
                {"Occluder", {"OccluderComponent"}},
                {"Billboard", {"BillboardComponent"}},
                {"Destructible", {"DestructibleComponent"}},
                {"Dialogue Trigger", {"DialogueTriggerComponent"}},
                {"Physics Joint", {"PhysicsJointComponent"}},
                {"Character Controller", {"CharacterControllerComponent"}},
                {"Vehicle", {"VehicleComponent"}},
                {"Cover Point", {"CoverPointComponent"}},
                {"Tactical Point", {"TacticalPointComponent"}},
                {"Nav Region", {"NavRegionComponent"}},
                {"Nav Link", {"NavLinkComponent"}},
                {"Skybox", {"SkyboxComponent"}},
                {"Trail Renderer", {"TrailRendererComponent"}},
                {"Text 3D", {"Text3DComponent"}},
                {"Foliage Volume", {"FoliageVolumeComponent"}},
                {"Ragdoll", {"RagdollComponent"}},
                {"Soft Body", {"SoftBodyComponent"}},
                {"Constant Force", {"ConstantForceComponent"}},
                {"Force Region", {"ForceRegionComponent"}},
                {"Buoyancy Volume", {"BuoyancyVolumeComponent"}},
                {"Spring Arm", {"SpringArmComponent"}},
            };
            return componentMap;
        }
    } // namespace

    void EditorDocument::SetHooks(RecordedHook onRecorded, RestoredHook onRestored)
    {
        m_onRecorded = std::move(onRecorded);
        m_onRestored = std::move(onRestored);
    }

    void EditorDocument::ReplaceWorld(std::unique_ptr<::World> world)
    {
        m_world = std::move(world);
        m_selectedEntity = entt::null;
    }

    std::string EditorDocument::Capture() const
    {
        return m_world ? Spark::SerializeWorld(*m_world) : std::string{};
    }

    bool EditorDocument::Restore(const std::string& json, ::EntityID selection)
    {
        if (!m_world)
            return false;
        auto restored = std::make_unique<::World>();
        // Undo/redo and play-mode snapshots are SerializeWorld output captured in this process.
        if (!Spark::DeserializeInto(*restored, json, Spark::SceneDeserializeMode::TrustedSnapshot))
            return false;

        // Keep the World object's address: panels and commands hold it.
        m_world->GetRegistry() = std::move(restored->GetRegistry());
        m_selectedEntity = entt::null;
        if (m_onRestored)
            m_onRestored();
        if (selection != entt::null && m_world->GetRegistry().valid(selection))
            m_selectedEntity = selection;
        return true;
    }

    bool EditorDocument::CreateEntity(const std::string& menuName)
    {
        if (!m_world)
            return false;

        const std::string before = Spark::SerializeWorld(*m_world);
        const ::EntityID selectionBefore = m_selectedEntity;

        const ::EntityID entity = m_world->CreateEntity(menuName == "Empty" ? "Entity" : menuName);
        m_world->AddComponent<::Transform>(entity);

        auto failUnsupported = [&]()
        {
            m_world->DestroyEntity(entity);
            return false;
        };

        if (menuName == "Cube" || menuName == "Sphere" || menuName == "Cylinder" || menuName == "Plane")
        {
            auto& mesh = m_world->AddComponent<::MeshRenderer>(entity);
            mesh.meshPath = "__spark_primitive_" + menuName + ".obj";
        }
        else if (menuName == "Camera")
        {
            auto& camera = m_world->AddComponent<::Camera>(entity);
            bool hasMainCamera = false;
            for (const ::EntityID other : m_world->GetEntitiesWith<::Camera>())
            {
                if (other != entity && m_world->GetComponent<::Camera>(other)->isMainCamera)
                {
                    hasMainCamera = true;
                    break;
                }
            }
            camera.isMainCamera = !hasMainCamera;
        }
        else if (menuName == "Directional Light" || menuName == "Point Light" || menuName == "Spot Light")
        {
            auto& light = m_world->AddComponent<::LightComponent>(entity);
            light.type = menuName == "Directional Light" ? ::LightComponent::Type::Directional
                         : menuName == "Spot Light"      ? ::LightComponent::Type::Spot
                                                         : ::LightComponent::Type::Point;
        }
        else if (menuName != "Empty")
        {
            const auto& componentMap = MenuComponentTypes();
            const auto it = componentMap.find(menuName);
            if (it == componentMap.end())
                return failUnsupported();
            auto& factory = Spark::ComponentFactory::Get();
            for (const std::string& type : it->second)
            {
                if (!factory.IsRegistered(type))
                    return failUnsupported();
                factory.AddComponent(type, m_world.get(), static_cast<uint32_t>(entity));
            }
        }

        const std::string after = Spark::SerializeWorld(*m_world);
        const std::string description = "Create " + (menuName == "Empty" ? std::string("Entity") : menuName);
        return CommitSnapshot(before, after, entity, selectionBefore, description);
    }

    bool EditorDocument::DeleteSelected()
    {
        if (!m_world || m_selectedEntity == entt::null || !m_world->GetRegistry().valid(m_selectedEntity))
            return false;
        const std::string before = Spark::SerializeWorld(*m_world);
        const ::EntityID selectionBefore = m_selectedEntity;
        m_world->DestroyEntity(m_selectedEntity);
        m_selectedEntity = entt::null;
        const std::string after = Spark::SerializeWorld(*m_world);
        return CommitSnapshot(before, after, entt::null, selectionBefore, "Delete Entity");
    }

    bool EditorDocument::RecordApplied(const std::string& before, const std::string& description)
    {
        if (!m_world || before.empty())
            return false;
        const std::string after = Spark::SerializeWorld(*m_world);
        if (after == before)
            return false;
        return CommitSnapshot(before, after, m_selectedEntity, m_selectedEntity, description);
    }

    bool EditorDocument::CommitSnapshot(const std::string& before, const std::string& after, ::EntityID redoSelection,
                                        ::EntityID undoSelection, const std::string& description)
    {
        // Execute runs the redo body immediately, so the live World is the
        // restored `after` snapshot from here on: exactly what a later redo reproduces.
        Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [this, after, redoSelection]() { Restore(after, redoSelection); },
            [this, before, undoSelection]() { Restore(before, undoSelection); }, description));
        if (m_onRecorded)
            m_onRecorded(description);
        return true;
    }

} // namespace SparkEditor
