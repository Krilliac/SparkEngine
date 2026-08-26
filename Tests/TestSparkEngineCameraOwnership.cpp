/**
 * @file TestSparkEngineCameraOwnership.cpp
 * @brief Regression coverage for camera vector getter ownership.
 */

#include "TestFramework.h"
#include "Camera/SparkEngineCamera.h"

#include <cmath>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<const SparkEngineCamera&>().GetPosition()), DirectX::XMFLOAT3>);
static_assert(std::is_same_v<decltype(std::declval<const SparkEngineCamera&>().GetForward()), DirectX::XMFLOAT3>);

TEST(SparkEngineCamera_VectorGettersReturnStableOwnedSnapshots)
{
    SparkEngineCamera camera;
    camera.SetPosition({1.0f, 2.0f, 3.0f});

    const DirectX::XMFLOAT3 positionSnapshot = camera.GetPosition();
    const DirectX::XMFLOAT3 forwardSnapshot = camera.GetForward();

    camera.SetPosition({10.0f, 20.0f, 30.0f});
    camera.Yaw(0.25f);

    const DirectX::XMFLOAT3 currentPosition = camera.GetPosition();
    const DirectX::XMFLOAT3 currentForward = camera.GetForward();

    EXPECT_NEAR(positionSnapshot.x, 1.0f, 0.001f);
    EXPECT_NEAR(positionSnapshot.y, 2.0f, 0.001f);
    EXPECT_NEAR(positionSnapshot.z, 3.0f, 0.001f);
    EXPECT_NEAR(currentPosition.x, 10.0f, 0.001f);
    EXPECT_NEAR(currentPosition.y, 20.0f, 0.001f);
    EXPECT_NEAR(currentPosition.z, 30.0f, 0.001f);

    EXPECT_NEAR(forwardSnapshot.x, 0.0f, 0.001f);
    EXPECT_NEAR(forwardSnapshot.y, 0.0f, 0.001f);
    EXPECT_NEAR(forwardSnapshot.z, 1.0f, 0.001f);
    EXPECT_TRUE(std::abs(currentForward.x - forwardSnapshot.x) > 0.01f);
    EXPECT_TRUE(std::abs(currentForward.z - forwardSnapshot.z) > 0.01f);
}
