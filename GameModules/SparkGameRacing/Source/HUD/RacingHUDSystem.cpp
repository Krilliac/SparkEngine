/**
 * @file RacingHUDSystem.cpp
 * @brief Racing HUD overlay rendering
 */

#include "RacingHUDSystem.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{

    bool RacingHUDSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[Racing HUD] HUD system initialized");
        return true;
    }

    void RacingHUDSystem::Update(float deltaTime)
    {
        (void)deltaTime;
        // HUD is purely reactive to data; no per-frame logic needed
    }

    void RacingHUDSystem::Shutdown()
    {
        m_minimapEntries.clear();
        m_initialized = false;
    }

    void RacingHUDSystem::SetHUDData(const HUDData& data)
    {
        m_data = data;
    }

    void RacingHUDSystem::SetMinimapEntries(const std::vector<MinimapEntry>& entries)
    {
        m_minimapEntries = entries;
    }

    const char* RacingHUDSystem::GetPositionSuffix(uint32_t position)
    {
        if (position >= 11 && position <= 13)
            return "th";
        switch (position % 10)
        {
        case 1:
            return "st";
        case 2:
            return "nd";
        case 3:
            return "rd";
        default:
            return "th";
        }
    }

    void RacingHUDSystem::RenderSpeedometer()
    {
#ifdef ENABLE_EDITOR
        ImGui::Text("Speed: %d km/h", static_cast<int>(m_data.speed));

        // Speed bar gauge
        float speedFraction = std::clamp(m_data.speed / m_data.maxSpeed, 0.0f, 1.0f);
        ImVec4 speedColor = speedFraction > 0.8f ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f) : ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, speedColor);
        ImGui::ProgressBar(speedFraction, ImVec2(200, 20), "");
        ImGui::PopStyleColor();

        ImGui::Text("Gear: %u", m_data.gear);
#endif
    }

    void RacingHUDSystem::RenderTachometer()
    {
#ifdef ENABLE_EDITOR
        float rpmFraction = std::clamp(m_data.rpm, 0.0f, 1.0f);
        ImVec4 rpmColor = rpmFraction > 0.85f ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, rpmColor);
        ImGui::ProgressBar(rpmFraction, ImVec2(200, 14), "RPM");
        ImGui::PopStyleColor();
#endif
    }

    void RacingHUDSystem::RenderMinimap()
    {
#ifdef ENABLE_EDITOR
        if (!m_showMinimap || m_minimapEntries.empty())
            return;

        ImGui::BeginChild("Minimap", ImVec2(150, 150), true);
        ImGui::Text("Minimap");

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float mapSize = 130.0f;

        // Find bounds for mapping world coords to minimap
        float minX = m_minimapEntries[0].x;
        float maxX = minX;
        float minZ = m_minimapEntries[0].z;
        float maxZ = minZ;
        for (const auto& entry : m_minimapEntries)
        {
            minX = std::min(minX, entry.x);
            maxX = std::max(maxX, entry.x);
            minZ = std::min(minZ, entry.z);
            maxZ = std::max(maxZ, entry.z);
        }
        float rangeX = std::max(maxX - minX, 1.0f);
        float rangeZ = std::max(maxZ - minZ, 1.0f);

        for (const auto& entry : m_minimapEntries)
        {
            float normX = (entry.x - minX) / rangeX;
            float normZ = (entry.z - minZ) / rangeZ;
            float dotX = origin.x + normX * mapSize;
            float dotY = origin.y + normZ * mapSize;

            ImU32 color = entry.isPlayer ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 100, 100, 255);
            drawList->AddCircleFilled(ImVec2(dotX, dotY), entry.isPlayer ? 5.0f : 3.0f, color);
        }

        ImGui::EndChild();
#endif
    }

    void RacingHUDSystem::RenderPositionIndicator()
    {
#ifdef ENABLE_EDITOR
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.2f, 1.0f), "%u%s / %u", m_data.position,
                           GetPositionSuffix(m_data.position), m_data.totalRacers);
#endif
    }

    void RacingHUDSystem::RenderLapCounter()
    {
#ifdef ENABLE_EDITOR
        ImGui::Text("Lap %u / %u", m_data.currentLap, m_data.totalLaps);
        ImGui::Text("Lap Time: %.2f s", m_data.lapTime);
        if (m_data.bestLapTime >= 0.0f)
            ImGui::Text("Best Lap: %.2f s", m_data.bestLapTime);
        ImGui::Text("Total: %.2f s", m_data.totalTime);
#endif
    }

    void RacingHUDSystem::RenderBoostMeter()
    {
#ifdef ENABLE_EDITOR
        float nitroFraction = std::clamp(m_data.nitroLevel, 0.0f, 1.0f);
        ImVec4 nitroColor = m_data.boostActive > 0.0f ? ImVec4(0.2f, 0.6f, 1.0f, 1.0f) : ImVec4(0.3f, 0.3f, 0.8f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, nitroColor);
        ImGui::ProgressBar(nitroFraction, ImVec2(120, 16), "NITRO");
        ImGui::PopStyleColor();
#endif
    }

    void RacingHUDSystem::RenderCountdown()
    {
#ifdef ENABLE_EDITOR
        if (m_data.raceState != RaceState::Countdown)
            return;

        int count = static_cast<int>(std::ceil(m_data.countdown));
        if (count > 0)
        {
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.45f);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%d", count);
        }
        else
        {
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.4f);
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "GO!");
        }
#endif
    }

    void RacingHUDSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing HUD"))
            return;

        RenderCountdown();
        RenderPositionIndicator();
        ImGui::Separator();
        RenderSpeedometer();
        RenderTachometer();
        ImGui::Separator();
        RenderLapCounter();
        ImGui::Separator();
        RenderBoostMeter();
        ImGui::Separator();
        RenderMinimap();

        ImGui::Separator();
        ImGui::Checkbox("Show Speedometer", &m_showSpeedometer);
        ImGui::Checkbox("Show Minimap", &m_showMinimap);
        ImGui::Checkbox("Show Standings", &m_showStandings);
#endif
    }

} // namespace Racing
