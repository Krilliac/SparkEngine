/**
 * @file SparkEngineCamera.cpp
 * @brief Core camera movement, rotation, and matrix update implementation
 *
 * Movement, rotation, zoom, transition, and thread-safe state access methods.
 * The Console_* integration methods live in SparkEngineCameraConsoleOps.cpp;
 * shared logging helpers live in SparkEngineCameraInternal.h.
 */

#include "SparkEngineCamera.h"
#include "../Core/Platform.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "SparkEngineCameraInternal.h"
#include <cmath>
#include <string>

using namespace DirectX;

void SparkEngineCamera::Initialize(float aspectRatio)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SparkEngineCamera::Initialize called. aspectRatio=%f", aspectRatio);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, aspectRatio > 0.0f, "Aspect ratio must be positive");

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_aspectRatio = aspectRatio;
        UpdateProjectionMatrix();
        UpdateViewMatrix();
    }
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

    // Process smooth transition if active. m_isTransitioning is written under
    // m_stateMutex, so read it under the lock too (an unlocked pre-check would be a
    // data race and could observe a torn transition start).
    bool didTransition = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_isTransitioning)
        {
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
            didTransition = true;
        }
    }

    // Fire the callback outside the lock (see NotifyStateChange).
    if (didTransition)
    {
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

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        float adjustedAngle = angle * m_rotationSpeed * m_mouseSensitivity;
        if (m_invertY)
        {
            adjustedAngle = -adjustedAngle;
        }

        m_pitch = std::clamp(m_pitch + adjustedAngle, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
        UpdateViewMatrix();
    }
    NotifyStateChange();
}

void SparkEngineCamera::Yaw(float angle)
{
    // **ENHANCED: Apply mouse sensitivity from console**
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(angle), "Angle must be finite");

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        float adjustedAngle = angle * m_rotationSpeed * m_mouseSensitivity;

        // Wrap into [0, 2*PI). std::fmod handles deltas larger than 2*PI in a single
        // call (a single subtraction would leave the angle out of range).
        m_yaw = std::fmod(m_yaw + adjustedAngle, XM_2PI);
        if (m_yaw < 0.0f)
            m_yaw += XM_2PI;
        UpdateViewMatrix();
    }
    NotifyStateChange();
}

void SparkEngineCamera::Roll(float angle)
{
    LOG_TO_CONSOLE(std::wstring(L"SparkEngineCamera::Roll called. angle=") + std::to_wstring(angle), L"OPERATION");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, std::isfinite(angle), "Roll angle must be finite");

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Wrap into [0, 2*PI); std::fmod handles multi-revolution deltas.
        m_roll = std::fmod(m_roll + angle * m_rotationSpeed, XM_2PI);
        if (m_roll < 0.0f)
            m_roll += XM_2PI;
        UpdateViewMatrix();
    }
    NotifyStateChange();
}

void SparkEngineCamera::SetZoom(bool enabled)
{
    LOG_TO_CONSOLE(std::wstring(L"SparkEngineCamera::SetZoom called. enabled=") + std::to_wstring(enabled),
                   L"OPERATION");

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Pick FOV
        float fov = enabled ? m_zoomedFov : m_defaultFov;
        SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, fov > 0.0f && fov < XM_PI, "Invalid FOV");

        m_projectionMatrix = XMMatrixPerspectiveFovLH(fov, m_aspectRatio, m_nearPlane, m_farPlane);
    }
    NotifyStateChange();

    LOG_TO_CONSOLE(L"Camera zoom set.", L"INFO");
}

void SparkEngineCamera::NotifyStateChange()
{
    // Snapshot the callback under the lock, then invoke it OUTSIDE the lock.
    // The callback is arbitrary client code that commonly re-enters the camera
    // (e.g. Console_GetState -> GetStateThreadSafe re-locks m_stateMutex); firing
    // it while holding the non-recursive m_stateMutex would self-deadlock.
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        callback = m_stateCallback;
    }
    if (callback)
    {
        callback();
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
