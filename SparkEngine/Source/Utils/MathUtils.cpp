#include "MathUtils.h"
#include "../Core/Platform.h"
// MathUtils.cpp
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "Validate.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <cmath>

using namespace DirectX;

// Static constant definitions
const float MathUtils::PI = 3.14159265359f;
const float MathUtils::TWO_PI = 6.28318530718f;
const float MathUtils::HALF_PI = 1.57079632679f;
const float MathUtils::DEG_TO_RAD = 0.01745329252f;
const float MathUtils::RAD_TO_DEG = 57.2957795131f;

// =============================================================================
// ANGLE UTILITIES
// =============================================================================

float MathUtils::DegreesToRadians(float degrees)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(degrees), "Degrees must be finite");
    return degrees * DEG_TO_RAD;
}

float MathUtils::RadiansToDegrees(float radians)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(radians), "Radians must be finite");
    return radians * RAD_TO_DEG;
}

float MathUtils::WrapAngle(float angle)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(angle), "Angle must be finite");
    while (angle > PI)
        angle -= TWO_PI;
    while (angle < -PI)
        angle += TWO_PI;
    return angle;
}

float MathUtils::NormalizeAngle(float angle)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(angle), "Angle must be finite");
    while (angle < 0.0f)
        angle += TWO_PI;
    while (angle >= TWO_PI)
        angle -= TWO_PI;
    return angle;
}

// =============================================================================
// INTERPOLATION FUNCTIONS
// =============================================================================

float MathUtils::Lerp(float a, float b, float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(a) && std::isfinite(b) && std::isfinite(t),
                      "Lerp inputs must be finite");
    return a + t * (b - a);
}

XMFLOAT3 MathUtils::Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(t), "Lerp t must be finite");
    return XMFLOAT3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
}

float MathUtils::SmoothStep(float a, float b, float t)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(a) && std::isfinite(b) && std::isfinite(t),
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
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z),
                      "Distance a must be finite");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z),
                      "Distance b must be finite");
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

float MathUtils::DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z),
                      "DistanceSquared a must be finite");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z),
                      "DistanceSquared b must be finite");
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    return dx * dx + dy * dy + dz * dz;
}

XMFLOAT3 MathUtils::Direction(const XMFLOAT3& from, const XMFLOAT3& to)
{
    XMFLOAT3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
    return Normalize(dir);
}

// =============================================================================
// CLAMPING FUNCTIONS
// =============================================================================

float MathUtils::Clamp(float value, float min, float max)
{
    SPARK_LOG_IF(Spark::LogLevel::Warn, Spark::LogCategory::Core, min > max,
                 "MathUtils::Clamp: min (%f) > max (%f), inputs swapped", min, max);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, min <= max, "Clamp min must be <= max");
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

int MathUtils::Clamp(int value, int min, int max)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, min <= max, "Clamp min must be <= max");
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

XMFLOAT3 MathUtils::Clamp(const XMFLOAT3& value, const XMFLOAT3& min, const XMFLOAT3& max)
{
    return XMFLOAT3(Clamp(value.x, min.x, max.x), Clamp(value.y, min.y, max.y), Clamp(value.z, min.z, max.z));
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
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(scalar), "Multiply scalar must be finite");
    return XMFLOAT3(v.x * scalar, v.y * scalar, v.z * scalar);
}

XMFLOAT3 MathUtils::Divide(const XMFLOAT3& v, float scalar)
{
    SPARK_LOG_IF(Spark::LogLevel::Error, Spark::LogCategory::Core, scalar == 0.0f,
                 "MathUtils::Divide: Division by zero attempted");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, scalar != 0.0f, "Divide by zero");
    float inv = 1.0f / scalar;
    return XMFLOAT3(v.x * inv, v.y * inv, v.z * inv);
}

float MathUtils::Dot(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

XMFLOAT3 MathUtils::Cross(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

XMFLOAT3 MathUtils::Normalize(const XMFLOAT3& v)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
                      "Normalize input must be finite");
    float len = Length(v);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, len >= 0.0f, "Length must be non-negative");
    if (len == 0.0f)
        SPARK_LOG_WARN(Spark::LogCategory::Core, "MathUtils::Normalize: Zero-length vector, returning zero vector");
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
// QUATERNION / EULER CONVERSION
// =============================================================================

XMFLOAT3 MathUtils::QuaternionToEulerDegrees(float qx, float qy, float qz, float qw)
{
    // Roll (X axis)
    float sinrCosp = 2.0f * (qw * qx + qy * qz);
    float cosrCosp = 1.0f - 2.0f * (qx * qx + qy * qy);
    float roll = std::atan2(sinrCosp, cosrCosp) * RAD_TO_DEG;

    // Pitch (Y axis) — handle gimbal lock
    float sinp = 2.0f * (qw * qy - qz * qx);
    float pitch = 0.0f;
    if (std::abs(sinp) >= 1.0f)
        pitch = std::copysign(90.0f, sinp);
    else
        pitch = std::asin(sinp) * RAD_TO_DEG;

    // Yaw (Z axis)
    float sinyCosp = 2.0f * (qw * qz + qx * qy);
    float cosyCosp = 1.0f - 2.0f * (qy * qy + qz * qz);
    float yaw = std::atan2(sinyCosp, cosyCosp) * RAD_TO_DEG;

    return XMFLOAT3(roll, pitch, yaw);
}

XMFLOAT4 MathUtils::EulerDegreesToQuaternion(float rollDeg, float pitchDeg, float yawDeg)
{
    float rx = rollDeg * DEG_TO_RAD * 0.5f;
    float ry = pitchDeg * DEG_TO_RAD * 0.5f;
    float rz = yawDeg * DEG_TO_RAD * 0.5f;

    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    return XMFLOAT4(sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz,
                    cx * cy * cz + sx * sy * sz);
}
