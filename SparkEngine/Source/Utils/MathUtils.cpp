#include "../Core/Platform.h"
// MathUtils.cpp
#include "MathUtils.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <random>
#include <cmath>
#include <iostream>

using namespace DirectX;

// Static constant definitions
const float MathUtils::PI = 3.14159265359f;
const float MathUtils::TWO_PI = 6.28318530718f;
const float MathUtils::HALF_PI = 1.57079632679f;
const float MathUtils::DEG_TO_RAD = 0.01745329252f;
const float MathUtils::RAD_TO_DEG = 57.2957795131f;

// Static random number generator
static std::random_device s_randomDevice;
static std::mt19937        s_randomGenerator(s_randomDevice());
static bool                s_randomInitialized = false;

// =============================================================================
// ANGLE UTILITIES
// =============================================================================

float MathUtils::DegreesToRadians(float degrees)
{
    ASSERT_MSG(std::isfinite(degrees), "Degrees must be finite");
    return degrees * DEG_TO_RAD;
}

float MathUtils::RadiansToDegrees(float radians)
{
    ASSERT_MSG(std::isfinite(radians), "Radians must be finite");
    return radians * RAD_TO_DEG;
}

float MathUtils::WrapAngle(float angle)
{
    ASSERT_MSG(std::isfinite(angle), "Angle must be finite");
    while (angle > PI)  angle -= TWO_PI;
    while (angle < -PI) angle += TWO_PI;
    return angle;
}

float MathUtils::NormalizeAngle(float angle)
{
    ASSERT_MSG(std::isfinite(angle), "Angle must be finite");
    while (angle < 0.0f)      angle += TWO_PI;
    while (angle >= TWO_PI)   angle -= TWO_PI;
    return angle;
}

// =============================================================================
// INTERPOLATION FUNCTIONS
// =============================================================================

float MathUtils::Lerp(float a, float b, float t)
{
    ASSERT_MSG(std::isfinite(a) && std::isfinite(b) && std::isfinite(t),
        "Lerp inputs must be finite");
    return a + t * (b - a);
}

XMFLOAT3 MathUtils::Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    ASSERT_MSG(std::isfinite(t), "Lerp t must be finite");
    return XMFLOAT3(
        Lerp(a.x, b.x, t),
        Lerp(a.y, b.y, t),
        Lerp(a.z, b.z, t)
    );
}

float MathUtils::SmoothStep(float a, float b, float t)
{
    ASSERT_MSG(std::isfinite(a) && std::isfinite(b) && std::isfinite(t),
        "SmoothStep inputs must be finite");
    t = Clamp(t, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return Lerp(a, b, t);
}

// =============================================================================
// DISTANCE CALCULATIONS
// =============================================================================

float MathUtils::Distance(const XMFLOAT3& a, const XMFLOAT3& b)
{
    ASSERT_MSG(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z), "Distance a must be finite");
    ASSERT_MSG(std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z), "Distance b must be finite");
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

float MathUtils::DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b)
{
    ASSERT_MSG(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z), "DistanceSquared a must be finite");
    ASSERT_MSG(std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z), "DistanceSquared b must be finite");
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    return dx * dx + dy * dy + dz * dz;
}

XMFLOAT3 MathUtils::Direction(const XMFLOAT3& from, const XMFLOAT3& to)
{
    XMFLOAT3 dir{ to.x - from.x, to.y - from.y, to.z - from.z };
    return Normalize(dir);
}

// =============================================================================
// RANDOM NUMBER GENERATION
// =============================================================================

void MathUtils::InitializeRandom()
{
    if (!s_randomInitialized)
    {
        s_randomGenerator.seed(s_randomDevice());
        s_randomInitialized = true;
    }
}

float MathUtils::RandomFloat(float min, float max) {
    std::wcout << L"[OPERATION] MathUtils::RandomFloat called. min=" << min << L" max=" << max << std::endl;
    float result = min + static_cast<float>(rand()) / (static_cast<float>(RAND_MAX / (max - min)));
    std::wcout << L"[INFO] MathUtils::RandomFloat result=" << result << std::endl;
    return result;
}

int MathUtils::RandomInt(int min, int max)
{
    ASSERT_MSG(min <= max, "RandomInt min must be <= max");
    InitializeRandom();
    std::uniform_int_distribution<int> distribution(min, max);
    return distribution(s_randomGenerator);
}

XMFLOAT3 MathUtils::RandomDirection()
{
    float theta = RandomFloat(0.0f, TWO_PI);
    float phi = RandomFloat(-HALF_PI, HALF_PI);
    float cosTheta = cosf(theta), sinTheta = sinf(theta);
    float cosPhi = cosf(phi), sinPhi = sinf(phi);
    return XMFLOAT3(cosPhi * cosTheta, sinPhi, cosPhi * sinTheta);
}

