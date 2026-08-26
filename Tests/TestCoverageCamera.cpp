/**
 * @file TestCoverageCamera.cpp
 * @brief Production coverage for SparkEngineCamera behavior and console controls.
 */

#include "TestFramework.h"

#include "Camera/SparkEngineCamera.h"

#include <cmath>

namespace
{
    constexpr float kEpsilon = 0.001f;

    float Length(const DirectX::XMFLOAT3& value)
    {
        return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    }
} // namespace

TEST(CoverageCamera_RollPitchYawMatchesDirectXMathForwardSemantics)
{
    const auto rotation =
        DirectX::XMMatrixRotationRollPitchYaw(0.0f, DirectX::XM_PIDIV2, DirectX::XMConvertToRadians(-30.0f));
    const auto transformed = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation);
    DirectX::XMFLOAT3 forward{};
    DirectX::XMStoreFloat3(&forward, transformed);

    // DirectXMath applies roll, then pitch, then yaw for row vectors. Roll is
    // therefore around the local forward axis and cannot change this result.
    EXPECT_NEAR(forward.x, 1.0f, kEpsilon);
    EXPECT_NEAR(forward.y, 0.0f, kEpsilon);
    EXPECT_NEAR(forward.z, 0.0f, kEpsilon);
}

TEST(CoverageCamera_RealMovementRotationAndProjectionPaths)
{
    SparkEngineCamera camera;
    camera.Initialize(2.0f);

    auto state = camera.Console_GetState();
    EXPECT_NEAR(state.aspectRatio, 2.0f, kEpsilon);
    EXPECT_NEAR(state.defaultFov, 90.0f, kEpsilon);
    EXPECT_NEAR(Length(state.forward), 1.0f, kEpsilon);
    EXPECT_NEAR(Length(state.right), 1.0f, kEpsilon);
    EXPECT_NEAR(Length(state.up), 1.0f, kEpsilon);

    camera.Console_SetMoveSpeed(2.0f);
    camera.SetPosition({1.0f, 2.0f, 3.0f});
    camera.MoveForward(1.0f);
    camera.MoveRight(0.5f);
    camera.MoveUp(-1.0f);
    const auto moved = camera.GetPosition();
    EXPECT_NEAR(moved.x, 2.0f, kEpsilon);
    EXPECT_NEAR(moved.y, 0.0f, kEpsilon);
    EXPECT_NEAR(moved.z, 5.0f, kEpsilon);

    camera.Console_SetRotationSpeed(1.0f);
    camera.Console_SetMouseSensitivity(2.0f);
    camera.Console_SetInvertY(true);
    camera.Pitch(0.25f);
    camera.Yaw(-0.25f);
    camera.Roll(-0.25f);
    state = camera.Console_GetState();
    EXPECT_NEAR(state.rotation.x, DirectX::XMConvertToDegrees(-0.5f), kEpsilon);
    EXPECT_NEAR(state.rotation.y, DirectX::XMConvertToDegrees(DirectX::XM_2PI - 0.5f), kEpsilon);
    EXPECT_NEAR(state.rotation.z, DirectX::XMConvertToDegrees(DirectX::XM_2PI - 0.25f), kEpsilon);

    camera.Pitch(100.0f);
    EXPECT_NEAR(camera.GetRotation().x, -DirectX::XM_PIDIV2 + 0.01f, kEpsilon);

    DirectX::XMFLOAT4X4 normalProjection{};
    DirectX::XMFLOAT4X4 zoomProjection{};
    camera.SetZoom(false);
    DirectX::XMStoreFloat4x4(&normalProjection, camera.GetProjectionMatrix());
    camera.SetZoom(true);
    DirectX::XMStoreFloat4x4(&zoomProjection, camera.GetProjectionMatrix());
    EXPECT_GT(zoomProjection._22, normalProjection._22);
}

