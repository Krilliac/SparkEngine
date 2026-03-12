/**
 * @file SparkEngineCamera.h
 * @brief First-person camera system with smooth movement and console integration
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive first-person camera implementation with
 * smooth movement, mouse look controls, zoom functionality, proper matrix
 * calculations for 3D rendering, and full console integration for real-time
 * camera parameter adjustment and debugging.
 */

#pragma once
#include "../Core/Platform.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <DirectXMath.h>
#endif                    // SPARK_PLATFORM_WINDOWS
#include <algorithm>      // std::clamp
#include <functional>     // std::function
#include <mutex>          // std::mutex
#include "Utils/Assert.h" // custom assert

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief First-person camera controller with console integration
 * 
 * The SparkEngineCamera class provides a complete first-person camera system
 * with smooth movement, mouse look controls, configurable parameters, and
 * comprehensive console integration for real-time debugging and tuning.
 * 
 * Features include:
 * - Smooth first-person movement (forward, right, up)
 * - Mouse look with pitch, yaw, and roll controls
 * - Configurable movement and rotation speeds
 * - Zoom functionality with different FOV settings
 * - Automatic view matrix updates
 * - Pitch clamping to prevent over-rotation
 * - Real-time console parameter adjustment
 * - Live camera state monitoring
 * - Thread-safe parameter access
 * 
 * @note Camera uses right-handed coordinate system with Y-up
 * @warning Initialize() must be called before any movement or matrix operations
 */
class SparkEngineCamera
{
  private:
    XMFLOAT3 m_position{0, 0, 0};          ///< Camera position in world space
    XMFLOAT3 m_forward{0, 0, 1};           ///< Camera forward direction vector
    XMFLOAT3 m_right{1, 0, 0};             ///< Camera right direction vector
    XMFLOAT3 m_up{0, 1, 0};                ///< Camera up direction vector
    float m_pitch{0}, m_yaw{0}, m_roll{0}; ///< Camera rotation angles in radians

    XMMATRIX m_viewMatrix{};       ///< Cached view transformation matrix
    XMMATRIX m_projectionMatrix{}; ///< Cached projection transformation matrix

    float m_moveSpeed{10.0f};                     ///< Movement speed in units per second
    float m_rotationSpeed{2.0f};                  ///< Rotation speed multiplier
    float m_defaultFov{DirectX::XM_PIDIV2};       ///< Default field of view (90 degrees)
    float m_zoomedFov{DirectX::XM_PIDIV2 / 2.0f}; ///< Zoomed field of view (45 degrees)
    float m_aspectRatio{16.0f / 9.0f};            ///< Screen aspect ratio

    // Console integration state
    float m_mouseSensitivity{1.0f}; ///< Mouse sensitivity multiplier
    bool m_invertY{false};          ///< Invert Y-axis for mouse look
    bool m_smoothMovement{true};    ///< Enable smooth movement interpolation
    float m_nearPlane{0.1f};        ///< Near clipping plane distance
    float m_farPlane{1000.0f};      ///< Far clipping plane distance

    // Smooth transition state
    bool m_isTransitioning = false;      ///< Whether a smooth transition is active
    XMFLOAT3 m_transitionStart{0, 0, 0}; ///< Starting position of smooth transition
    XMFLOAT3 m_transitionEnd{0, 0, 0};   ///< Target position of smooth transition
    float m_transitionDuration = 0.0f;   ///< Total transition duration in seconds
    float m_transitionElapsed = 0.0f;    ///< Elapsed time during transition

    // Console callback system
    mutable std::mutex m_stateMutex;       ///< Thread safety for state access
    std::function<void()> m_stateCallback; ///< Callback for state changes

  public:
    /**
     * @brief Default constructor
     * 
     * Initializes camera with default values. Call Initialize() before use.
     */
    SparkEngineCamera() = default;

    /**
     * @brief Default destructor
     * 
     * Uses default cleanup as all resources are managed automatically.
     */
    ~SparkEngineCamera() = default;

