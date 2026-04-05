#include "SparkEngineCamera.h"
#include "../Core/Platform.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cmath>
#include <iostream>
#include <chrono>
#include <string>

using namespace DirectX;

// **FIXED: Corrected logging macros to handle string conversion properly**
#undef LOG_TO_CONSOLE_RATE_LIMITED
#undef LOG_TO_CONSOLE
#undef LOG_TO_CONSOLE_IMMEDIATE
#define LOG_TO_CONSOLE_RATE_LIMITED(msg, type)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        static auto lastLogTime = std::chrono::steady_clock::now();                                                    \
        static int logCounter = 0;                                                                                     \
        auto now = std::chrono::steady_clock::now();                                                                   \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count();                    \
        if (elapsed >= 10 || logCounter < 1)                                                                           \
        {                                                                                                              \
            std::wstring wmsg = msg;                                                                                   \
            std::wstring wtype = type;                                                                                 \
            std::string smsg(wmsg.begin(), wmsg.end());                                                                \
            std::string stype(wtype.begin(), wtype.end());                                                             \
            Spark::SimpleConsole::GetInstance().Log(smsg, stype);                                                      \
            if (elapsed >= 10)                                                                                         \
            {                                                                                                          \
                lastLogTime = now;                                                                                     \
                logCounter = 0;                                                                                        \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                logCounter++;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define LOG_TO_CONSOLE(msg, type) LOG_TO_CONSOLE_RATE_LIMITED(msg, type)
#define LOG_TO_CONSOLE_IMMEDIATE(msg, type)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        std::wstring wmsg = msg;                                                                                       \
        std::wstring wtype = type;                                                                                     \
        std::string smsg(wmsg.begin(), wmsg.end());                                                                    \
        std::string stype(wtype.begin(), wtype.end());                                                                 \
        Spark::SimpleConsole::GetInstance().Log(smsg, stype);                                                          \
    } while (0)

void SparkEngineCamera::Initialize(float aspectRatio)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SparkEngineCamera::Initialize called. aspectRatio=%f", aspectRatio);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, aspectRatio > 0.0f, "Aspect ratio must be positive");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_aspectRatio = aspectRatio;
    UpdateProjectionMatrix();
    UpdateViewMatrix();
    NotifyStateChange();

    LOG_TO_CONSOLE_IMMEDIATE(L"Camera initialized with aspect ratio.", L"INFO");
}

void SparkEngineCamera::UpdateViewMatrix()
{
    // **FIXED: Remove per-frame logging completely**
    // Build rotation matrix
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, m_roll);

    // Transform basis vectors
    XMVECTOR fb = XMVector3TransformCoord(XMVectorSet(0, 0, 1, 0), rot);
    XMVECTOR rb = XMVector3TransformCoord(XMVectorSet(1, 0, 0, 0), rot);
    XMVECTOR ub = XMVector3TransformCoord(XMVectorSet(0, 1, 0, 0), rot);

    // Store updated basis
    XMStoreFloat3(&m_forward, fb);
    XMStoreFloat3(&m_right, rb);
    XMStoreFloat3(&m_up, ub);

    // Compute view matrix
    XMVECTOR pos = XMLoadFloat3(&m_position);
    m_viewMatrix = XMMatrixLookAtLH(pos, pos + fb, ub);
}

void SparkEngineCamera::UpdateProjectionMatrix()
{
    // **NEW: Separate projection matrix update method**
    m_projectionMatrix = XMMatrixPerspectiveFovLH(m_defaultFov, m_aspectRatio, m_nearPlane, m_farPlane);
}

void SparkEngineCamera::Update(float deltaTime)
{
    // **FIXED: Remove per-frame logging completely**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, deltaTime >= 0.0f && std::isfinite(deltaTime),
                      "Camera Update deltaTime must be non-negative and finite");

    // Process smooth transition if active
    if (m_isTransitioning)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_transitionElapsed += deltaTime;
        float t = std::clamp(m_transitionElapsed / m_transitionDuration, 0.0f, 1.0f);

        // SmoothStep for natural-looking acceleration and deceleration
        float smoothT = t * t * (3.0f - 2.0f * t);

        // Lerp position
        XMVECTOR start = XMLoadFloat3(&m_transitionStart);
        XMVECTOR end = XMLoadFloat3(&m_transitionEnd);
        XMStoreFloat3(&m_position, XMVectorLerp(start, end, smoothT));

        if (t >= 1.0f)
        {
            m_isTransitioning = false;
        }

        NotifyStateChange();
    }

    UpdateViewMatrix();
}

