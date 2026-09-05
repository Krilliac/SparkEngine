/**
 * @file PrefabEditorPanel.h
 * @brief Panel for editing prefab templates
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a UI for browsing, creating, editing, and managing prefab assets.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Prefabs/PrefabManager.h"
#include <string>

namespace SparkEditor
{

    /**
     * @brief Panel for browsing and editing prefab templates
     *
     * Displays a list of loaded prefabs on the left, and a component
     * inspector for the selected prefab on the right. Supports creating,
     * deleting, saving, and instantiating prefabs.
     */
    class PrefabEditorPanel : public EditorPanel
    {
      public:
        /**
         * @brief Construct the prefab editor panel
         * @param prefabManager Pointer to the prefab manager (non-owning)
         */
        explicit PrefabEditorPanel(PrefabManager* prefabManager);

        ~PrefabEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "PrefabEditorPanel"; }

      private:
        void RenderToolbar();
        void RenderPrefabList();
        void RenderPrefabInspector();
        void RenderComponentEditor(SerializedComponent& component);
        void RenderPropertyEditor(const std::string& name, PrefabPropertyValue& value);

        PrefabManager* m_prefabManager = nullptr;
        std::string m_selectedPrefab;
        /// Component type queued by the context menu for removal after the render loop.
        std::string m_componentToRemove;
        std::string m_searchFilter;
        char m_newPrefabName[128] = {};
        bool m_showCreateDialog = false;
    };

} // namespace SparkEditor
