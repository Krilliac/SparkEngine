/**
 * @file InspectorPanel.h
 * @brief Inspector panel for property editing in the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <memory>

namespace SparkEditor
{

    /**
 * @brief Inspector panel
 * 
 * Shows properties of the currently selected object(s) and allows editing.
 */
    class InspectorPanel : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        InspectorPanel();

        /**
     * @brief Destructor
     */
        ~InspectorPanel() override = default;

        /**
     * @brief Initialize the inspector panel
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update inspector panel
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render inspector panel
     */
        void Render() override;

        /**
     * @brief Shutdown the inspector panel
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Set object to inspect
     * @param objectId ID of object to inspect
     */
        void SetInspectedObject(const std::string& objectId);

      private:
        void RenderObjectProperties();
        void RenderComponentList();
        void RenderTransformComponent();
        void RenderAddComponentMenu();

      private:
        std::string m_inspectedObject;
        bool m_showAddComponentMenu = false;
    };

} // namespace SparkEditor