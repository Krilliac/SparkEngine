/**
 * @file ProjectSettingsPanel.h
 * @brief Project-wide settings editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>

namespace SparkEditor
{

    /**
     * @brief Panel for editing engine and project settings via tabbed UI
     *
     * Wraps the engine's EngineSettings singleton, exposing all settings
     * categories (Graphics, Audio, Physics, AI, Gameplay, etc.) as
     * editable tabs with live preview and save/load support.
     */
    class ProjectSettingsPanel : public EditorPanel
    {
      public:
        ProjectSettingsPanel();
        ~ProjectSettingsPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderToolbar();
        void RenderGraphicsTab();
        void RenderAudioTab();
        void RenderPhysicsTab();
        void RenderAITab();
        void RenderGameplayTab();
        void RenderCameraTab();
        void RenderEditorTab();
        void RenderProjectInfoTab();

        int m_activeTab = 0;
        bool m_modified = false;
    };

} // namespace SparkEditor
