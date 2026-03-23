/**
 * @file TestCommonMath.h
 * @brief Shared math types and comparison macros for test files
 *
 * Replaces Vec3/Vec4/Quaternion definitions duplicated across 26+ test files
 * with a single shared header. Include this alongside TestFramework.h.
 */

#pragma once

#include "TestFramework.h"
#include <cmath>

namespace TestMath
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    };

    struct Vec4
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    };

    struct Quaternion
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };

    inline float Length(const Vec3& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    inline Vec3 Normalize(const Vec3& v)
    {
        float len = Length(v);
        if (len < 1e-8f)
            return {0.0f, 0.0f, 0.0f};
        return {v.x / len, v.y / len, v.z / len};
    }

    inline float Dot(const Vec3& a, const Vec3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

} // namespace TestMath

// Compare two Vec3 values component-wise within a tolerance
#define EXPECT_VECTOR_NEAR(a, b, tolerance)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        EXPECT_NEAR((a).x, (b).x, tolerance);                                                                          \
        EXPECT_NEAR((a).y, (b).y, tolerance);                                                                          \
        EXPECT_NEAR((a).z, (b).z, tolerance);                                                                          \
    } while (0)

// Compare two Quaternion values component-wise within a tolerance
#define EXPECT_QUATERNION_NEAR(a, b, tolerance)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        EXPECT_NEAR((a).x, (b).x, tolerance);                                                                          \
        EXPECT_NEAR((a).y, (b).y, tolerance);                                                                          \
        EXPECT_NEAR((a).z, (b).z, tolerance);                                                                          \
        EXPECT_NEAR((a).w, (b).w, tolerance);                                                                          \
    } while (0)
