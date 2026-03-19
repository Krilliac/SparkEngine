/**
 * @file WeatherFogPanel.h
 * @brief Weather and fog preset editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include <vector>
#include <string>

namespace SparkEditor
{

    /**
     * @brief Panel for creating and managing weather presets
     *
     * Allows authoring weather states (Clear, Rain, Snow, Storm, etc.)
     * with precipitation, wind, fog, and lighting parameters.
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

      private:
        /// Editable weather preset with all environmental parameters.
        struct WeatherPreset
        {
            char name[64] = {};                           ///< Preset display name (e.g. "Heavy Rain").
            int type = 0;                                 ///< Weather type: 0=Clear, 1=Cloudy, ..., 5=Storm.
            float intensity = 0.0f;                       ///< Overall weather intensity [0, 1].
            float precipitationRate = 0.0f;               ///< Particles per second for rain/snow.
            float windSpeed = 0.0f;                       ///< Wind speed (m/s).
            XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f};  ///< Normalized wind direction vector.
            float fogDensity = 0.0f;                      ///< Exponential fog density.
            XMFLOAT4 fogColor = {0.7f, 0.7f, 0.8f, 1.0f}; ///< Fog color (RGBA).
            float ambientMultiplier = 1.0f;               ///< Ambient light intensity multiplier.
            float lightningFrequency = 0.0f;              ///< Lightning flashes per minute.
            float wetness = 0.0f;                         ///< Surface wetness [0, 1] (affects specularity).
            float snowCoverage = 0.0f;                    ///< Snow accumulation [0, 1].
        };

        void RenderPresetList();
        void RenderPresetEditor();

        std::vector<WeatherPreset> m_presets;
        int m_selectedPreset = -1;
    };

} // namespace SparkEditor
