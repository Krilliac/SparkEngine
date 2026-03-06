/**
 * @file SparkFutureIntegration.h
 * @brief Future technology integration system for advanced editor modes
 * @author Spark Engine Team
 * @date 2025
 *
 * Manages transitions between different technology modes including
 * traditional 2D, enhanced 3D, VR, AI-assisted, and cloud collaborative modes.
 * Gracefully degrades when required hardware/services are not available.
 */

#pragma once

#include <string>

namespace SparkEditor {

/**
 * @brief Future technology integration modes
 */
enum class FutureTechMode {
    Traditional2D,      ///< Standard 2D docking (always available)
    Enhanced3D,         ///< 3D spatial docking (requires 3D hardware)
    VRImmersive,        ///< Full VR mode (requires VR headset)
    AIAssisted,         ///< AI-powered optimization (requires AI service)
    CloudCollaborative, ///< Cloud-synchronized collaboration (requires network)
    HybridReality,      ///< Mixed VR and traditional (requires VR headset)
    FullFuture          ///< All technologies enabled (requires all hardware)
};

/**
 * @brief Future technology integration system with mode management
 *
 * Provides technology mode detection, transition management, and
 * graceful degradation when required capabilities are unavailable.
 */
class SparkFutureIntegration {
public:
    SparkFutureIntegration() = default;
    ~SparkFutureIntegration() = default;

    /**
     * @brief Transition to a technology mode
     * @param mode Target technology mode
     * @param duration Transition duration in seconds
     *
     * If the mode is not available, the transition is rejected and
     * the current mode remains unchanged.
     */
    void TransitionToMode(FutureTechMode mode, float duration = 1.0f) {
        if (!IsModeAvailable(mode)) return;
        if (m_isTransitioning) return;

        m_previousMode = m_currentMode;
        m_targetMode = mode;
        m_transitionDuration = duration;

        if (duration <= 0.0f) {
            // Instant transition
            m_currentMode = mode;
        } else {
            m_isTransitioning = true;
            m_transitionProgress = 0.0f;
            // In a real implementation, the Update method would advance the transition
            // For now, complete immediately
            m_currentMode = mode;
            m_isTransitioning = false;
        }
    }

    /**
     * @brief Get current active technology mode
     * @return Current mode
     */
    FutureTechMode GetCurrentMode() const {
        return m_currentMode;
    }

    /**
     * @brief Get the previous technology mode (before last transition)
     * @return Previous mode
     */
    FutureTechMode GetPreviousMode() const {
        return m_previousMode;
    }

    /**
     * @brief Check if a technology mode is available on this system
     * @param mode Mode to check
     * @return true if the mode can be activated
     */
    bool IsModeAvailable(FutureTechMode mode) const {
        switch (mode) {
            case FutureTechMode::Traditional2D:
                return true;  // Always available
            case FutureTechMode::Enhanced3D:
                return m_has3DSupport;
            case FutureTechMode::VRImmersive:
            case FutureTechMode::HybridReality:
                return m_hasVRSupport;
            case FutureTechMode::AIAssisted:
                return m_hasAIService;
            case FutureTechMode::CloudCollaborative:
                return m_hasCloudAccess;
            case FutureTechMode::FullFuture:
                return m_has3DSupport && m_hasVRSupport && m_hasAIService && m_hasCloudAccess;
            default:
                return false;
        }
    }

    /**
     * @brief Check if a mode transition is in progress
     * @return true if transitioning between modes
     */
    bool IsTransitioning() const {
        return m_isTransitioning;
    }

    /**
     * @brief Get transition progress
     * @return Progress from 0.0 to 1.0
     */
    float GetTransitionProgress() const {
        return m_transitionProgress;
    }

    /**
     * @brief Get human-readable name for a technology mode
     * @param mode Mode to get name for
     * @return Mode name string
     */
    static const char* GetModeName(FutureTechMode mode) {
        switch (mode) {
            case FutureTechMode::Traditional2D:      return "Traditional 2D";
            case FutureTechMode::Enhanced3D:          return "Enhanced 3D";
            case FutureTechMode::VRImmersive:         return "VR Immersive";
            case FutureTechMode::AIAssisted:          return "AI Assisted";
            case FutureTechMode::CloudCollaborative:  return "Cloud Collaborative";
            case FutureTechMode::HybridReality:       return "Hybrid Reality";
            case FutureTechMode::FullFuture:          return "Full Future";
            default:                                  return "Unknown";
        }
    }

private:
    FutureTechMode m_currentMode = FutureTechMode::Traditional2D;   ///< Current active mode
    FutureTechMode m_previousMode = FutureTechMode::Traditional2D;  ///< Previous mode
    FutureTechMode m_targetMode = FutureTechMode::Traditional2D;    ///< Target mode during transition

    bool m_isTransitioning = false;     ///< Whether a transition is in progress
    float m_transitionProgress = 0.0f;  ///< Transition progress (0-1)
    float m_transitionDuration = 1.0f;  ///< Current transition duration

    // Hardware/service availability flags
    bool m_has3DSupport = false;    ///< 3D spatial hardware detected
    bool m_hasVRSupport = false;    ///< VR headset detected
    bool m_hasAIService = false;    ///< AI service available
    bool m_hasCloudAccess = false;  ///< Cloud service accessible
};

} // namespace SparkEditor
