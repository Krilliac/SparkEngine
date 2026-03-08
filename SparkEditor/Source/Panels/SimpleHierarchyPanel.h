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

      private:
        std::vector<std::string> m_sceneObjects;
        std::string m_selectedObject;
        char m_searchFilter[256] = {0};
    };

} // namespace SparkEditor