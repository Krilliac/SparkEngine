/**
 * @file VRSystem.cpp
 * @brief Implementation of the VR system framework (OpenXR-ready)
 */

#include "VRSystem.h"
#include "../../Utils/Validate.h"

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
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "VRSystem initializing");
        // OpenXR initialization would happen here:
        // 1. xrCreateInstance()
        // 2. xrGetSystem()
        // 3. xrCreateSession()
        // 4. xrCreateSwapchain() for each eye
        //
        // For now, provide the framework stub
        m_initialized = false; // Set to true when OpenXR is linked
        m_runtimeStatus = "OpenXR runtime not linked in this build (ENABLE_VR currently provides stub backend).";
        if (!m_initialized)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VRSystem::Initialize — %s", m_runtimeStatus.c_str());
        }
        return m_initialized;
    }

    void VRSystem::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "VRSystem shutting down");
        // Release OpenXR resources
        m_initialized = false;
    }

    void VRSystem::UpdateTracking()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
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
        if (amplitude < 0.0f || amplitude > 1.0f || duration < 0.0f)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "VRSystem::TriggerHaptic invalid params (left=%d amplitude=%.3f duration=%.3f)",
                           isLeft ? 1 : 0, amplitude, duration);
            return;
        }
        if (!m_initialized)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Core, "VRSystem::TriggerHaptic ignored; runtime unavailable: %s",
                            m_runtimeStatus.c_str());
            return;
        }
        (void)isLeft;
        (void)amplitude;
        (void)duration;
        // OpenXR: xrApplyHapticFeedback()
    }

    void VRSystem::RecenterTracking()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "VR recentering tracking space");
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
            oss << "Unavailable reason: " << m_runtimeStatus << "\n";
        }
        return oss.str();
    }

} // namespace Spark::VR