TEST(CoverageCamera_ConsoleValidationCallbacksLookAtAndReset)
{
    SparkEngineCamera camera;
    camera.Initialize(16.0f / 9.0f);

    int callbackCount = 0;
    SparkEngineCamera::CameraState callbackState{};
    camera.Console_RegisterStateCallback(
        [&]
        {
            ++callbackCount;
            // Deliberately re-enter the camera: callbacks must run outside its mutex.
            callbackState = camera.Console_GetState();
        });

    camera.Console_SetFOV(65.0f);
    camera.Console_SetMouseSensitivity(1.5f);
    camera.Console_SetMoveSpeed(25.0f);
    camera.Console_SetRotationSpeed(3.0f);
    camera.Console_SetClippingPlanes(0.5f, 2500.0f);
    EXPECT_EQ(callbackCount, 5);

    auto state = camera.Console_GetState();
    EXPECT_NEAR(state.defaultFov, 65.0f, kEpsilon);
    EXPECT_NEAR(state.mouseSensitivity, 1.5f, kEpsilon);
    EXPECT_NEAR(state.moveSpeed, 25.0f, kEpsilon);
    EXPECT_NEAR(state.rotationSpeed, 3.0f, kEpsilon);
    EXPECT_NEAR(state.nearPlane, 0.5f, kEpsilon);
    EXPECT_NEAR(state.farPlane, 2500.0f, kEpsilon);
    EXPECT_NEAR(callbackState.farPlane, 2500.0f, kEpsilon);

    // Rejected console values must preserve state and must not claim a change.
    camera.Console_SetFOV(9.0f);
    camera.Console_SetMouseSensitivity(10.1f);
    camera.Console_SetMoveSpeed(0.0f);
    camera.Console_SetRotationSpeed(11.0f);
    camera.Console_SetClippingPlanes(5.0f, 4.0f);
    EXPECT_EQ(callbackCount, 5);
    state = camera.Console_GetState();
    EXPECT_NEAR(state.defaultFov, 65.0f, kEpsilon);
    EXPECT_NEAR(state.moveSpeed, 25.0f, kEpsilon);
    EXPECT_NEAR(state.nearPlane, 0.5f, kEpsilon);

    camera.Console_SetPosition(2.0f, 3.0f, 4.0f);
    camera.Console_SetRotation(120.0f, 45.0f, -30.0f);
    EXPECT_EQ(callbackCount, 7);
    state = camera.Console_GetState();
    EXPECT_NEAR(state.position.x, 2.0f, kEpsilon);
    EXPECT_NEAR(state.rotation.x, DirectX::XMConvertToDegrees(DirectX::XM_PIDIV2 - 0.01f), kEpsilon);
    EXPECT_NEAR(state.rotation.y, 45.0f, kEpsilon);
    EXPECT_NEAR(state.rotation.z, -30.0f, kEpsilon);

    camera.Console_SetPosition(0.0f, 0.0f, 0.0f);
    camera.Console_LookAt(1.0f, 0.0f, 0.0f);
    state = camera.Console_GetState();
    EXPECT_NEAR(state.forward.x, 1.0f, kEpsilon);
    EXPECT_NEAR(state.forward.y, 0.0f, kEpsilon);
    EXPECT_NEAR(state.forward.z, 0.0f, kEpsilon);

    camera.Console_ResetToDefaults();
    state = camera.Console_GetState();
    EXPECT_NEAR(state.position.x, 0.0f, kEpsilon);
    EXPECT_NEAR(state.position.y, 0.0f, kEpsilon);
    EXPECT_NEAR(state.position.z, 0.0f, kEpsilon);
    EXPECT_NEAR(state.rotation.x, 0.0f, kEpsilon);
    EXPECT_NEAR(state.defaultFov, 90.0f, kEpsilon);
    EXPECT_NEAR(state.nearPlane, 0.1f, kEpsilon);
    EXPECT_NEAR(state.farPlane, 1000.0f, kEpsilon);
    EXPECT_FALSE(state.invertY);
}

TEST(CoverageCamera_SmoothMoveUsesSmoothStepAndCompletes)
{
    SparkEngineCamera camera;
    camera.Initialize(1.0f);
    camera.SetPosition({0.0f, 0.0f, 0.0f});

    int callbackCount = 0;
    camera.Console_RegisterStateCallback(
        [&]
        {
            ++callbackCount;
            (void)camera.GetPosition();
        });

    camera.Console_SmoothMoveTo(10.0f, 4.0f, -2.0f, 2.0f);
    EXPECT_TRUE(camera.IsTransitioning());
    camera.Update(1.0f);
    auto position = camera.GetPosition();
    EXPECT_NEAR(position.x, 5.0f, kEpsilon);
    EXPECT_NEAR(position.y, 2.0f, kEpsilon);
    EXPECT_NEAR(position.z, -1.0f, kEpsilon);
    EXPECT_TRUE(camera.IsTransitioning());

    camera.Update(1.0f);
    position = camera.GetPosition();
    EXPECT_NEAR(position.x, 10.0f, kEpsilon);
    EXPECT_NEAR(position.y, 4.0f, kEpsilon);
    EXPECT_NEAR(position.z, -2.0f, kEpsilon);
    EXPECT_FALSE(camera.IsTransitioning());
    EXPECT_EQ(callbackCount, 2);

    // Zero/negative durations use the documented minimum duration.
    camera.Console_SmoothMoveTo(12.0f, 4.0f, -2.0f, 0.0f);
    camera.Update(0.001f);
    EXPECT_FALSE(camera.IsTransitioning());
    EXPECT_NEAR(camera.GetPosition().x, 12.0f, kEpsilon);

    // The non-transition update path still refreshes the view matrix safely.
    camera.Update(0.0f);
}
