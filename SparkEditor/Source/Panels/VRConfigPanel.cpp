/**
 * @file VRConfigPanel.cpp
 * @brief Implementation of the VR configuration panel
 */

#include "VRConfigPanel.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

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
            Spark::Ui::StatusText(Spark::Ui::Status::Success, "VR Runtime Available");
        }
        else
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "No VR Runtime Detected");
            Spark::Ui::SecondaryText("Install OpenXR or SteamVR to enable VR support.");
        }

        ImGui::Separator();
        if (Spark::Ui::BeginPropertyGrid("VRDevice"))
        {
            Spark::Ui::PropertyLabel("Recommended Render Size");
            Spark::Ui::MonoText("%d x %d", m_renderWidth, m_renderHeight);
            const char* trackingSpaces[] = {"Seated", "Room Scale"};
            Spark::Ui::PropertyLabel("Tracking Space");
            ImGui::Combo("##TrackingSpace", &m_trackingSpace, trackingSpaces, 2);
            Spark::Ui::EndPropertyGrid();
        }

        Spark::Ui::UnavailableFeature("Recenter Tracking",
                                      "The editor holds no VR session; recentering runs in the launched game.");
    }

    void VRConfigPanel::RenderTrackingInfo()
    {
        if (Spark::Ui::BeginPropertyGrid("VRTracking"))
        {
            Spark::Ui::PropertyLabel("Head Position");
            Spark::Ui::MonoText("(%.2f, %.2f, %.2f)", m_headPosition[0], m_headPosition[1], m_headPosition[2]);
            Spark::Ui::PropertyLabel("Head Orientation");
            Spark::Ui::MonoText("(%.2f, %.2f, %.2f, %.2f)", m_headOrientation[0], m_headOrientation[1],
                                m_headOrientation[2], m_headOrientation[3]);
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("State preview does not update VR tracking.");
        Spark::Ui::SecondaryText(
            "Use Play Control > Launch Game to test runtime VR tracking (state preview is not gameplay).");
        ImGui::PopTextWrapPos();
    }

    void VRConfigPanel::RenderControllerMapping()
    {
        if (Spark::Ui::BeginPropertyGrid("VRControllers"))
        {
            Spark::Ui::PropertyLabel("Left Controller");
            Spark::Ui::StatusText(m_leftControllerConnected ? Spark::Ui::Status::Success : Spark::Ui::Status::Neutral,
                                  "%s", m_leftControllerConnected ? "Connected" : "Disconnected");
            Spark::Ui::PropertyLabel("Right Controller");
            Spark::Ui::StatusText(m_rightControllerConnected ? Spark::Ui::Status::Success : Spark::Ui::Status::Neutral,
                                  "%s", m_rightControllerConnected ? "Connected" : "Disconnected");
            Spark::Ui::PropertyLabel("Left Trigger");
            Spark::Ui::MonoText("%.2f", m_leftTrigger);
            Spark::Ui::PropertyLabel("Right Trigger");
            Spark::Ui::MonoText("%.2f", m_rightTrigger);
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();
        Spark::Ui::SecondaryText("Controller button mapping is configured in Project Settings.");
    }

    void VRConfigPanel::RenderHapticTest()
    {
        if (Spark::Ui::BeginPropertyGrid("VRHaptics"))
        {
            Spark::Ui::PropertyLabel("Amplitude");
            ImGui::SliderFloat("##Amplitude", &m_hapticAmplitude, 0.0f, 1.0f, "%.2f");
            Spark::Ui::PropertyLabel("Duration");
            ImGui::SliderFloat("##Duration", &m_hapticDuration, 0.01f, 1.0f, "%.2f s");
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();
        Spark::Ui::UnavailableFeature("Haptic test pulses",
                                      "The editor holds no VR session; test haptics in the launched game.");
    }

} // namespace SparkEditor
