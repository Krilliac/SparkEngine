/**
 * @file VRConfigPanel.h
 * @brief VR device configuration and tracking panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>

namespace SparkEditor
{

    /**
     * @brief Panel for configuring VR headset, controllers, and tracking
     *
     * Shows VR device connection status, head/controller tracking data,
     * tracking space configuration, and haptic feedback testing.
     */
    class VRConfigPanel : public EditorPanel
    {
      public:
        VRConfigPanel();
        ~VRConfigPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderDeviceStatus();
        void RenderTrackingInfo();
        void RenderControllerMapping();
        void RenderHapticTest();

        bool m_vrAvailable = false;
        int m_trackingSpace = 1; // 0=Seated, 1=RoomScale
        float m_headPosition[3] = {};
        float m_headOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        bool m_leftControllerConnected = false;
        bool m_rightControllerConnected = false;
        float m_leftTrigger = 0.0f;
        float m_rightTrigger = 0.0f;
        float m_hapticAmplitude = 0.5f;
        float m_hapticDuration = 0.1f;
        int m_renderWidth = 0;
        int m_renderHeight = 0;
    };

} // namespace SparkEditor
