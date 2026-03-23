/**
 * @file TimeOfDayPanel.h
 * @brief Time-of-day controls for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    /**
     * @brief Panel for controlling the TimeOfDaySystem day/night cycle
     *
     * Provides a time slider, time scale control, sun/ambient preview,
     * and quick-jump presets for common times of day.
     */
    class TimeOfDayPanel : public EditorPanel
    {
      public:
        TimeOfDayPanel();
        ~TimeOfDayPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderTimeControls();
        void RenderPresets();
        void RenderLightingPreview();
        void RenderDayInfo();

        float m_currentHour = 12.0f;
        float m_timeScale = 60.0f;
        bool m_paused = false;
        int m_dayCount = 0;

        // Cached lighting state
        float m_sunDirection[3] = {0.0f, 1.0f, 0.0f};
        float m_sunColor[3] = {1.0f, 1.0f, 1.0f};
        float m_sunIntensity = 1.0f;
        float m_ambientColor[3] = {0.1f, 0.1f, 0.12f};
        float m_ambientIntensity = 0.3f;
    };

} // namespace SparkEditor