void SparkEngineCamera::MoveForward(float amount)
{
    // **FIXED: Remove per-frame logging completely**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(amount), "Move amount must be finite");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    XMVECTOR p = XMLoadFloat3(&m_position);
    XMVECTOR f = XMLoadFloat3(&m_forward);
    p = XMVectorAdd(p, XMVectorScale(f, amount * m_moveSpeed));
    XMStoreFloat3(&m_position, p);
    UpdateViewMatrix();
}

void SparkEngineCamera::MoveRight(float amount)
{
    // **FIXED: Remove per-frame logging completely**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(amount), "Move amount must be finite");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    XMVECTOR p = XMLoadFloat3(&m_position);
    XMVECTOR r = XMLoadFloat3(&m_right);
    p = XMVectorAdd(p, XMVectorScale(r, amount * m_moveSpeed));
    XMStoreFloat3(&m_position, p);
    UpdateViewMatrix();
}

void SparkEngineCamera::MoveUp(float amount)
{
    // **FIXED: Remove per-frame logging completely**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(amount), "Move amount must be finite");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    XMVECTOR p = XMLoadFloat3(&m_position);
    XMVECTOR u = XMLoadFloat3(&m_up);
    p = XMVectorAdd(p, XMVectorScale(u, amount * m_moveSpeed));
    XMStoreFloat3(&m_position, p);
    UpdateViewMatrix();
}

void SparkEngineCamera::Pitch(float angle)
{
    // **ENHANCED: Apply mouse sensitivity and Y-inversion from console**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(angle), "Angle must be finite");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    float adjustedAngle = angle * m_rotationSpeed * m_mouseSensitivity;
    if (m_invertY)
    {
        adjustedAngle = -adjustedAngle;
    }

    m_pitch = std::clamp(m_pitch + adjustedAngle, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
    UpdateViewMatrix();
    NotifyStateChange();
}

void SparkEngineCamera::Yaw(float angle)
{
    // **ENHANCED: Apply mouse sensitivity from console**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(angle), "Angle must be finite");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    float adjustedAngle = angle * m_rotationSpeed * m_mouseSensitivity;

    m_yaw += adjustedAngle;
    if (m_yaw > XM_2PI)
        m_yaw -= XM_2PI;
    if (m_yaw < 0.0f)
        m_yaw += XM_2PI;
    UpdateViewMatrix();
    NotifyStateChange();
}

void SparkEngineCamera::Roll(float angle)
{
    LOG_TO_CONSOLE(std::wstring(L"SparkEngineCamera::Roll called. angle=") + std::to_wstring(angle), L"OPERATION");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(angle), "Roll angle must be finite");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_roll += angle * m_rotationSpeed;
    if (m_roll > XM_2PI)
        m_roll -= XM_2PI;
    if (m_roll < 0.0f)
        m_roll += XM_2PI;
    UpdateViewMatrix();
    NotifyStateChange();
}

void SparkEngineCamera::SetZoom(bool enabled)
{
    LOG_TO_CONSOLE(std::wstring(L"SparkEngineCamera::SetZoom called. enabled=") + std::to_wstring(enabled),
                   L"OPERATION");

    std::lock_guard<std::mutex> lock(m_stateMutex);
    // Pick FOV
    float fov = enabled ? m_zoomedFov : m_defaultFov;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, fov > 0.0f && fov < XM_PI, "Invalid FOV");

    m_projectionMatrix = XMMatrixPerspectiveFovLH(fov, m_aspectRatio, m_nearPlane, m_farPlane);
    NotifyStateChange();

    LOG_TO_CONSOLE(L"Camera zoom set.", L"INFO");
}

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS - Full Cross-Code Hooking
// ============================================================================

void SparkEngineCamera::Console_SetFOV(float fovDegrees)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (fovDegrees >= 10.0f && fovDegrees <= 170.0f)
    {
        m_defaultFov = XMConvertToRadians(fovDegrees);
        UpdateProjectionMatrix();
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(
            std::wstring(L"Camera FOV set to ") + std::to_wstring(fovDegrees) + L" degrees via console", L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid FOV value. Must be between 10 and 170 degrees", L"ERROR");
    }
}

