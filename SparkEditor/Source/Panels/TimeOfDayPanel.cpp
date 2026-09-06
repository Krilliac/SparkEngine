/**
 * @file TimeOfDayPanel.cpp
 * @brief Implementation of the time-of-day editor panel
 */

#include "TimeOfDayPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Utils/LogMacros.h"
#include <cstdio>
#include <imgui.h>

namespace SparkEditor
{

    namespace
    {
        /**
         * @brief Resolve the one TimeOfDaySystem this process uses.
         *
         * The engine registers the same singleton in the EngineContext, so both
         * paths reach one instance; the context lookup keeps the panel correct if
         * a host ever registers a different instance.
         */
        Spark::TimeOfDaySystem& ResolveTimeOfDay()
        {
            if (auto* context = ::EngineContext::Get())
            {
                if (auto* timeOfDay = context->GetTimeOfDay())
                {
                    return *timeOfDay;
                }
            }
            return Spark::TimeOfDaySystem::GetInstance();
        }
    } // namespace

    TimeOfDayPanel::TimeOfDayPanel() : EditorPanel("Time of Day", "time_of_day_panel") {}

    bool TimeOfDayPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Time of Day panel");
        m_hourSlider = GetHour();
        m_timeScaleSlider = GetTimeScale();
        return true;
    }

    void TimeOfDayPanel::Update(float deltaTime)
    {
        if (IsDrivingClock())
        {
            ResolveTimeOfDay().Update(deltaTime);
        }

        m_hourSlider = GetHour();
        m_timeScaleSlider = GetTimeScale();
    }

    void TimeOfDayPanel::SetHour(float hour)
    {
        ResolveTimeOfDay().SetTimeOfDay(hour);
        m_hourSlider = GetHour();
    }

    void TimeOfDayPanel::SetTimeScale(float scale)
    {
        ResolveTimeOfDay().SetTimeScale(scale);
        m_timeScaleSlider = GetTimeScale();
    }

    void TimeOfDayPanel::SetPaused(bool paused)
    {
        ResolveTimeOfDay().SetPaused(paused);
    }

    float TimeOfDayPanel::GetHour() const
    {
        return ResolveTimeOfDay().GetTimeOfDay();
    }

    float TimeOfDayPanel::GetTimeScale() const
    {
        return ResolveTimeOfDay().GetTimeScale();
    }

    bool TimeOfDayPanel::IsPaused() const
    {
        return ResolveTimeOfDay().IsPaused();
    }

    bool TimeOfDayPanel::IsDrivingClock() const
    {
        return ::EngineContext::Get() == nullptr;
    }

    void TimeOfDayPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderTimeControls();
            ImGui::Separator();
            RenderPresets();
            ImGui::Separator();
            RenderLightingPreview();
            ImGui::Separator();
            RenderDayInfo();
        }
        EndPanel();
    }

    void TimeOfDayPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Time of Day panel");
    }

    void TimeOfDayPanel::RenderTimeControls()
    {
        const float hour = GetHour();
        const int hours = static_cast<int>(hour);
        const int minutes = static_cast<int>((hour - static_cast<float>(hours)) * 60.0f);
        char timeStr[16];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hours, minutes);

        ImGui::Text(ICON_FA_CLOCK " Time: %s", timeStr);

        if (ImGui::SliderFloat("##TimeSlider", &m_hourSlider, 0.0f, 23.99f, "%.2f h"))
        {
            SetHour(m_hourSlider);
        }

        ImGui::Text(ICON_FA_TACHOMETER_ALT " Time Scale:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::DragFloat("##TimeScale", &m_timeScaleSlider, 1.0f, 0.0f, 3600.0f, "%.0f x"))
        {
            SetTimeScale(m_timeScaleSlider);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("1x"))
            SetTimeScale(1.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("10x"))
            SetTimeScale(10.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("60x"))
            SetTimeScale(60.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("600x"))
            SetTimeScale(600.0f);

        const bool paused = IsPaused();
        if (ImGui::Button(paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
            SetPaused(!paused);

        if (!IsDrivingClock())
        {
            ImGui::TextDisabled("Clock advanced by the running engine lifecycle");
        }
    }

    void TimeOfDayPanel::RenderPresets()
    {
        ImGui::Text(ICON_FA_SUN " Presets:");

        if (ImGui::Button("Dawn (6:00)"))
            SetHour(6.0f);
        ImGui::SameLine();
        if (ImGui::Button("Morning (9:00)"))
            SetHour(9.0f);
        ImGui::SameLine();
        if (ImGui::Button("Noon (12:00)"))
            SetHour(12.0f);
        ImGui::SameLine();
        if (ImGui::Button("Dusk (18:00)"))
            SetHour(18.0f);
        ImGui::SameLine();
        if (ImGui::Button("Midnight (0:00)"))
            SetHour(0.0f);
    }

    void TimeOfDayPanel::RenderLightingPreview()
    {
        const Spark::TimeOfDaySystem& timeOfDay = ResolveTimeOfDay();
        const auto sunDirection = timeOfDay.GetSunDirection();
        const auto sunColor = timeOfDay.GetSunColor();
        const auto ambientColor = timeOfDay.GetAmbientColor();

        ImGui::Text(ICON_FA_LIGHTBULB " Lighting State");
        ImGui::Text("Sun Direction: (%.2f, %.2f, %.2f)", sunDirection.x, sunDirection.y, sunDirection.z);

        ImGui::Text("Sun Color:");
        ImGui::SameLine();
        ImGui::ColorButton("##SunColor", ImVec4(sunColor.x, sunColor.y, sunColor.z, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
        ImGui::SameLine();
        ImGui::Text("Intensity: %.2f", timeOfDay.GetSunIntensity());

        ImGui::Text("Ambient:");
        ImGui::SameLine();
        ImGui::ColorButton("##AmbientColor", ImVec4(ambientColor.x, ambientColor.y, ambientColor.z, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
        ImGui::SameLine();
        ImGui::Text("Intensity: %.2f", timeOfDay.GetAmbientIntensity());

        const bool isNight = timeOfDay.IsNight();
        ImGui::TextColored(isNight ? ImVec4(0.4f, 0.4f, 0.8f, 1.0f) : ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           isNight ? ICON_FA_ADJUST " Night" : ICON_FA_SUN " Day");
    }

    void TimeOfDayPanel::RenderDayInfo()
    {
        const Spark::TimeOfDaySystem& timeOfDay = ResolveTimeOfDay();

        const char* period = "Unknown";
        switch (timeOfDay.GetDayPeriod())
        {
        case Spark::DayPeriod::Night:
            period = "Night";
            break;
        case Spark::DayPeriod::Dawn:
            period = "Dawn";
            break;
        case Spark::DayPeriod::Morning:
            period = "Morning";
            break;
        case Spark::DayPeriod::Midday:
            period = "Midday";
            break;
        case Spark::DayPeriod::Afternoon:
            period = "Afternoon";
            break;
        case Spark::DayPeriod::Dusk:
            period = "Dusk";
            break;
        case Spark::DayPeriod::Evening:
            period = "Evening";
            break;
        case Spark::DayPeriod::LateNight:
            period = "Late Night";
            break;
        }

        ImGui::Text("Period: %s", period);
        ImGui::Text("Day: %d", timeOfDay.GetDayCount() + 1);

        ImGui::ProgressBar(GetHour() / 24.0f, ImVec2(-1, 0), "Day Cycle");
    }

} // namespace SparkEditor
