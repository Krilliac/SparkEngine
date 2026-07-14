// MathUtilsExtended.cpp
#include "../Core/Platform.h"
#include "MathUtils.h"
#include "Utils/Assert.h"
#include "Validate.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <random>
#include <cmath>
#include <iostream>

using namespace DirectX;

// Static random number generator
static std::random_device s_randomDevice;
static std::mt19937 s_randomGenerator(s_randomDevice());
static bool s_randomInitialized = false;

// =============================================================================
// RANDOM NUMBER GENERATION
// =============================================================================

void MathUtilsExtended::InitializeRandom()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    if (!s_randomInitialized)
    {
        s_randomGenerator.seed(s_randomDevice());
        s_randomInitialized = true;
    }
}

float MathUtilsExtended::RandomFloat(float min, float max)
{
    std::wcout << L"[OPERATION] MathUtils::RandomFloat called. min=" << min << L" max=" << max << std::endl;
    float result = min + static_cast<float>(rand()) / (static_cast<float>(RAND_MAX) / (max - min));
    std::wcout << L"[INFO] MathUtils::RandomFloat result=" << result << std::endl;
    return result;
}

int MathUtilsExtended::RandomInt(int min, int max)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, min <= max, "RandomInt min must be <= max");
    InitializeRandom();
    std::uniform_int_distribution<int> distribution(min, max);
    return distribution(s_randomGenerator);
}

XMFLOAT3 MathUtilsExtended::RandomDirection()
{
    float theta = RandomFloat(0.0f, MathUtils::TWO_PI);
    float phi = RandomFloat(-MathUtils::HALF_PI, MathUtils::HALF_PI);
    float cosTheta = cosf(theta), sinTheta = sinf(theta);
    float cosPhi = cosf(phi), sinPhi = sinf(phi);
    return XMFLOAT3(cosPhi * cosTheta, sinPhi, cosPhi * sinTheta);
}

XMFLOAT3 MathUtilsExtended::RandomPointInSphere(float radius)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, radius >= 0.0f, "Sphere radius must be non-negative");
    XMFLOAT3 point{};
    float lengthSq;
    do
    {
        point.x = RandomFloat(-1.0f, 1.0f);
        point.y = RandomFloat(-1.0f, 1.0f);
        point.z = RandomFloat(-1.0f, 1.0f);
        lengthSq = MathUtils::LengthSquared(point);
    } while (lengthSq > 1.0f);
    return MathUtils::Multiply(point, radius);
}

// =============================================================================
// MATRIX UTILITIES
// =============================================================================

XMMATRIX MathUtilsExtended::CreateLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up)
{
    return XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMLoadFloat3(&up));
}

XMMATRIX MathUtilsExtended::CreatePerspective(float fovY, float aspectRatio, float nearPlane, float farPlane)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, fovY > 0 && aspectRatio > 0 && nearPlane > 0 && farPlane > nearPlane,
                      "Invalid perspective parameters");
    return XMMatrixPerspectiveFovLH(fovY, aspectRatio, nearPlane, farPlane);
}

XMMATRIX MathUtilsExtended::CreateOrthographic(float width, float height, float nearPlane, float farPlane)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, width > 0 && height > 0 && farPlane > nearPlane,
                      "Invalid orthographic parameters");
    return XMMatrixOrthographicLH(width, height, nearPlane, farPlane);
}

// =============================================================================
// COLLISION UTILITIES
// =============================================================================

bool MathUtilsExtended::PointInSphere(const XMFLOAT3& point, const XMFLOAT3& sphereCenter, float sphereRadius)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, sphereRadius >= 0.0f, "Sphere radius must be non-negative");
    return MathUtils::DistanceSquared(point, sphereCenter) <= (sphereRadius * sphereRadius);
}

bool MathUtilsExtended::PointInBox(const XMFLOAT3& point, const XMFLOAT3& boxMin, const XMFLOAT3& boxMax)
{
    return point.x >= boxMin.x && point.x <= boxMax.x && point.y >= boxMin.y && point.y <= boxMax.y &&
           point.z >= boxMin.z && point.z <= boxMax.z;
}

// =============================================================================
// EASING FUNCTIONS
// =============================================================================

float MathUtilsExtended::EaseInQuad(float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "EaseInQuad t must be finite");
    return t * t;
}

float MathUtilsExtended::EaseOutQuad(float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "EaseOutQuad t must be finite");
    return 1.0f - (1.0f - t) * (1.0f - t);
}

float MathUtilsExtended::EaseInOutQuad(float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "EaseInOutQuad t must be finite");
    if (t < 0.5f)
        return 2.0f * t * t;
    else
    {
        float f = (1.0f - t);
        return 1.0f - 2.0f * f * f;
    }
}

float MathUtilsExtended::EaseInCubic(float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "EaseInCubic t must be finite");
    return t * t * t;
}

float MathUtilsExtended::EaseOutCubic(float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "EaseOutCubic t must be finite");
    float f = (1.0f - t);
    return 1.0f - f * f * f;
}

float MathUtilsExtended::EaseInOutCubic(float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "EaseInOutCubic t must be finite");
    if (t < 0.5f)
        return 4.0f * t * t * t;
    else
    {
        float f = 2.0f * t - 2.0f;
        return 1.0f + f * f * f * 0.5f;
    }
}