void SparkEngineCamera::Console_SetMouseSensitivity(float sensitivity)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (sensitivity >= 0.1f && sensitivity <= 10.0f)
    {
        m_mouseSensitivity = sensitivity;
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(
            std::wstring(L"Mouse sensitivity set to ") + std::to_wstring(sensitivity) + L" via console", L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid sensitivity value. Must be between 0.1 and 10.0", L"ERROR");
    }
}

void SparkEngineCamera::Console_SetInvertY(bool invert)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_invertY = invert;
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Y-axis inversion ") + (invert ? L"enabled" : L"disabled") + L" via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SetMoveSpeed(float speed)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (speed >= 0.1f && speed <= 100.0f)
    {
        m_moveSpeed = speed;
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(
            std::wstring(L"Camera movement speed set to ") + std::to_wstring(speed) + L" via console", L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid movement speed. Must be between 0.1 and 100.0", L"ERROR");
    }
}

void SparkEngineCamera::Console_SetRotationSpeed(float speed)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (speed >= 0.1f && speed <= 10.0f)
    {
        m_rotationSpeed = speed;
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(
            std::wstring(L"Camera rotation speed set to ") + std::to_wstring(speed) + L" via console", L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid rotation speed. Must be between 0.1 and 10.0", L"ERROR");
    }
}

void SparkEngineCamera::Console_SetPosition(float x, float y, float z)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_position = XMFLOAT3(x, y, z);
    UpdateViewMatrix();
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera position set to (") + std::to_wstring(x) + L", " +
                                 std::to_wstring(y) + L", " + std::to_wstring(z) + L") via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SetRotation(float pitch, float yaw, float roll)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_pitch = XMConvertToRadians(pitch);
    m_yaw = XMConvertToRadians(yaw);
    m_roll = XMConvertToRadians(roll);

    // Clamp pitch to valid range
    m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

    UpdateViewMatrix();
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Camera rotation set to (") + std::to_wstring(pitch) + L", " +
                                 std::to_wstring(yaw) + L", " + std::to_wstring(roll) + L") degrees via console",
                             L"SUCCESS");
}

void SparkEngineCamera::Console_SetClippingPlanes(float nearPlane, float farPlane)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (nearPlane >= 0.01f && nearPlane <= 10.0f && farPlane >= 100.0f && farPlane <= 10000.0f && nearPlane < farPlane)
    {
        m_nearPlane = nearPlane;
        m_farPlane = farPlane;
        UpdateProjectionMatrix();
        NotifyStateChange();
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

void SparkEngineCamera::Console_ResetToDefaults()
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
    NotifyStateChange();

    LOG_TO_CONSOLE_IMMEDIATE(L"Camera reset to default settings via console", L"SUCCESS");
}

SparkEngineCamera::CameraState SparkEngineCamera::Console_GetState() const
{
    return GetStateThreadSafe();
}

void SparkEngineCamera::Console_RegisterStateCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stateCallback = callback;
    LOG_TO_CONSOLE_IMMEDIATE(L"Camera state callback registered", L"INFO");
}

void SparkEngineCamera::Console_LookAt(float targetX, float targetY, float targetZ)
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

void SparkEngineCamera::NotifyStateChange()
{
    if (m_stateCallback)
    {
        m_stateCallback();
    }
}

SparkEngineCamera::CameraState SparkEngineCamera::GetStateThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    CameraState state;
    state.position = m_position;
    state.rotation = XMFLOAT3(XMConvertToDegrees(m_pitch), XMConvertToDegrees(m_yaw), XMConvertToDegrees(m_roll));
    state.forward = m_forward;
    state.right = m_right;
    state.up = m_up;
    state.moveSpeed = m_moveSpeed;
    state.rotationSpeed = m_rotationSpeed;
    state.mouseSensitivity = m_mouseSensitivity;
    state.defaultFov = XMConvertToDegrees(m_defaultFov);
    state.zoomedFov = XMConvertToDegrees(m_zoomedFov);
    state.currentFov = XMConvertToDegrees(m_defaultFov); // Simplified for now
    state.aspectRatio = m_aspectRatio;
    state.nearPlane = m_nearPlane;
    state.farPlane = m_farPlane;
    state.invertY = m_invertY;
    state.smoothMovement = m_smoothMovement;
    state.isZoomed = false; // Would need to track zoom state
    return state;
}