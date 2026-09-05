/**
 * @file TimeOfDayPanel.h
 * @brief Time-of-day controls for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    /**
     * @brief Panel for controlling the engine TimeOfDaySystem day/night cycle
     *
     * Every control writes through to Spark::TimeOfDaySystem and every value shown
     * is read back from it; the panel keeps no private day/night simulation.
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

        /// @brief Set the engine clock to @p hour (hours, [0, 24)).
        void SetHour(float hour);
        /// @brief Set the engine time scale (game seconds per real second).
        void SetTimeScale(float scale);
        /// @brief Pause or resume the engine clock.
        void SetPaused(bool paused);

        /// @brief Current hour reported by the engine TimeOfDaySystem.
        float GetHour() const;
        /// @brief Current time scale reported by the engine TimeOfDaySystem.
        float GetTimeScale() const;
        /// @brief Whether the engine TimeOfDaySystem is paused.
        bool IsPaused() const;

        /**
         * @brief Whether this panel advances the clock itself.
         *
         * True in the standalone editor, where no engine lifecycle ticks the
         * TimeOfDaySystem. False once an EngineContext exists in the process,
         * because the gameplay lifecycle already ticks the same system and a
         * second tick would double the elapsed game time.
         */
        bool IsDrivingClock() const;

      private:
        void RenderTimeControls();
        void RenderPresets();
        void RenderLightingPreview();
        void RenderDayInfo();

        // ImGui slider scratch values; re-synced from the engine system every Update().
        float m_hourSlider = 12.0f;
        float m_timeScaleSlider = 60.0f;
    };

} // namespace SparkEditor
