/**
 * @file VRConfigPanel.cpp
 * @brief Implementation of the VR configuration panel
 */

#include "VRConfigPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    VRConfigPanel::VRConfigPanel() : EditorPanel("VR Config", "vr_config_panel") {}

    bool VRConfigPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing VR Config panel");
        return true;
    }

    void VRConfigPanel::Update(float /*deltaTime*/) {}

    void VRConfigPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("VRConfigTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_GAMEPAD " Device"))
                {
                    RenderDeviceStatus();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CROSSHAIRS " Tracking"))
                {
                    RenderTrackingInfo();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_HAND_POINTER " Controllers"))
                {
                    RenderControllerMapping();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_BOLT " Haptics"))
                {
                    RenderHapticTest();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void VRConfigPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down VR Config panel");
    }

    void VRConfigPanel::RenderDeviceStatus()
    {
        if (m_vrAvailable)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), ICON_FA_CHECK " VR Runtime Available");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.2f, 1.0f), ICON_FA_EXCLAMATION " No VR Runtime Detected");
            ImGui::TextDisabled("Install OpenXR or SteamVR to enable VR support.");
        }

        ImGui::Separator();
        ImGui::Text("Recommended Render Size: %d x %d", m_renderWidth, m_renderHeight);

        const char* trackingSpaces[] = {"Seated", "Room Scale"};
        ImGui::Combo("Tracking Space", &m_trackingSpace, trackingSpaces, 2);

        if (ImGui::Button("Recenter Tracking"))
        {
            // Would call VRSystem::RecenterTracking()
        }
    }

    void VRConfigPanel::RenderTrackingInfo()
    {
        ImGui::Text("Head Position: (%.2f, %.2f, %.2f)", m_headPosition[0], m_headPosition[1], m_headPosition[2]);
        ImGui::Text("Head Orientation: (%.2f, %.2f, %.2f, %.2f)", m_headOrientation[0], m_headOrientation[1],
                    m_headOrientation[2], m_headOrientation[3]);

        ImGui::Separator();
        ImGui::TextDisabled("Tracking data updates in Play mode when VR is active.");
    }

    void VRConfigPanel::RenderControllerMapping()
    {
        ImGui::Text("Left Controller: %s",
                    m_leftControllerConnected ? (ICON_FA_CHECK " Connected") : (ICON_FA_TIMES " Disconnected"));
        ImGui::Text("Right Controller: %s",
                    m_rightControllerConnected ? (ICON_FA_CHECK " Connected") : (ICON_FA_TIMES " Disconnected"));

        ImGui::Separator();
        ImGui::Text("Left Trigger: %.2f", m_leftTrigger);
        ImGui::Text("Right Trigger: %.2f", m_rightTrigger);

        ImGui::Separator();
        ImGui::TextDisabled("Controller button mapping is configured in Project Settings.");
    }

    void VRConfigPanel::RenderHapticTest()
    {
        ImGui::SliderFloat("Amplitude", &m_hapticAmplitude, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Duration", &m_hapticDuration, 0.01f, 1.0f, "%.2f s");

        ImGui::Separator();
        if (ImGui::Button("Test Left"))
        {
            // Would call VRSystem::TriggerHaptic(true, m_hapticAmplitude, m_hapticDuration)
        }
        ImGui::SameLine();
        if (ImGui::Button("Test Right"))
        {
            // Would call VRSystem::TriggerHaptic(false, m_hapticAmplitude, m_hapticDuration)
        }
    }

} // namespace SparkEditor
