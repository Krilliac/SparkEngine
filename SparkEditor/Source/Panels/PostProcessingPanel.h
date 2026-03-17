/**
 * @file PostProcessingPanel.h
 * @brief Post-processing settings panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"

namespace SparkEditor
{

    /**
     * @brief Panel for editing post-processing and environment settings
     *
     * Exposes bloom, tonemapping, fog, sky, and wind parameters from
     * the scene's EnvironmentSettings.
     */
    class PostProcessingPanel : public EditorPanel
    {
      public:
        PostProcessingPanel();
        ~PostProcessingPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        void SetScene(SceneFile* scene) { m_scene = scene; }

      private:
        void RenderBloomSettings();
        void RenderTonemappingSettings();
        void RenderFogSettings();
        void RenderSkySettings();
        void RenderWindSettings();

        SceneFile* m_scene = nullptr;
    };

} // namespace SparkEditor