    /**
     * @brief Initialize the camera with projection settings
     * 
     * Sets up the camera with the specified aspect ratio and calculates
     * the initial projection matrix. Must be called before movement operations.
     * 
     * @param aspectRatio Screen width divided by height (e.g., 16/9 = 1.777)
     * @note Call this after graphics system initialization and window creation
     */
    void Initialize(float aspectRatio);

    /**
     * @brief Update camera for the current frame
     * 
     * Recalculates the view matrix based on current position and orientation.
     * Should be called once per frame after processing movement input.
     * 
     * @param deltaTime Time elapsed since last frame in seconds (currently unused)
     */
    void Update(float deltaTime);

    /**
     * @brief Move camera forward or backward
     * 
     * Moves the camera along its forward direction vector. Positive values
     * move forward, negative values move backward.
     * 
     * @param amount Distance to move (positive = forward, negative = backward)
     */
    void MoveForward(float amount);

    /**
     * @brief Move camera left or right
     * 
     * Moves the camera along its right direction vector. Positive values
     * move right, negative values move left.
     * 
     * @param amount Distance to move (positive = right, negative = left)
     */
    void MoveRight(float amount);

    /**
     * @brief Move camera up or down
     * 
     * Moves the camera along its up direction vector. Positive values
     * move up, negative values move down.
     * 
     * @param amount Distance to move (positive = up, negative = down)
     */
    void MoveUp(float amount);

    /**
     * @brief Rotate camera around X-axis (look up/down)
     * 
     * Adjusts the camera's pitch rotation. Positive values pitch up,
     * negative values pitch down. Pitch is automatically clamped to
     * prevent over-rotation.
     * 
     * @param angle Pitch angle to add in radians
     */
    void Pitch(float angle);

    /**
     * @brief Rotate camera around Y-axis (look left/right)
     * 
     * Adjusts the camera's yaw rotation. Positive values yaw right,
     * negative values yaw left.
     * 
     * @param angle Yaw angle to add in radians
     */
    void Yaw(float angle);

    /**
     * @brief Rotate camera around Z-axis (tilt left/right)
     * 
     * Adjusts the camera's roll rotation. Positive values roll clockwise,
     * negative values roll counter-clockwise.
     * 
     * @param angle Roll angle to add in radians
     */
    void Roll(float angle);

    /**
     * @brief Toggle between normal and zoomed field of view
     * 
     * Switches between default and zoomed FOV settings, useful for
     * aiming mechanics or telescope effects.
     * 
     * @param enabled true to enable zoom (narrow FOV), false for normal FOV
     */
    void SetZoom(bool enabled);

