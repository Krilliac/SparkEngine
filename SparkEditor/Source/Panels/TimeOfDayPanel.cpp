/**
 * @file TimeOfDayPanel.cpp
 * @brief Implementation of the time-of-day editor panel
 */

#include "TimeOfDayPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cmath>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    TimeOfDayPanel::TimeOfDayPanel() : EditorPanel("Time of Day", "time_of_day_panel") {}

    bool TimeOfDayPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Time of Day panel");
        return true;
    }

    void TimeOfDayPanel::Update(float deltaTime)
    {
        if (!m_paused && m_timeScale > 0.0f)
        {
            m_currentHour += (deltaTime * m_timeScale) / 3600.0f;
            while (m_currentHour >= 24.0f)
            {
                m_currentHour -= 24.0f;
                ++m_dayCount;
            }

            // Update sun direction from hour (simplified spherical mapping)
            float angle = (m_currentHour / 24.0f) * 2.0f * 3.14159265f - 1.5707963f;
            m_sunDirection[0] = std::cos(angle);
            m_sunDirection[1] = std::sin(angle);
            m_sunDirection[2] = 0.3f;

            // Sun intensity based on elevation
            m_sunIntensity = std::max(0.0f, m_sunDirection[1]);

            // Sun color shifts warm at dawn/dusk
            float elevationFactor = std::max(0.0f, std::min(1.0f, m_sunDirection[1] * 3.0f));
            m_sunColor[0] = 1.0f;
            m_sunColor[1] = 0.7f + 0.3f * elevationFactor;
            m_sunColor[2] = 0.4f + 0.6f * elevationFactor;

            // Ambient dims at night
            m_ambientIntensity = 0.1f + 0.2f * m_sunIntensity;
        }
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
        // Time slider
        int hours = static_cast<int>(m_currentHour);
        int minutes = static_cast<int>((m_currentHour - hours) * 60.0f);
        char timeStr[16];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hours, minutes);

        ImGui::Text(ICON_FA_CLOCK " Time: %s", timeStr);

        if (ImGui::SliderFloat("##TimeSlider", &m_currentHour, 0.0f, 23.99f, "%.2f h"))
        {
            // Clamp to valid range
            if (m_currentHour < 0.0f)
                m_currentHour = 0.0f;
        }

        // Time scale
        ImGui::Text(ICON_FA_TACHOMETER_ALT " Time Scale:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::DragFloat("##TimeScale", &m_timeScale, 1.0f, 0.0f, 3600.0f, "%.0f x");

        // Quick scale buttons
        ImGui::SameLine();
        if (ImGui::SmallButton("1x"))
            m_timeScale = 1.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("10x"))
            m_timeScale = 10.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("60x"))
            m_timeScale = 60.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("600x"))
            m_timeScale = 600.0f;

        // Pause/resume
        if (ImGui::Button(m_paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
            m_paused = !m_paused;
    }

    void TimeOfDayPanel::RenderPresets()
    {
        ImGui::Text(ICON_FA_SUN " Presets:");

        if (ImGui::Button("Dawn (6:00)"))
            m_currentHour = 6.0f;
        ImGui::SameLine();
        if (ImGui::Button("Morning (9:00)"))
            m_currentHour = 9.0f;
        ImGui::SameLine();
        if (ImGui::Button("Noon (12:00)"))
            m_currentHour = 12.0f;
        ImGui::SameLine();
        if (ImGui::Button("Dusk (18:00)"))
            m_currentHour = 18.0f;
        ImGui::SameLine();
        if (ImGui::Button("Midnight (0:00)"))
            m_currentHour = 0.0f;
    }

    void TimeOfDayPanel::RenderLightingPreview()
    {
        ImGui::Text(ICON_FA_LIGHTBULB " Lighting State");

        // Sun direction
        ImGui::Text("Sun Direction: (%.2f, %.2f, %.2f)", m_sunDirection[0], m_sunDirection[1], m_sunDirection[2]);

        // Sun color swatch
        ImGui::Text("Sun Color:");
        ImGui::SameLine();
        ImGui::ColorButton("##SunColor", ImVec4(m_sunColor[0], m_sunColor[1], m_sunColor[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
        ImGui::SameLine();
        ImGui::Text("Intensity: %.2f", m_sunIntensity);

        // Ambient color swatch
        ImGui::Text("Ambient:");
        ImGui::SameLine();
        ImGui::ColorButton("##AmbientColor", ImVec4(m_ambientColor[0], m_ambientColor[1], m_ambientColor[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
        ImGui::SameLine();
        ImGui::Text("Intensity: %.2f", m_ambientIntensity);

        // Day/night indicator
        bool isNight = m_sunDirection[1] < 0.0f;
        ImGui::TextColored(isNight ? ImVec4(0.4f, 0.4f, 0.8f, 1.0f) : ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           isNight ? ICON_FA_ADJUST " Night" : ICON_FA_SUN " Day");
    }

    void TimeOfDayPanel::RenderDayInfo()
    {
        // Day period
        const char* period = "Unknown";
        if (m_currentHour < 5.0f)
            period = "Night";
        else if (m_currentHour < 7.0f)
            period = "Dawn";
        else if (m_currentHour < 10.0f)
            period = "Morning";
        else if (m_currentHour < 14.0f)
            period = "Midday";
        else if (m_currentHour < 17.0f)
            period = "Afternoon";
        else if (m_currentHour < 19.0f)
            period = "Dusk";
        else if (m_currentHour < 21.0f)
            period = "Evening";
        else
            period = "Late Night";

        ImGui::Text("Period: %s", period);
        ImGui::Text("Day: %d", m_dayCount + 1);

        // Progress bar for day cycle
        float dayProgress = m_currentHour / 24.0f;
        ImGui::ProgressBar(dayProgress, ImVec2(-1, 0), "Day Cycle");
    }

} // namespace SparkEditor
