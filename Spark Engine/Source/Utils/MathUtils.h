// MathUtils.h
#pragma once

#include "../Core/framework.h"    // XMFLOAT3, XMMATRIX
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

class MathUtils
{
public:
    // Constants
    static const float PI;
    static const float TWO_PI;
    static const float HALF_PI;
    static const float DEG_TO_RAD;
    static const float RAD_TO_DEG;

    // Angle utilities
    static float DegreesToRadians(float degrees);
    static float RadiansToDegrees(float radians);
    static float WrapAngle(float angle);
    static float NormalizeAngle(float angle);

    // Interpolation
    static float    Lerp(float a, float b, float t);
    static XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t);
    static float    SmoothStep(float a, float b, float t);

    // Distance calculations
    static float    Distance(const XMFLOAT3& a, const XMFLOAT3& b);
    static float    DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b);
    static XMFLOAT3 Direction(const XMFLOAT3& from, const XMFLOAT3& to);

    // Random number generation
    static void     InitializeRandom();
    static float    RandomFloat(float min = 0.0f, float max = 1.0f);
    static int      RandomInt(int min, int max);
    static XMFLOAT3 RandomDirection();
    static XMFLOAT3 RandomPointInSphere(float radius);

    // Clamping
    static float    Clamp(float value, float min, float max);
    static int      Clamp(int value, int min, int max);
    static XMFLOAT3 Clamp(const XMFLOAT3& value, const XMFLOAT3& min, const XMFLOAT3& max);

    // Matrix utilities
    static XMMATRIX CreateLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up);
    static XMMATRIX CreatePerspective(float fovY, float aspectRatio, float nearPlane, float farPlane);
    static XMMATRIX CreateOrthographic(float width, float height, float nearPlane, float farPlane);

    // Collision utilities
    static bool     PointInSphere(const XMFLOAT3& point, const XMFLOAT3& sphereCenter, float sphereRadius);
    static bool     PointInBox(const XMFLOAT3& point, const XMFLOAT3& boxMin, const XMFLOAT3& boxMax);

    // Vector operations
    static XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b);
    static XMFLOAT3 Subtract(const XMFLOAT3& a, const XMFLOAT3& b);
    static XMFLOAT3 Multiply(const XMFLOAT3& v, float scalar);
    static XMFLOAT3 Divide(const XMFLOAT3& v, float scalar);
    static float    Dot(const XMFLOAT3& a, const XMFLOAT3& b);
    static XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b);
    static XMFLOAT3 Normalize(const XMFLOAT3& v);
    static float    Length(const XMFLOAT3& v);
    static float    LengthSquared(const XMFLOAT3& v);

    // Easing functions
    static float    EaseInQuad(float t);
    static float    EaseOutQuad(float t);
    static float    EaseInOutQuad(float t);
    static float    EaseInCubic(float t);
    static float    EaseOutCubic(float t);
    static float    EaseInOutCubic(float t);
};