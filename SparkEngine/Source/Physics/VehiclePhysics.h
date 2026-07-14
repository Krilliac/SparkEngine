/**
 * @file VehiclePhysics.h
 * @brief Jolt vehicle physics wrapper for wheeled, tracked, and motorcycle vehicles
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps Jolt's VehicleConstraint system to provide game-ready vehicle physics
 * with engine simulation, transmission, suspension, and wheel friction.
 *
 * @see PhysicsSystem.h, PhysicsTypes.h
 */

#pragma once

#include "../Core/Platform.h"
#include "PhysicsTypes.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>
#include <vector>

class PhysicsSystem;
class PhysicsBody;

/**
 * @class VehiclePhysics
 * @brief Game-ready vehicle physics using Jolt's VehicleConstraint.
 *
 * Supports three vehicle types:
 * - **Wheeled** (cars, trucks, APCs) with configurable differentials
 * - **Tracked** (tanks, bulldozers) with track-based steering
 * - **Motorcycle** (two-wheeled) with lean stabilization
 *
 * ### Usage
 * @code
 *   VehicleDesc desc;
 *   desc.type = PhysicsVehicleType::Wheeled;
 *   // Add 4 wheels (FL, FR, RL, RR)
 *   VehicleWheelDesc fl; fl.position = {-0.8f, -0.3f, 1.4f}; fl.maxSteerAngle = 0.5f;
 *   desc.wheels = {fl, fr, rl, rr};
 *   auto vehicle = physics.CreateVehicle(carBody, desc);
 *
 *   // Each frame:
 *   vehicle->SetInput(throttle, brake, steerAngle, handbrake);
 * @endcode
 */
class VehiclePhysics
{
  public:
    VehiclePhysics(PhysicsSystem* physicsSystem, std::shared_ptr<PhysicsBody> body, const VehicleDesc& desc);
    ~VehiclePhysics();

    // Non-copyable
    VehiclePhysics(const VehiclePhysics&) = delete;
    VehiclePhysics& operator=(const VehiclePhysics&) = delete;

    // =========================================================================
    // Input
    // =========================================================================

    /**
     * @brief Set vehicle control inputs for the current frame.
     * @param throttle  Throttle [0-1]
     * @param brake     Brake [0-1]
     * @param steerAngle Steering angle in radians (negative = left, positive = right)
     * @param handbrake Handbrake [0-1]
     */
    void SetInput(float throttle, float brake, float steerAngle, float handbrake);

    // =========================================================================
    // Vehicle state
    // =========================================================================

    /** @brief Get current engine RPM. */
    float GetEngineRPM() const;

    /** @brief Get current transmission gear (-1 = reverse, 0 = neutral, 1+ = forward). */
    int GetCurrentGear() const;

    /** @brief Get current vehicle speed in m/s. */
    float GetSpeed() const;

    /** @brief Get number of wheels on this vehicle. */
    uint32_t GetWheelCount() const;

    // =========================================================================
    // Per-wheel state
    // =========================================================================

    /** @brief Get wheel contact position in world space. */
    XMFLOAT3 GetWheelContactPosition(uint32_t wheelIndex) const;

    /** @brief Get wheel contact normal in world space. */
    XMFLOAT3 GetWheelContactNormal(uint32_t wheelIndex) const;

    /** @brief Get current suspension length for a wheel. */
    float GetWheelSuspensionLength(uint32_t wheelIndex) const;

    /** @brief Check if a wheel is touching the ground. */
    bool IsWheelOnGround(uint32_t wheelIndex) const;

    /** @brief Get wheel rotation angle (for visual spinning). */
    float GetWheelRotationAngle(uint32_t wheelIndex) const;

    /** @brief Get wheel steering angle (for visual turning). */
    float GetWheelSteerAngle(uint32_t wheelIndex) const;

    /** @brief Get the underlying physics body. */
    std::shared_ptr<PhysicsBody> GetBody() const { return m_body; }

  private:
    PhysicsSystem* m_physicsSystem;
    std::shared_ptr<PhysicsBody> m_body;
    VehicleDesc m_desc;
    void* m_joltConstraint = nullptr; ///< Opaque pointer to JPH::VehicleConstraint
    void* m_joltController = nullptr; ///< Opaque pointer to JPH::VehicleController
};
