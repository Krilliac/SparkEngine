/**
 * @file WeatherFogPanel.cpp
 * @brief Implementation of the weather and fog editor panel
 */

#include "WeatherFogPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Utils/LogMacros.h"
#include <cstdio>
#include <imgui.h>

namespace SparkEditor
{

    namespace
    {
        Spark::WeatherSystem* ResolveWeatherSystem()
        {
            auto* context = ::EngineContext::Get();
            return context ? context->GetWeather() : nullptr;
        }
    } // namespace

    WeatherFogPanel::WeatherFogPanel() : EditorPanel("Weather & Fog", "weather_fog_panel") {}

    bool WeatherFogPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Weather & Fog panel");
        return true;
    }

    void WeatherFogPanel::Update(float /*deltaTime*/) {}

    bool WeatherFogPanel::IsWeatherSystemConnected() const
    {
        return ResolveWeatherSystem() != nullptr;
    }

    bool WeatherFogPanel::ApplySelected()
    {
        Spark::WeatherSystem* weather = ResolveWeatherSystem();
        if (!weather)
        {
            return false;
        }
        weather->SetWeather(m_selectedType, m_intensity, m_transitionSeconds);
        return true;
    }

    void WeatherFogPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            const float listWidth = ImGui::GetContentRegionAvail().x * 0.3f;
            ImGui::BeginChild("WeatherTypeList", ImVec2(listWidth, 0), true);
            RenderPresetList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("WeatherDetails", ImVec2(0, 0), true);
            RenderPresetDetails();
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
        const char* typeIcons[] = {ICON_FA_SUN,       ICON_FA_CLOUD, ICON_FA_CLOUD_RAIN,
                                   ICON_FA_SNOWFLAKE, ICON_FA_SMOG,  ICON_FA_BOLT};

        for (int i = 0; i < static_cast<int>(Spark::WeatherType::Count); ++i)
        {
            const auto type = static_cast<Spark::WeatherType>(i);
            char label[128];
            snprintf(label, sizeof(label), "%s %s", typeIcons[i], Spark::WeatherSystem::GetWeatherTypeName(type));
            if (ImGui::Selectable(label, m_selectedType == type))
                m_selectedType = type;
        }
    }

    void WeatherFogPanel::RenderPresetDetails()
    {
        const Spark::WeatherState preset = GetSelectedPreset();

        ImGui::Text("%s preset (engine values)", Spark::WeatherSystem::GetWeatherTypeName(m_selectedType));
        ImGui::Separator();

        ImGui::Text("Precipitation rate: %.0f /s", preset.precipitationRate);
        ImGui::Text("Wind speed: %.1f m/s (gustiness %.2f)", preset.windSpeed, preset.windGustiness);
        ImGui::Text("Fog density: %.3f  (%.0f m - %.0f m)", preset.fogDensity, preset.fogStartDistance,
                    preset.fogEndDistance);
        ImGui::Text("Fog color:");
        ImGui::SameLine();
        ImGui::ColorButton("##FogColor",
                           ImVec4(preset.fogColor.x, preset.fogColor.y, preset.fogColor.z, preset.fogColor.w),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
        ImGui::Text("Ambient x%.2f  Directional x%.2f", preset.ambientMultiplier, preset.directionalMultiplier);
        ImGui::Text("Lightning: %.1f /min   Wetness: %.2f   Snow: %.2f", preset.lightningFrequency, preset.wetness,
                    preset.snowCoverage);

        ImGui::Separator();
        ImGui::SliderFloat("Intensity", &m_intensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Transition", &m_transitionSeconds, 0.1f, 30.0f, "%.1f s");

        const bool connected = IsWeatherSystemConnected();
        ImGui::BeginDisabled(!connected);
        if (ImGui::Button(ICON_FA_CHECK " Apply to Scene"))
        {
            ApplySelected();
        }
        ImGui::EndDisabled();

        if (!connected)
        {
            ImGui::TextDisabled("Preview - not connected: no WeatherSystem is registered in this");
            ImGui::TextDisabled("process. Start a game session to apply weather.");
            return;
        }

        const Spark::WeatherState& current = ResolveWeatherSystem()->GetCurrentState();
        ImGui::Separator();
        ImGui::Text("Live weather: %s (intensity %.2f)", Spark::WeatherSystem::GetWeatherTypeName(current.type),
                    current.intensity);
        if (ResolveWeatherSystem()->IsTransitioning())
        {
            ImGui::SameLine();
            ImGui::Text("- transitioning %.0f%%", ResolveWeatherSystem()->GetTransitionProgress() * 100.0f);
        }
    }

} // namespace SparkEditor
