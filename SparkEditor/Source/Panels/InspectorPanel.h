/**
 * @file InspectorPanel.h
 * @brief Inspector panel for property editing in the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <string>
#include <memory>

namespace SparkEditor
{

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

        void RenderAddComponentMenu();

        /// Helper: check if the inspected object has a specific component type
        bool HasComponent(ComponentType type) const;

        /// Helper: add a component to the inspected object through CommandHistory
        void AddComponent(ComponentType type);

        /// Helper: remove a component from the inspected object through CommandHistory
        void RemoveComponent(ComponentType type);

        /// Helper: find a component by type on a given object
        static Component* FindComponent(SceneFile* scene, ObjectID objectID, ComponentType type);

        /// Helper: draw a labeled XYZ drag-float control with colored reset buttons
        static void DrawVec3Control(const char* label, float* values, float resetValue, float speed);

      private:
        SceneFile* m_scene = nullptr;                     ///< Non-owning pointer to the active scene.
        ObjectID m_inspectedObjectID = INVALID_OBJECT_ID; ///< Currently inspected object (by ID).
        std::string m_inspectedObject;                    ///< Legacy string-based object identifier.
        bool m_showAddComponentMenu = false;              ///< Whether the "Add Component" popup is open.
    };

} // namespace SparkEditor
