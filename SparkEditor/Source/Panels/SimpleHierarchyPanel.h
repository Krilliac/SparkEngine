/**
 * @file SimpleHierarchyPanel.h
 * @brief Simple hierarchy panel for editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>
#include <unordered_set>

namespace SparkEditor
{

    class SimpleHierarchyPanel : public EditorPanel
    {
      public:
        SimpleHierarchyPanel();
        ~SimpleHierarchyPanel() override;

        // EditorPanel overrides
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Create new object
     * @param objectType Type of object to create
     */
        void CreateObject(const std::string& objectType);

        /**
     * @brief Delete object
     * @param objectName Name of object to delete
     */
        void DeleteObject(const std::string& objectName);

        /**
     * @brief Get selected object
     * @return Selected object name
     */
        const std::string& GetSelectedObject() const;

        /**
     * @brief Set selected object
     * @param objectName Object name to select
     */
        void SetSelectedObject(const std::string& objectName);

        /**
     * @brief Reset hierarchy to default objects for a new scene
     */
        void ResetToDefault();

        /**
     * @brief Get all scene objects
     */
        const std::vector<std::string>& GetSceneObjects() const { return m_sceneObjects; }

        /**
         * @brief Check if an object is visible
         * @param objectName Name of the object
         * @return true if visible (default), false if hidden
         */
        bool IsObjectVisible(const std::string& objectName) const;

        /**
         * @brief Toggle visibility of an object
         * @param objectName Name of the object
         */
        void ToggleObjectVisibility(const std::string& objectName);

      private:
        std::vector<std::string> m_sceneObjects;
        std::string m_selectedObject;
        char m_searchFilter[256] = {0};
        std::unordered_set<std::string> m_hiddenObjects;
    };

} // namespace SparkEditor