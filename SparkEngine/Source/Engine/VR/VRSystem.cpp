/**
 * @file VRSystem.cpp
 * @brief Implementation of the VR system framework (OpenXR-ready)
 */

#include "VRSystem.h"

#include <sstream>

namespace Spark::VR
{

    VRSystem::VRSystem() = default;
    VRSystem::~VRSystem()
    {
        if (m_initialized)
        {
            Shutdown();
        }
    }

    bool VRSystem::Initialize()
    {
        // OpenXR initialization would happen here:
        // 1. xrCreateInstance()
        // 2. xrGetSystem()
        // 3. xrCreateSession()
        // 4. xrCreateSwapchain() for each eye
        //
        // For now, provide the framework stub
        m_initialized = false; // Set to true when OpenXR is linked
        return m_initialized;
    }

    void VRSystem::Shutdown()
    {
        // Release OpenXR resources
        m_initialized = false;
    }

    void VRSystem::UpdateTracking()
    {
        if (!m_initialized)
        {
            return;
        }
        // OpenXR: xrWaitFrame(), xrBeginFrame(), xrLocateViews()
        // Update m_headPosition, m_headOrientation, controllers, eye matrices
    }

    std::pair<int, int> VRSystem::GetRecommendedRenderSize() const
    {
        return {m_recommendedWidth, m_recommendedHeight};
    }

    void VRSystem::TriggerHaptic(bool isLeft, float amplitude, float duration)
    {
        if (!m_initialized)
        {
            return;
        }
        (void)isLeft;
        (void)amplitude;
        (void)duration;
        // OpenXR: xrApplyHapticFeedback()
    }

    void VRSystem::RecenterTracking()
    {
        if (!m_initialized)
        {
            return;
        }
        // OpenXR: create new reference space centered at current position
    }

    std::string VRSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== VR System ===\n";
        oss << "Initialized: " << (m_initialized ? "YES" : "NO") << "\n";
        if (m_initialized)
        {
            oss << "Head pos: (" << m_headPosition.x << ", " << m_headPosition.y << ", " << m_headPosition.z << ")\n";
            oss << "Left controller: " << (m_leftController.connected ? "Connected" : "N/A") << "\n";
            oss << "Right controller: " << (m_rightController.connected ? "Connected" : "N/A") << "\n";
            const char* spaceNames[] = {"Seated", "RoomScale"};
            oss << "Tracking: " << spaceNames[static_cast<int>(m_trackingSpace)] << "\n";
            oss << "Render size: " << m_recommendedWidth << "x" << m_recommendedHeight << " per eye\n";
        }
        else
        {
            oss << "VR hardware not detected or OpenXR not linked.\n";
        }
        return oss.str();
    }

} // namespace Spark::VR
