/**
 * @file SparkEngineCameraConsoleOps.cpp
 * @brief Console integration methods for SparkEngineCamera
 *
 * All Console_* methods for real-time camera parameter adjustment via console commands.
 * Split from SparkEngineCamera.cpp for maintainability.
 */

#include "SparkEngineCamera.h"
#include "SparkEngineCameraInternal.h"
#include <cmath>
#include <string>

using namespace DirectX;

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS - Full Cross-Code Hooking
// ============================================================================

void SparkEngineCamera::Console_SetFOV(float fovDegrees)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (fovDegrees >= 10.0f && fovDegrees <= 170.0f)
        {
            m_defaultFov = XMConvertToRadians(fovDegrees);
            UpdateProjectionMatrix();
            changed = true;
            LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera FOV set to ") + std::to_wstring(fovDegrees) +
                                         L" degrees via console",
                                     L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Invalid FOV value. Must be between 10 and 170 degrees", L"ERROR");
        }
    }
    if (changed)
        NotifyStateChange();
}

void SparkEngineCamera::Console_SetMouseSensitivity(float sensitivity)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (sensitivity >= 0.1f && sensitivity <= 10.0f)
        {
            m_mouseSensitivity = sensitivity;
            changed = true;
            LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Mouse sensitivity set to ") + std::to_wstring(sensitivity) +
                                         L" via console",
                                     L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Invalid sensitivity value. Must be between 0.1 and 10.0", L"ERROR");
        }
    }
    if (changed)
        NotifyStateChange();
}

void SparkEngineCamera::Console_SetInvertY(bool invert)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_invertY = invert;
    }
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Y-axis inversion ") + (invert ? L"enabled" : L"disabled") + L" via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SetMoveSpeed(float speed)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (speed >= 0.1f && speed <= 100.0f)
        {
            m_moveSpeed = speed;
            changed = true;
            LOG_TO_CONSOLE_IMMEDIATE(
                std::wstring(L"Camera movement speed set to ") + std::to_wstring(speed) + L" via console", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Invalid movement speed. Must be between 0.1 and 100.0", L"ERROR");
        }
    }
    if (changed)
        NotifyStateChange();
}

void SparkEngineCamera::Console_SetRotationSpeed(float speed)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (speed >= 0.1f && speed <= 10.0f)
        {
            m_rotationSpeed = speed;
            changed = true;
            LOG_TO_CONSOLE_IMMEDIATE(
                std::wstring(L"Camera rotation speed set to ") + std::to_wstring(speed) + L" via console", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Invalid rotation speed. Must be between 0.1 and 10.0", L"ERROR");
        }
    }
    if (changed)
        NotifyStateChange();
}

void SparkEngineCamera::Console_SetPosition(float x, float y, float z)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_position = XMFLOAT3(x, y, z);
        UpdateViewMatrix();
    }
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera position set to (") + std::to_wstring(x) + L", " +
                                 std::to_wstring(y) + L", " + std::to_wstring(z) + L") via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SetRotation(float pitch, float yaw, float roll)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_pitch = XMConvertToRadians(pitch);
        m_yaw = XMConvertToRadians(yaw);
        m_roll = XMConvertToRadians(roll);

        // Clamp pitch to valid range
        m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

        UpdateViewMatrix();
    }
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera rotation set to (") + std::to_wstring(pitch) + L", " +
                                 std::to_wstring(yaw) + L", " + std::to_wstring(roll) + L") degrees via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SetClippingPlanes(float nearPlane, float farPlane)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (nearPlane >= 0.01f && nearPlane <= 10.0f && farPlane >= 100.0f && farPlane <= 10000.0f &&
            nearPlane < farPlane)
        {
            m_nearPlane = nearPlane;
            m_farPlane = farPlane;
            UpdateProjectionMatrix();
            changed = true;
            LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera clipping planes set - Near: ") + std::to_wstring(nearPlane) +
                                         L", Far: " + std::to_wstring(farPlane) + L" via console",
                                     L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Invalid clipping plane values. Near: 0.01-10.0, Far: 100-10000, Near < Far",
                                     L"ERROR");
        }
    }
    if (changed)
        NotifyStateChange();
}

void SparkEngineCamera::Console_ResetToDefaults()
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_position = XMFLOAT3(0, 0, 0);
        m_pitch = m_yaw = m_roll = 0.0f;
        m_moveSpeed = 10.0f;
        m_rotationSpeed = 2.0f;
        m_mouseSensitivity = 1.0f;
        m_invertY = false;
        m_defaultFov = DirectX::XM_PIDIV2;
        m_zoomedFov = DirectX::XM_PIDIV2 / 2.0f;
        m_nearPlane = 0.1f;
        m_farPlane = 1000.0f;

        UpdateViewMatrix();
        UpdateProjectionMatrix();
    }
    NotifyStateChange();

    LOG_TO_CONSOLE_IMMEDIATE(L"Camera reset to default settings via console", L"SUCCESS");
}

SparkEngineCamera::CameraState SparkEngineCamera::Console_GetState() const
{
    return GetStateThreadSafe();
}

void SparkEngineCamera::Console_RegisterStateCallback(std::function<void()> callback)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_stateCallback = callback;
    }
    LOG_TO_CONSOLE_IMMEDIATE(L"Camera state callback registered", L"INFO");
}

void SparkEngineCamera::Console_LookAt(float targetX, float targetY, float targetZ)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);

        // Calculate direction from camera to target
        XMFLOAT3 target(targetX, targetY, targetZ);
        XMVECTOR direction = XMVectorSubtract(XMLoadFloat3(&target), XMLoadFloat3(&m_position));
        direction = XMVector3Normalize(direction);

        // Calculate yaw and pitch from direction vector
        XMFLOAT3 dir;
        XMStoreFloat3(&dir, direction);

        m_yaw = atan2f(dir.x, dir.z);
        m_pitch = asinf(-dir.y);

        // Clamp pitch
        m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

        UpdateViewMatrix();
    }
    NotifyStateChange();

    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera looking at (") + std::to_wstring(targetX) + L", " +
                                 std::to_wstring(targetY) + L", " + std::to_wstring(targetZ) + L") via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SmoothMoveTo(float targetX, float targetY, float targetZ, float duration)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_transitionStart = m_position;
    m_transitionEnd = XMFLOAT3(targetX, targetY, targetZ);
    m_transitionDuration = (std::max)(duration, 0.001f);
    m_transitionElapsed = 0.0f;
    m_isTransitioning = true;
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera smooth movement to (") + std::to_wstring(targetX) + L", " +
                                 std::to_wstring(targetY) + L", " + std::to_wstring(targetZ) + L") over " +
                                 std::to_wstring(duration) + L"s via console",
                             L"SUCCESS");
}
