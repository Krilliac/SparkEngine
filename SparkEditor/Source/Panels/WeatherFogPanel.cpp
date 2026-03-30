/**
 * @file WeatherFogPanel.cpp
 * @brief Implementation of the weather and fog preset editor panel
 */

#include "WeatherFogPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstring>
#include <cstdio>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    WeatherFogPanel::WeatherFogPanel() : EditorPanel("Weather & Fog", "weather_fog_panel") {}

    bool WeatherFogPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Weather & Fog panel");

        // Create default presets matching engine WeatherSystem
        const char* defaultNames[] = {"Clear", "Cloudy", "Rain", "Snow", "Fog", "Storm"};
        for (int i = 0; i < 6; ++i)
        {
            WeatherPreset preset;
            strncpy(preset.name, defaultNames[i], sizeof(preset.name) - 1);
            preset.type = i;
            m_presets.push_back(preset);
        }

        // Set realistic defaults for each
        m_presets[2].intensity = 0.7f;
        m_presets[2].precipitationRate = 500.0f;
        m_presets[2].windSpeed = 5.0f;
        m_presets[2].fogDensity = 0.01f;
        m_presets[2].wetness = 0.8f;

        m_presets[3].intensity = 0.5f;
        m_presets[3].precipitationRate = 200.0f;
        m_presets[3].windSpeed = 2.0f;
        m_presets[3].snowCoverage = 0.6f;

        m_presets[4].fogDensity = 0.05f;
        m_presets[4].intensity = 0.8f;
        m_presets[4].ambientMultiplier = 0.7f;

        m_presets[5].intensity = 1.0f;
        m_presets[5].precipitationRate = 1000.0f;
        m_presets[5].windSpeed = 15.0f;
        m_presets[5].fogDensity = 0.02f;
        m_presets[5].lightningFrequency = 5.0f;
        m_presets[5].wetness = 1.0f;

        return true;
    }

    void WeatherFogPanel::Update(float /*deltaTime*/) {}

    void WeatherFogPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            float listWidth = ImGui::GetContentRegionAvail().x * 0.3f;
            ImGui::BeginChild("PresetList", ImVec2(listWidth, 0), true);
            RenderPresetList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("PresetEditor", ImVec2(0, 0), true);
            RenderPresetEditor();
            ImGui::EndChild();
        }
        EndPanel();
    }

    void WeatherFogPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Weather & Fog panel");
    }

    void WeatherFogPanel::RenderPresetList()
    {
        if (ImGui::Button(ICON_FA_PLUS " New Preset"))
        {
            WeatherPreset preset;
            snprintf(preset.name, sizeof(preset.name), "Custom %d", static_cast<int>(m_presets.size()));
            m_presets.push_back(preset);
            m_selectedPreset = static_cast<int>(m_presets.size()) - 1;
        }

        ImGui::Separator();

        const char* typeIcons[] = {ICON_FA_SUN,       ICON_FA_CLOUD, ICON_FA_CLOUD_RAIN,
                                   ICON_FA_SNOWFLAKE, ICON_FA_SMOG,  ICON_FA_BOLT};

        for (int i = 0; i < static_cast<int>(m_presets.size()); ++i)
        {
            auto& preset = m_presets[i];
            int typeIdx = (std::min)(preset.type, 5);

            char label[128];
            snprintf(label, sizeof(label), "%s %s", typeIcons[typeIdx], preset.name);

            if (ImGui::Selectable(label, m_selectedPreset == i))
                m_selectedPreset = i;
        }
    }

    void WeatherFogPanel::RenderPresetEditor()
    {
        if (m_selectedPreset < 0 || m_selectedPreset >= static_cast<int>(m_presets.size()))
        {
            ImGui::TextDisabled("Select a weather preset to edit");
            return;
        }

        auto& preset = m_presets[m_selectedPreset];

        ImGui::InputText("Name", preset.name, sizeof(preset.name));

        const char* weatherTypes[] = {"Clear", "Cloudy", "Rain", "Snow", "Fog", "Storm"};
        ImGui::Combo("Type", &preset.type, weatherTypes, IM_ARRAYSIZE(weatherTypes));

        ImGui::SliderFloat("Intensity", &preset.intensity, 0.0f, 1.0f);

        if (ImGui::CollapsingHeader("Precipitation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);
            ImGui::DragFloat("Rate", &preset.precipitationRate, 10.0f, 0.0f, 5000.0f, "%.0f/s");
            ImGui::Unindent(4);
        }

        if (ImGui::CollapsingHeader("Wind", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);
            ImGui::DragFloat("Speed", &preset.windSpeed, 0.1f, 0.0f, 50.0f, "%.1f m/s");

            float windDir[3] = {preset.windDirection.x, preset.windDirection.y, preset.windDirection.z};
            if (ImGui::DragFloat3("Direction", windDir, 0.1f, -1.0f, 1.0f))
                preset.windDirection = {windDir[0], windDir[1], windDir[2]};
            ImGui::Unindent(4);
        }

        if (ImGui::CollapsingHeader("Fog"))
        {
            ImGui::Indent(4);
            ImGui::DragFloat("Density", &preset.fogDensity, 0.001f, 0.0f, 1.0f);

            float fogCol[4] = {preset.fogColor.x, preset.fogColor.y, preset.fogColor.z, preset.fogColor.w};
            if (ImGui::ColorEdit4("Fog Color", fogCol))
                preset.fogColor = {fogCol[0], fogCol[1], fogCol[2], fogCol[3]};
            ImGui::Unindent(4);
        }

        if (ImGui::CollapsingHeader("Atmosphere"))
        {
            ImGui::Indent(4);
            ImGui::DragFloat("Ambient Multiplier", &preset.ambientMultiplier, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Lightning Freq", &preset.lightningFrequency, 0.5f, 0.0f, 30.0f, "%.1f/min");
            ImGui::Unindent(4);
        }

        if (ImGui::CollapsingHeader("Surface Effects"))
        {
            ImGui::Indent(4);
            ImGui::SliderFloat("Wetness", &preset.wetness, 0.0f, 1.0f);
            ImGui::SliderFloat("Snow Coverage", &preset.snowCoverage, 0.0f, 1.0f);
            ImGui::Unindent(4);
        }

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " Delete Preset") && m_presets.size() > 1)
        {
            m_presets.erase(m_presets.begin() + m_selectedPreset);
            m_selectedPreset = -1;
        }
    }

} // namespace SparkEditor
