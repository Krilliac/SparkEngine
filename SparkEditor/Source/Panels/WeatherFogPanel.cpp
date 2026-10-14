/**
 * @file WeatherFogPanel.cpp
 * @brief Implementation of the weather and fog editor panel
 */

#include "WeatherFogPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
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
            std::snprintf(label, sizeof(label), "%s %s", typeIcons[i], Spark::WeatherSystem::GetWeatherTypeName(type));
            if (ImGui::Selectable(label, m_selectedType == type))
                m_selectedType = type;
        }
    }

    void WeatherFogPanel::RenderPresetDetails()
    {
        const Spark::WeatherState preset = GetSelectedPreset();

        char title[96];
        std::snprintf(title, sizeof(title), "%s preset (engine values)",
                      Spark::WeatherSystem::GetWeatherTypeName(m_selectedType));
        Spark::Ui::SubHeader(title);

        if (Spark::Ui::BeginPropertyGrid("WeatherPresetValues"))
        {
            Spark::Ui::PropertyLabel("Precipitation rate");
            Spark::Ui::MonoText("%.0f /s", preset.precipitationRate);
            Spark::Ui::PropertyLabel("Wind speed");
            Spark::Ui::MonoText("%.1f m/s (gustiness %.2f)", preset.windSpeed, preset.windGustiness);
            Spark::Ui::PropertyLabel("Fog density");
            Spark::Ui::MonoText("%.3f  (%.0f m - %.0f m)", preset.fogDensity, preset.fogStartDistance,
                                preset.fogEndDistance);
            Spark::Ui::PropertyLabel("Fog color");
            ImGui::ColorButton("##FogColor",
                               ImVec4(preset.fogColor.x, preset.fogColor.y, preset.fogColor.z, preset.fogColor.w),
                               ImGuiColorEditFlags_NoTooltip, Spark::Ui::Px2(40, 20));
            Spark::Ui::PropertyLabel("Lighting");
            Spark::Ui::MonoText("Ambient x%.2f  Directional x%.2f", preset.ambientMultiplier,
                                preset.directionalMultiplier);
            Spark::Ui::PropertyLabel("Lightning");
            Spark::Ui::MonoText("%.1f /min", preset.lightningFrequency);
            Spark::Ui::PropertyLabel("Wetness");
            Spark::Ui::MonoText("%.2f", preset.wetness);
            Spark::Ui::PropertyLabel("Snow");
            Spark::Ui::MonoText("%.2f", preset.snowCoverage);
            Spark::Ui::EndPropertyGrid();
        }

        Spark::Ui::SubHeader("Apply");
        if (Spark::Ui::BeginPropertyGrid("WeatherApply"))
        {
            Spark::Ui::PropertyLabel("Intensity");
            ImGui::SliderFloat("##Intensity", &m_intensity, 0.0f, 1.0f);
            Spark::Ui::PropertyLabel("Transition");
            ImGui::SliderFloat("##Transition", &m_transitionSeconds, 0.1f, 30.0f, "%.1f s");
            Spark::Ui::EndPropertyGrid();
        }

        const bool connected = IsWeatherSystemConnected();
        {
            const Spark::Ui::ScopedDisabled disabled(!connected, "No WeatherSystem is registered in this process.");
            if (Spark::Ui::PrimaryButton(ICON_FA_CHECK " Apply to Scene"))
            {
                ApplySelected();
            }
        }

        if (!connected)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Info,
                                  "Preview - not connected: no WeatherSystem is registered in this process.");
            Spark::Ui::SecondaryText("Start a game session to apply weather.");
            return;
        }

        const Spark::WeatherState& current = ResolveWeatherSystem()->GetCurrentState();
        Spark::Ui::SubHeader("Live weather");
        if (Spark::Ui::BeginPropertyGrid("WeatherLive"))
        {
            Spark::Ui::PropertyLabel("Type");
            ImGui::TextUnformatted(Spark::WeatherSystem::GetWeatherTypeName(current.type));
            Spark::Ui::PropertyLabel("Intensity");
            Spark::Ui::MonoText("%.2f", current.intensity);
            if (ResolveWeatherSystem()->IsTransitioning())
            {
                Spark::Ui::PropertyLabel("Transition");
                Spark::Ui::StatusText(Spark::Ui::Status::Info, "%.0f%%",
                                      ResolveWeatherSystem()->GetTransitionProgress() * 100.0f);
            }
            Spark::Ui::EndPropertyGrid();
        }
    }

} // namespace SparkEditor