XMFLOAT3 MathUtils::RandomPointInSphere(float radius)
{
    ASSERT_MSG(radius >= 0.0f, "Sphere radius must be non-negative");
    XMFLOAT3 point{};
    float lengthSq;
    do {
        point.x = RandomFloat(-1.0f, 1.0f);
        point.y = RandomFloat(-1.0f, 1.0f);
        point.z = RandomFloat(-1.0f, 1.0f);
        lengthSq = LengthSquared(point);
    } while (lengthSq > 1.0f);
    return Multiply(point, radius);
}

// =============================================================================
// CLAMPING FUNCTIONS
// =============================================================================

float MathUtils::Clamp(float value, float min, float max)
{
    ASSERT_MSG(min <= max, "Clamp min must be <= max");
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

int MathUtils::Clamp(int value, int min, int max)
{
    ASSERT_MSG(min <= max, "Clamp min must be <= max");
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

XMFLOAT3 MathUtils::Clamp(const XMFLOAT3& value, const XMFLOAT3& min, const XMFLOAT3& max)
{
    return XMFLOAT3(
        Clamp(value.x, min.x, max.x),
        Clamp(value.y, min.y, max.y),
        Clamp(value.z, min.z, max.z)
    );
}

// =============================================================================
// MATRIX UTILITIES
// =============================================================================

XMMATRIX MathUtils::CreateLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up)
{
    return XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMLoadFloat3(&up));
}

XMMATRIX MathUtils::CreatePerspective(float fovY, float aspectRatio, float nearPlane, float farPlane)
{
    ASSERT_MSG(fovY > 0 && aspectRatio > 0 && nearPlane > 0 && farPlane > nearPlane,
        "Invalid perspective parameters");
    return XMMatrixPerspectiveFovLH(fovY, aspectRatio, nearPlane, farPlane);
}

XMMATRIX MathUtils::CreateOrthographic(float width, float height, float nearPlane, float farPlane)
{
    ASSERT_MSG(width > 0 && height > 0 && farPlane > nearPlane,
        "Invalid orthographic parameters");
    return XMMatrixOrthographicLH(width, height, nearPlane, farPlane);
}

// =============================================================================
// VECTOR OPERATIONS
// =============================================================================

XMFLOAT3 MathUtils::Add(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}

XMFLOAT3 MathUtils::Subtract(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}

XMFLOAT3 MathUtils::Multiply(const XMFLOAT3& v, float scalar)
{
    ASSERT_MSG(std::isfinite(scalar), "Multiply scalar must be finite");
    return XMFLOAT3(v.x * scalar, v.y * scalar, v.z * scalar);
}

XMFLOAT3 MathUtils::Divide(const XMFLOAT3& v, float scalar)
{
    ASSERT_MSG(scalar != 0.0f, "Divide by zero");
    float inv = 1.0f / scalar;
    return XMFLOAT3(v.x * inv, v.y * inv, v.z * inv);
}

float MathUtils::Dot(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

XMFLOAT3 MathUtils::Cross(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

XMFLOAT3 MathUtils::Normalize(const XMFLOAT3& v)
{
    ASSERT_MSG(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
        "Normalize input must be finite");
    float len = Length(v);
    ASSERT_MSG(len >= 0.0f, "Length must be non-negative");
    return len > 0.0f ? Divide(v, len) : XMFLOAT3(0, 0, 0);
}

float MathUtils::Length(const XMFLOAT3& v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

float MathUtils::LengthSquared(const XMFLOAT3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// =============================================================================
// EASING FUNCTIONS
// =============================================================================

float MathUtils::EaseInQuad(float t)
{
    ASSERT_MSG(std::isfinite(t), "EaseInQuad t must be finite");
    return t * t;
}

float MathUtils::EaseOutQuad(float t)
{
    ASSERT_MSG(std::isfinite(t), "EaseOutQuad t must be finite");
    return 1.0f - (1.0f - t) * (1.0f - t);
}

float MathUtils::EaseInOutQuad(float t)
{
    ASSERT_MSG(std::isfinite(t), "EaseInOutQuad t must be finite");
    if (t < 0.5f)
        return 2.0f * t * t;
    else {
        float f = (1.0f - t);
        return 1.0f - 2.0f * f * f;
    }
}

float MathUtils::EaseInCubic(float t)
{
    ASSERT_MSG(std::isfinite(t), "EaseInCubic t must be finite");
    return t * t * t;
}

float MathUtils::EaseOutCubic(float t)
{
    ASSERT_MSG(std::isfinite(t), "EaseOutCubic t must be finite");
    float f = (1.0f - t);
    return 1.0f - f * f * f;
}

float MathUtils::EaseInOutCubic(float t)
{
    ASSERT_MSG(std::isfinite(t), "EaseInOutCubic t must be finite");
    if (t < 0.5f)
        return 4.0f * t * t * t;
    else {
        float f = 2.0f * t - 2.0f;
        return 1.0f + f * f * f * 0.5f;
    }
}