    /**
     * @brief Directly set the camera position
     * 
     * Immediately positions the camera at the specified world coordinates
     * and updates the view matrix.
     * 
     * @param pos New position in world coordinates
     */
    void SetPosition(const XMFLOAT3& pos)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_position = pos;
        UpdateViewMatrix();
        NotifyStateChange();
    }

    /**
     * @brief Get the current view transformation matrix
     * @return 4x4 view matrix for rendering transformations
     */
    const XMMATRIX& GetViewMatrix() const { return m_viewMatrix; }

    /**
     * @brief Get the current projection transformation matrix
     * @return 4x4 projection matrix for rendering transformations
     */
    const XMMATRIX& GetProjectionMatrix() const { return m_projectionMatrix; }

    /**
     * @brief Get the current camera position
     * @return Position vector in world coordinates
     */
    const XMFLOAT3& GetPosition() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_position;
    }

    /**
     * @brief Get the current camera forward direction
     * @return Normalized forward direction vector
     */
    const XMFLOAT3& GetForward() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_forward;
    }

    /**
     * @brief Get the current camera rotation (pitch, yaw, roll)
     * @return Rotation vector in radians
     */
    XMFLOAT3 GetRotation() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return XMFLOAT3(m_pitch, m_yaw, m_roll);
    }

    // ============================================================================
    // CONSOLE INTEGRATION METHODS - Full Cross-Code Hooking
    // ============================================================================

    /**
     * @brief Camera state structure for console integration
     */
    struct CameraState
    {
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 forward, right, up;
        float moveSpeed, rotationSpeed, mouseSensitivity;
        float defaultFov, zoomedFov, currentFov;
        float aspectRatio, nearPlane, farPlane;
        bool invertY, smoothMovement, isZoomed;
    };

    /**
     * @brief Set field of view (console integration)
     * @param fovDegrees New FOV in degrees (10-170)
     */
    void Console_SetFOV(float fovDegrees);

    /**
     * @brief Set mouse sensitivity (console integration)
     * @param sensitivity New sensitivity multiplier (0.1-10.0)
     */
    void Console_SetMouseSensitivity(float sensitivity);

    /**
     * @brief Set Y-axis inversion (console integration)
     * @param invert true to invert Y-axis, false for normal
     */
    void Console_SetInvertY(bool invert);

    /**
     * @brief Set movement speed (console integration)
     * @param speed New movement speed (0.1-100.0)
     */
    void Console_SetMoveSpeed(float speed);

    /**
     * @brief Set rotation speed (console integration)
     * @param speed New rotation speed multiplier (0.1-10.0)
     */
    void Console_SetRotationSpeed(float speed);

    /**
     * @brief Set camera position directly (console integration)
     * @param x New X coordinate
     * @param y New Y coordinate
     * @param z New Z coordinate
     */
    void Console_SetPosition(float x, float y, float z);

    /**
     * @brief Set camera rotation directly (console integration)
     * @param pitch New pitch in degrees
     * @param yaw New yaw in degrees
     * @param roll New roll in degrees
     */
    void Console_SetRotation(float pitch, float yaw, float roll);

    /**
     * @brief Set near and far clipping planes (console integration)
     * @param nearPlane Near clipping distance (0.01-10.0)
     * @param farPlane Far clipping distance (100-10000)
     */
    void Console_SetClippingPlanes(float nearPlane, float farPlane);

    /**
     * @brief Reset camera to default settings (console integration)
     */
    void Console_ResetToDefaults();

    /**
     * @brief Get comprehensive camera state (console integration)
     * @return CameraState structure with all current values
     */
    CameraState Console_GetState() const;

    /**
     * @brief Register console state change callback
     * @param callback Function to call when camera state changes
     */
    void Console_RegisterStateCallback(std::function<void()> callback);

    /**
     * @brief Look at a specific point (console integration)
     * @param targetX Target X coordinate
     * @param targetY Target Y coordinate
     * @param targetZ Target Z coordinate
     */
    void Console_LookAt(float targetX, float targetY, float targetZ);

    /**
     * @brief Smooth transition to position (console integration)
     * @param targetX Target X coordinate
     * @param targetY Target Y coordinate
     * @param targetZ Target Z coordinate
     * @param duration Transition time in seconds
     */
    void Console_SmoothMoveTo(float targetX, float targetY, float targetZ, float duration);

    /**
     * @brief Check whether a smooth transition is currently in progress
     * @return true if the camera is transitioning, false otherwise
     */
    bool IsTransitioning() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_isTransitioning;
    }

  private:
    /**
     * @brief Recalculate the view matrix from current transform
     * 
     * Updates the cached view matrix based on current position and rotation.
     * Called automatically when transform properties change.
     */
    void UpdateViewMatrix();

    /**
     * @brief Recalculate the projection matrix
     * 
     * Updates the cached projection matrix based on current FOV, aspect ratio,
     * and clipping plane settings.
     */
    void UpdateProjectionMatrix();

    /**
     * @brief Notify console of state change
     */
    void NotifyStateChange();

    /**
     * @brief Thread-safe state access helper
     * @return Current camera state with thread safety
     */
    CameraState GetStateThreadSafe() const;
};
