/**
 * @file SpriteAnimationEditorPanel.h
 * @brief Sprite animation editor for creating and editing frame-based 2D animations
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Editor panel for creating and editing sprite animations
     *
     * Provides:
     * - Timeline view with frame thumbnails
     * - Drag-and-drop frame reordering
     * - Per-frame duration editing
     * - Animation preview with play/pause/step controls
     * - Sprite sheet auto-slice for batch frame creation
     * - Animation clip management (add, remove, rename clips)
     * - Onion skinning for animation visualization
     * - Export animation data as JSON
     */
    class SpriteAnimationEditorPanel : public EditorPanel
    {
      public:
        SpriteAnimationEditorPanel();
        ~SpriteAnimationEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        void SetScene(SceneFile* scene) { m_scene = scene; }
        void SetInspectedObject(ObjectID objectID) { m_inspectedObjectID = objectID; }

      private:
        void RenderToolbar();
        void RenderClipList();
        void RenderTimeline();
        void RenderPreview();
        void RenderFrameProperties();
        void RenderAutoSliceDialog();

        SceneFile* m_scene = nullptr;
        ObjectID m_inspectedObjectID = INVALID_OBJECT_ID;

        // Animation state
        struct AnimFrame
        {
            float sourceRect[4] = {0, 0, 1, 1};
            float duration = 0.1f;
        };

        struct AnimClip
        {
            std::string name = "New Clip";
            std::vector<AnimFrame> frames;
            bool loop = true;
        };

        std::vector<AnimClip> m_clips;
        int m_selectedClipIndex = -1;
        int m_selectedFrameIndex = -1;

        // Preview state
        bool m_isPlaying = false;
        float m_previewTimer = 0.0f;
        int m_previewFrame = 0;
        float m_previewSpeed = 1.0f;
        bool m_onionSkinning = false;
        int m_onionSkinFrames = 2;

        // Timeline state
        float m_timelineZoom = 1.0f;
        float m_timelineScrollX = 0.0f;

        // Auto-slice dialog
        bool m_showAutoSlice = false;
        int m_autoSliceCols = 4;
        int m_autoSliceRows = 1;
        float m_autoSliceFrameDuration = 0.1f;
    };

} // namespace SparkEditor
