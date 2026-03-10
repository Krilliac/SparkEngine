/**
 * @file SpriteEditorPanel.h
 * @brief Sprite editor panel for 2D sprite configuration and preview
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <string>

namespace SparkEditor
{

    /**
     * @brief Editor panel for configuring SpriteRenderer components
     *
     * Provides:
     * - Sprite texture selection and preview
     * - Source rect editing (sub-region picker on the texture atlas)
     * - Pivot point configuration with visual overlay
     * - Pixels-per-unit, sorting layer, and flip settings
     * - Color tint picker
     * - Real-time preview of the sprite in the panel
     */
    class SpriteEditorPanel : public EditorPanel
    {
      public:
        SpriteEditorPanel();
        ~SpriteEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        void SetScene(SceneFile* scene) { m_scene = scene; }
        void SetInspectedObject(ObjectID objectID) { m_inspectedObjectID = objectID; }

      private:
        void RenderTexturePreview();
        void RenderSpriteProperties();
        void RenderSourceRectEditor();
        void RenderPivotEditor();
        void RenderSortingSettings();
        void RenderColorPicker();

        SceneFile* m_scene = nullptr;
        ObjectID m_inspectedObjectID = INVALID_OBJECT_ID;

        // Preview state
        float m_previewZoom = 1.0f;
        float m_previewPanX = 0.0f;
        float m_previewPanY = 0.0f;
        bool m_showGrid = true;
        bool m_showPivot = true;
    };

} // namespace SparkEditor
