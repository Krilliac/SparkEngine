/**
 * @file Spark3DDocking.h
 * @brief 3D Docking system for spatial panel arrangement
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides 3D spatial docking capabilities for VR/AR environments.
 * Falls back gracefully when 3D hardware is not available.
 */

#pragma once

#include <string>
#include <vector>

namespace SparkEditor {

/**
 * @brief 3D Docking system with hardware detection and state management
 */
class Spark3DDocking {
public:
    Spark3DDocking() = default;
    ~Spark3DDocking() = default;

    /**
     * @brief Initialize 3D docking system
     * @return true if initialization succeeded (3D may still be unavailable)
     */
    bool Initialize() {
        if (m_isInitialized) return true;

        // Detect 3D hardware availability
        m_is3DAvailable = Detect3DHardware();
        m_isInitialized = true;
        return true;
    }

    /**
     * @brief Shutdown 3D docking system
     */
    void Shutdown() {
        m_isInitialized = false;
        m_is3DAvailable = false;
        m_isActive = false;
    }

    /**
     * @brief Check if 3D docking hardware is available
     * @return true if 3D spatial docking can be used
     */
    bool Is3DAvailable() const {
        return m_is3DAvailable;
    }

    /**
     * @brief Check if 3D docking is currently active
     * @return true if 3D mode is active
     */
    bool IsActive() const {
        return m_isActive;
    }

    /**
     * @brief Enable or disable 3D docking mode
     * @param active Whether to activate 3D mode
     * @return true if mode was changed successfully
     */
    bool SetActive(bool active) {
        if (active && !m_is3DAvailable) return false;
        m_isActive = active;
        return true;
    }

    /**
     * @brief Check if system is initialized
     * @return true if Initialize() has been called
     */
    bool IsInitialized() const {
        return m_isInitialized;
    }

private:
    /**
     * @brief Detect 3D/VR hardware availability
     * @return true if compatible hardware is detected
     */
    bool Detect3DHardware() {
        // 3D spatial docking requires VR/AR hardware
        // Currently no hardware detection implemented - returns false
        // When VR SDKs are integrated, this will check for HMD presence
        return false;
    }

    bool m_isInitialized = false;   ///< Whether system has been initialized
    bool m_is3DAvailable = false;   ///< Whether 3D hardware is available
    bool m_isActive = false;        ///< Whether 3D mode is currently active
};

} // namespace SparkEditor
