/**
 * @file WeatherFogPanel.h
 * @brief Weather and fog editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Graphics/WeatherSystem.h"

namespace SparkEditor
{

    /**
     * @brief Panel for inspecting the engine weather presets and applying them
     *
     * The parameter values shown are the engine's own Spark::GetWeatherPreset()
     * values, not an editor copy. Apply pushes the selected type and intensity into
     * the WeatherSystem registered in the EngineContext; with no system registered
     * the panel says so and the control is disabled.
     */
    class WeatherFogPanel : public EditorPanel
    {
      public:
        WeatherFogPanel();
        ~WeatherFogPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        /// @brief Currently selected weather type.
        Spark::WeatherType GetSelectedType() const { return m_selectedType; }
        /// @brief Select the weather type the editor shows and applies.
        void SetSelectedType(Spark::WeatherType type) { m_selectedType = type; }

        /// @brief Engine preset for the selected type (never an editor-local copy).
        Spark::WeatherState GetSelectedPreset() const { return Spark::GetWeatherPreset(m_selectedType); }

        /// @brief Whether a WeatherSystem is registered and can receive Apply.
        bool IsWeatherSystemConnected() const;

        /// @brief Push the selected type/intensity into the engine; false when unconnected.
        bool ApplySelected();

      private:
        void RenderPresetList();
        void RenderPresetDetails();

        Spark::WeatherType m_selectedType = Spark::WeatherType::Clear;
        float m_intensity = 1.0f;
        float m_transitionSeconds = 3.0f;
    };

} // namespace SparkEditor
