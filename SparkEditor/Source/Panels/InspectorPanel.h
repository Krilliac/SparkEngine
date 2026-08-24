/**
 * @file InspectorPanel.h
 * @brief Inspector panel for property editing in the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include "Core/Reflection.h"
// Engine ECS World/EntityID (Unit C3) — needed here (not just the .cpp)
// because RenderWorldBackedInspector()'s declaration is ::World* / ::EntityID.
// EntityID is a type alias (entt::entity), not forward-declarable, so the
// full header is required (mirrors EditorUI.h, which includes this for the
// same reason).
#include "Engine/ECS/Components.h"
#include <string>
#include <memory>
#include <unordered_map>

namespace SparkEditor
{
    // EditorUI (Unit C3) — non-owning source of the live ECS World + selected
    // entity for the World-backed inspector path. Forward-declared to avoid
    // a header cycle (EditorUI.h forward-declares panels and owns the panel
    // map; it doesn't need InspectorPanel's internals).
    class EditorUI;

    /**
 * @brief Inspector panel
 *
 * Shows properties of the currently selected object(s) and allows editing.
 * Reads/writes actual entity component data from the SceneFile and routes
 * all mutations through CommandHistory for undo/redo support.
 */
    class InspectorPanel : public EditorPanel
    {
      public:
        InspectorPanel();
        ~InspectorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Set the scene that provides entity data
     * @param scene Non-owning pointer to the active scene file
     */
        void SetScene(SceneFile* scene);

        /**
     * @brief Set the selected object by its ObjectID
     * @param objectID ID of the object to inspect
     */
        void SetInspectedObjectByID(ObjectID objectID);

        /**
     * @brief Set object to inspect (legacy string-based API)
     * @param objectId ID of object to inspect
     */
        void SetInspectedObject(const std::string& objectId);

        /**
     * @brief Set the EditorUI to source the live ECS World + selected entity
     * from (Unit C3).
     *
     * Non-owning. When EditorUI has a live World and a valid selected
     * entity (published by HierarchyPanel, Unit C2), Render() takes a
     * World-backed branch that lists/edits the entity's real engine
     * components via reflection (Spark::ComponentFactory /
     * Spark::TypeRegistry) instead of the legacy SceneFile-backed path.
     * @param ui EditorUI instance to source the World + selection from.
     */
        void SetEditorUI(EditorUI* ui) { m_editorUI = ui; }

        /// Helper: draw a labeled XYZ drag-float control with colored reset buttons.
        /// Public so reflection-based component renderers (outside the panel
        /// translation unit) can reuse it for Vector3 field rendering.
        static void DrawVec3Control(const char* label, float* values, float resetValue, float speed);

      private:
        void RenderObjectProperties();
        void RenderComponentList();
        void RenderTransformComponent();
        void RenderMeshRendererComponent();
        void RenderLightComponent();
        void RenderCameraComponent();
        void RenderRigidBodyComponent();
        void RenderColliderComponent();
        void RenderAudioSourceComponent();
        void RenderTerrainComponent();

        // 2D / Sprite components
        void RenderSpriteRendererComponent();
        void RenderSpriteAnimatorComponent();
        void RenderCamera2DComponent();
        void RenderTilemapComponent();
        void RenderNineSliceComponent();
        void RenderParallaxBGComponent();
        void RenderPixelPerfectComponent();
        void RenderRigidBody2DComponent();
        void RenderCollider2DComponent();

        // Animation & Effects
        void RenderParticleEmitterComponent();
        void RenderAnimationControllerComponent();
        void RenderScriptComponent();

        // Gameplay components
        void RenderHealthComponent();
        void RenderAIAgentComponent();
        void RenderSplineComponent();
        void RenderSplineFollowerComponent();
        void RenderDecalComponent();
        void RenderProjectileComponent();
        void RenderInteractionComponent();
        void RenderWeatherComponent();
        void RenderNetworkIdentityComponent();

        // Reflection-driven renderers for volumes, placement, and advanced components
        void RenderTriggerVolumeComponent();
        void RenderPostProcessVolumeComponent();
        void RenderReflectionProbeComponent();
        void RenderLightProbeComponent();
        void RenderNavObstacleComponent();
        void RenderWaterPlaneComponent();
        void RenderFogVolumeComponent();
        void RenderLODGroupComponent();
        void RenderSpawnPointComponent();
        void RenderAudioReverbZoneComponent();
        void RenderWindZoneComponent();
        void RenderBillboardComponent();
        void RenderAudioListenerComponent();
        void RenderCharacterControllerComponent();
        void RenderSkyboxComponent();

        void RenderAddComponentMenu();

        /**
         * @brief World-backed ECS inspector (Unit C3).
         *
         * Lists a collapsing header per real engine component the entity
         * has (via Spark::ComponentFactory::HasComponent), rendering each
         * one's fields generically through RenderReflectedFields (fed by
         * Spark::TypeRegistry), plus an Add-Component button/popup.
         *
         * Undo/redo for these edits is DEFERRED — RenderReflectedFields
         * writes directly to the live component memory, unlike the legacy
         * SceneFile path which routes every mutation through
         * CommandHistory. A follow-up unit should wrap ECS field edits in
         * CommandHistory commands.
         *
         * @param world  The live ECS World (owned by EditorUI).
         * @param entity The currently selected entity (already validated
         *               non-null and registry-valid by the caller).
         */
        void RenderWorldBackedInspector(::World* world, ::EntityID entity);

        /// Add-Component popup for the World-backed path (Unit C3): lists
        /// Spark::ComponentFactory's registered type names and adds the
        /// picked type to the entity via Spark::ComponentFactory::AddComponent.
        void RenderWorldAddComponentMenu(::World* world, ::EntityID entity);

        /// Helper: check if the inspected object has a specific component type
        bool HasComponent(ComponentType type) const;

        /// Helper: add a component to the inspected object through CommandHistory
        void AddComponent(ComponentType type);

        /// Helper: remove a component from the inspected object through CommandHistory
        void RemoveComponent(ComponentType type);

        /// Helper: find a component by type on a given object
        static Component* FindComponent(SceneFile* scene, ObjectID objectID, ComponentType type);

        /**
         * @brief Auto-render ImGui widgets for all fields described by a FieldInfo list.
         *
         * Reads/writes data directly through the void* pointer using field offsets.
         * Supports Bool, Int, Float, String (char[N] buffers), Vector3, Vector4.
         * Fields with hasRange use sliders; others use drag controls.
         *
         * @param data    Pointer to the start of the data struct.
         * @param fields  Vector of field descriptors (from TypeRegistry or inline).
         */
        static bool RenderReflectedFields(void* data, const std::vector<Spark::FieldInfo>& fields);

      private:
        SceneFile* m_scene = nullptr;                     ///< Non-owning pointer to the active scene.
        ObjectID m_inspectedObjectID = INVALID_OBJECT_ID; ///< Currently inspected object (by ID).
        std::string m_inspectedObject;                    ///< Legacy string-based object identifier.
        bool m_showAddComponentMenu = false;              ///< Whether the "Add Component" popup is open.
        uint32_t m_selectionMgrCallbackId = 0;            ///< SelectionManager subscription handle.

        // World-backed ECS inspector (Unit C3). Non-owning; owned by
        // EditorUI. When set and EditorUI reports a live World + valid
        // selected entity, Render() takes the ECS branch instead of the
        // legacy SceneFile path above.
        EditorUI* m_editorUI = nullptr;
        bool m_showWorldAddComponentMenu = false; ///< Whether the World-backed Add-Component popup is open.
        std::unordered_map<std::string, std::string> m_worldEditBaselines;
        ::EntityID m_worldEditEntity = entt::null;
    };

} // namespace SparkEditor
