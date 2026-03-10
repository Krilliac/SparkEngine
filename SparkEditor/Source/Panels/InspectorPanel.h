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
        void RenderAddComponentMenu();

        /// Helper: check if the inspected object has a specific component type
        bool HasComponent(ComponentType type) const;

        /// Helper: add a component to the inspected object through CommandHistory
        void AddComponent(ComponentType type);

        /// Helper: remove a component from the inspected object through CommandHistory
        void RemoveComponent(ComponentType type);

      private:
        SceneFile* m_scene = nullptr;
        ObjectID m_inspectedObjectID = INVALID_OBJECT_ID;
        std::string m_inspectedObject;
        bool m_showAddComponentMenu = false;
    };

} // namespace SparkEditor
