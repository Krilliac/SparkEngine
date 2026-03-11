/**
 * @file MobilePlatform.cpp
 * @brief Implementation of the mobile platform abstraction
 */

#include "MobilePlatform.h"

#include <algorithm>
#include <sstream>

namespace Spark::Mobile
{

    MobilePlatform::MobilePlatform() = default;

    bool MobilePlatform::Initialize()
    {
        m_qualitySettings = GetSettingsForPreset(m_qualityPreset);
        m_initialized = true;
        return true;
    }

    void MobilePlatform::Update(float deltaTime)
    {
        (void)deltaTime;
        RecognizeGestures();
        m_pendingGestures.clear();

        // Battery-aware scaling
        if (m_batteryAwareScaling && m_batteryLevel >= 0.0f && !m_isCharging)
        {
            if (m_batteryLevel < 0.1f && m_qualityPreset != MobileQualityPreset::Low)
            {
                SetQualityPreset(MobileQualityPreset::Low);
            }
            else if (m_batteryLevel < 0.25f && m_qualityPreset == MobileQualityPreset::High)
            {
                SetQualityPreset(MobileQualityPreset::Medium);
            }
        }
    }

    void MobilePlatform::ProcessTouchEvent(const TouchEvent& event)
    {
        // Update active touches
        if (event.type == TouchEventType::Began)
        {
            m_activeTouches.push_back(event);
        }
        else if (event.type == TouchEventType::Moved)
        {
            for (auto& touch : m_activeTouches)
            {
                if (touch.touchId == event.touchId)
                {
                    touch.x = event.x;
                    touch.y = event.y;
                    touch.pressure = event.pressure;
                    break;
                }
            }
        }
        else if (event.type == TouchEventType::Ended || event.type == TouchEventType::Cancelled)
        {
            m_activeTouches.erase(std::remove_if(m_activeTouches.begin(), m_activeTouches.end(),
                                                 [&](const TouchEvent& t) { return t.touchId == event.touchId; }),
                                  m_activeTouches.end());
        }
    }

    std::vector<TouchEvent> MobilePlatform::GetActiveTouches() const
    {
        return m_activeTouches;
    }

    std::vector<Gesture> MobilePlatform::GetGestures() const
    {
        return m_pendingGestures;
    }

    void MobilePlatform::OnGesture(GestureType type, std::function<void(const Gesture&)> callback)
    {
        m_gestureCallbacks[static_cast<int>(type)].push_back(std::move(callback));
    }

    void MobilePlatform::SetQualityPreset(MobileQualityPreset preset)
    {
        m_qualityPreset = preset;
        m_qualitySettings = GetSettingsForPreset(preset);
    }

    void MobilePlatform::SetOrientation(ScreenOrientation orientation)
    {
        m_orientation = orientation;
    }

    void MobilePlatform::RecognizeGestures()
    {
        // Simple gesture recognition from active touches
        // Full implementation would track touch history and timing
        if (m_activeTouches.size() == 1)
        {
            Gesture tap;
            tap.type = GestureType::Tap;
            tap.x = m_activeTouches[0].x;
            tap.y = m_activeTouches[0].y;
            tap.touchCount = 1;
            // Only add if touch just ended (simplified)
        }
    }

    MobileQualitySettings MobilePlatform::GetSettingsForPreset(MobileQualityPreset preset) const
    {
        MobileQualitySettings settings;
        switch (preset)
        {
        case MobileQualityPreset::Low:
            settings.renderScale = 0.5f;
            settings.shadowResolution = 256;
            settings.enablePostProcessing = false;
            settings.enableParticles = false;
            settings.maxDrawCalls = 200;
            settings.textureQuality = 0;
            settings.enableSSAO = false;
            settings.lodBias = 2.0f;
            break;
        case MobileQualityPreset::Medium:
            settings.renderScale = 0.75f;
            settings.shadowResolution = 512;
            settings.enablePostProcessing = true;
            settings.enableParticles = true;
            settings.maxDrawCalls = 400;
            settings.textureQuality = 1;
            settings.enableSSAO = false;
            settings.lodBias = 1.0f;
            break;
        case MobileQualityPreset::High:
        case MobileQualityPreset::Auto:
        default:
            settings.renderScale = 1.0f;
            settings.shadowResolution = 1024;
            settings.enablePostProcessing = true;
            settings.enableParticles = true;
            settings.maxDrawCalls = 600;
            settings.textureQuality = 2;
            settings.enableSSAO = true;
            settings.lodBias = 0.0f;
            break;
        }
        return settings;
    }

    std::string MobilePlatform::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Mobile Platform ===\n";
        oss << "Initialized: " << (m_initialized ? "YES" : "NO") << "\n";
        const char* presetNames[] = {"Low", "Medium", "High", "Auto"};
        oss << "Quality: " << presetNames[static_cast<int>(m_qualityPreset)] << "\n";
        oss << "Render scale: " << m_qualitySettings.renderScale << "\n";
        oss << "Shadow res: " << m_qualitySettings.shadowResolution << "\n";
        oss << "Active touches: " << m_activeTouches.size() << "\n";
        oss << "Battery: "
            << (m_batteryLevel >= 0 ? std::to_string(static_cast<int>(m_batteryLevel * 100)) + "%" : "N/A") << "\n";
        oss << "Charging: " << (m_isCharging ? "YES" : "NO") << "\n";
        oss << "Battery scaling: " << (m_batteryAwareScaling ? "ON" : "OFF") << "\n";
        return oss.str();
    }

} // namespace Spark::Mobile
