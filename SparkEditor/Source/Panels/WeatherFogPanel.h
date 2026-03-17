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
        struct WeatherPreset
        {
            char name[64] = {};
            int type = 0; // 0=Clear..5=Storm
            float intensity = 0.0f;
            float precipitationRate = 0.0f;
            float windSpeed = 0.0f;
            XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f};
            float fogDensity = 0.0f;
            XMFLOAT4 fogColor = {0.7f, 0.7f, 0.8f, 1.0f};
            float ambientMultiplier = 1.0f;
            float lightningFrequency = 0.0f;
            float wetness = 0.0f;
            float snowCoverage = 0.0f;
        };

        void RenderPresetList();
        void RenderPresetEditor();

        std::vector<WeatherPreset> m_presets;
        int m_selectedPreset = -1;
    };

} // namespace SparkEditor
