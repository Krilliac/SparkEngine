/**
 * @file Spark3DDocking.h
 * @brief 3D Docking system (stub — not yet implemented)
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>

namespace SparkEditor {

/**
 * @brief 3D Docking system stub (simplified to avoid compilation issues)
 */
class Spark3DDocking {
public:
    /**
     * @brief Constructor
     */
    Spark3DDocking() = default;

    /**
     * @brief Destructor
     */
    ~Spark3DDocking() = default;

    /**
     * @brief Initialize 3D docking (stub)
     */
    bool Initialize() {
        return false; // 3D docking not available in stub
    }

    /**
     * @brief Shutdown 3D docking (stub)
     */
    void Shutdown() {
        // Stub implementation
    }

    /**
     * @brief Check if 3D docking is available (stub)
     */
    bool Is3DAvailable() const {
        return false;
    }
};

} // namespace SparkEditor