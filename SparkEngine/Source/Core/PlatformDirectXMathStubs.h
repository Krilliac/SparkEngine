/**
 * @file PlatformDirectXMathStubs.h
 * @brief DirectXMath compatibility stubs for non-Windows platforms
 *
 * Provides lightweight implementations of DirectXMath types and functions
 * (XMFLOAT2/3/4, XMVECTOR, XMMATRIX, and all common math operations)
 * so that engine code using DirectXMath compiles on Linux and macOS.
 */

#pragma once

#ifndef SPARK_PLATFORM_WINDOWS

#include <cmath>
#include <cstring>

// ============================================================================
// DirectXMath Compatibility (minimal stubs for non-Windows)
// ============================================================================

namespace DirectX
{

    struct XMFLOAT2
    {
        float x, y;
        constexpr XMFLOAT2() : x(0), y(0) {}
        constexpr XMFLOAT2(float _x, float _y) : x(_x), y(_y) {}
    };

    struct XMFLOAT3
    {
        float x, y, z;
        constexpr XMFLOAT3() : x(0), y(0), z(0) {}
        constexpr XMFLOAT3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    };

    struct XMFLOAT4
    {
        float x, y, z, w;
        constexpr XMFLOAT4() : x(0), y(0), z(0), w(0) {}
        constexpr XMFLOAT4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    };

    struct XMFLOAT4X4
    {
        // Named row-major elements (_11.._44) alias the m[4][4] grid, matching
        // DirectXMath so code like `mat._41` (the translation row) compiles.
        union
        {
            struct
            {
                float _11, _12, _13, _14;
                float _21, _22, _23, _24;
                float _31, _32, _33, _34;
                float _41, _42, _43, _44;
            };
            float m[4][4];
        };
        XMFLOAT4X4() { memset(m, 0, sizeof(m)); }
    };

    // XMVECTOR as a 4-component float vector (must come before XMMATRIX for union)
    struct XMVECTOR
    {
        float x, y, z, w;
    };

    // XMMATRIX as a simple 4x4 float matrix with row access
    struct XMMATRIX
    {
        union
        {
            float m[4][4];
            XMVECTOR r[4];
        };
        XMMATRIX() { memset(m, 0, sizeof(m)); }
        XMMATRIX(float m00, float m01, float m02, float m03, float m10, float m11, float m12, float m13, float m20,
                 float m21, float m22, float m23, float m30, float m31, float m32, float m33)
        {
            m[0][0] = m00;
            m[0][1] = m01;
            m[0][2] = m02;
            m[0][3] = m03;
            m[1][0] = m10;
            m[1][1] = m11;
            m[1][2] = m12;
            m[1][3] = m13;
            m[2][0] = m20;
            m[2][1] = m21;
            m[2][2] = m22;
            m[2][3] = m23;
            m[3][0] = m30;
            m[3][1] = m31;
            m[3][2] = m32;
            m[3][3] = m33;
        }
    };

    inline XMMATRIX XMMatrixIdentity()
    {
        XMMATRIX mat;
        mat.m[0][0] = mat.m[1][1] = mat.m[2][2] = mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMMATRIX XMMatrixTranslation(float x, float y, float z)
    {
        XMMATRIX mat = XMMatrixIdentity();
        mat.m[3][0] = x;
        mat.m[3][1] = y;
        mat.m[3][2] = z;
        return mat;
    }

    inline XMMATRIX XMMatrixScaling(float x, float y, float z)
    {
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = x;
        mat.m[1][1] = y;
        mat.m[2][2] = z;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMMATRIX XMMatrixRotationY(float angle)
    {
        XMMATRIX mat = XMMatrixIdentity();
        float c = cosf(angle), s = sinf(angle);
        mat.m[0][0] = c;
        mat.m[0][2] = -s;
        mat.m[2][0] = s;
        mat.m[2][2] = c;
        return mat;
    }

    inline XMMATRIX XMMatrixMultiply(const XMMATRIX& a, const XMMATRIX& b)
    {
        XMMATRIX result;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                result.m[i][j] = 0;
                for (int k = 0; k < 4; k++)
                    result.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        return result;
    }

    inline XMMATRIX operator*(const XMMATRIX& a, const XMMATRIX& b)
    {
        return XMMatrixMultiply(a, b);
    }

    inline XMMATRIX XMMatrixInverse(XMVECTOR* det, XMMATRIX mat)
    {
        const float* m = &mat.m[0][0];
        float inv[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
                 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
                 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
                 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
                  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
                 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
                 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
                 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
                  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
                 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
                 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
                  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
                  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
                 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
                 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
                  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
                  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        float d = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (det)
        {
            det->x = d;
            det->y = d;
            det->z = d;
            det->w = d;
        }
        if (fabsf(d) < 1e-12f)
            return XMMatrixIdentity();
        float invDet = 1.0f / d;
        XMMATRIX result;
        for (int i = 0; i < 16; i++)
            (&result.m[0][0])[i] = inv[i] * invDet;
        return result;
    }

    // Non-const overload (called with rvalue)
    inline XMMATRIX XMMatrixTranspose(XMMATRIX m)
    {
        XMMATRIX result;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                result.m[i][j] = m.m[j][i];
        return result;
    }

    inline XMMATRIX XMMatrixPerspectiveFovLH(float fov, float aspect, float nearZ, float farZ)
    {
        float yScale = 1.0f / tanf(fov * 0.5f);
        float xScale = yScale / aspect;
        float range = farZ / (farZ - nearZ);
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = xScale;
        mat.m[1][1] = yScale;
        mat.m[2][2] = range;
        mat.m[2][3] = 1.0f;
        mat.m[3][2] = -range * nearZ;
        return mat;
    }

    inline XMMATRIX XMMatrixOrthographicLH(float width, float height, float nearZ, float farZ)
    {
        float range = 1.0f / (farZ - nearZ);
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = 2.0f / width;
        mat.m[1][1] = 2.0f / height;
        mat.m[2][2] = range;
        mat.m[3][2] = -range * nearZ;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMMATRIX XMMatrixOrthographicOffCenterLH(float left, float right, float bottom, float top, float nearZ,
                                                    float farZ)
    {
        float range = 1.0f / (farZ - nearZ);
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = 2.0f / (right - left);
        mat.m[1][1] = 2.0f / (top - bottom);
        mat.m[2][2] = range;
        mat.m[3][0] = -(right + left) / (right - left);
        mat.m[3][1] = -(top + bottom) / (top - bottom);
        mat.m[3][2] = -range * nearZ;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMVECTOR XMVectorSet(float x, float y, float z, float w)
    {
        return {x, y, z, w};
    }

    inline void XMStoreFloat3(XMFLOAT3* dest, XMVECTOR v)
    {
        dest->x = v.x;
        dest->y = v.y;
        dest->z = v.z;
    }

    inline void XMStoreFloat4(XMFLOAT4* dest, XMVECTOR v)
    {
        dest->x = v.x;
        dest->y = v.y;
        dest->z = v.z;
        dest->w = v.w;
    }

    inline XMVECTOR XMLoadFloat3(const XMFLOAT3* src)
    {
        return {src->x, src->y, src->z, 0.0f};
    }

    inline XMVECTOR XMLoadFloat4(const XMFLOAT4* src)
    {
        return {src->x, src->y, src->z, src->w};
    }

    inline float XMConvertToRadians(float degrees)
    {
        return degrees * 3.14159265358979323846f / 180.0f;
    }

    inline float XMConvertToDegrees(float radians)
    {
        return radians * 180.0f / 3.14159265358979323846f;
    }

    // Constants
    constexpr float XM_PI = 3.14159265358979323846f;
    constexpr float XM_2PI = 6.28318530717958647692f;
    constexpr float XM_PIDIV2 = 1.57079632679489661923f;
    constexpr float XM_PIDIV4 = 0.78539816339744830961f;
    constexpr float XM_1DIVPI = 0.31830988618379067154f;
    constexpr float XM_1DIV2PI = 0.15915494309189533577f;

    // Vector arithmetic
    inline XMVECTOR XMVectorAdd(XMVECTOR a, XMVECTOR b)
    {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    inline XMVECTOR XMVectorSubtract(XMVECTOR a, XMVECTOR b)
    {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    inline XMVECTOR XMVectorScale(XMVECTOR v, float s)
    {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    inline XMVECTOR XMVectorMultiply(XMVECTOR a, XMVECTOR b)
    {
        return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
    }
    inline XMVECTOR XMVectorNegate(XMVECTOR v)
    {
        return {-v.x, -v.y, -v.z, -v.w};
    }
    inline XMVECTOR XMVectorZero()
    {
        return {0, 0, 0, 0};
    }
    inline XMVECTOR XMVectorReplicate(float v)
    {
        return {v, v, v, v};
    }
    inline XMVECTOR XMVectorMin(XMVECTOR a, XMVECTOR b)
    {
        return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w};
    }
    inline XMVECTOR XMVectorMax(XMVECTOR a, XMVECTOR b)
    {
        return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w};
    }

    // Vector operators
    inline XMVECTOR operator+(XMVECTOR a, XMVECTOR b)
    {
        return XMVectorAdd(a, b);
    }
    inline XMVECTOR operator-(XMVECTOR a, XMVECTOR b)
    {
        return XMVectorSubtract(a, b);
    }
    inline XMVECTOR operator*(XMVECTOR v, float s)
    {
        return XMVectorScale(v, s);
    }
    inline XMVECTOR operator*(float s, XMVECTOR v)
    {
        return XMVectorScale(v, s);
    }

    // Vector3 math (return XMVECTOR for DirectXMath API compatibility)
    inline XMVECTOR XMVector3Dot(XMVECTOR a, XMVECTOR b)
    {
        float d = a.x * b.x + a.y * b.y + a.z * b.z;
        return {d, d, d, d};
    }
    inline XMVECTOR XMVector3Length(XMVECTOR v)
    {
        float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        return {len, len, len, len};
    }
    inline XMVECTOR XMVector3LengthSq(XMVECTOR v)
    {
        float sq = v.x * v.x + v.y * v.y + v.z * v.z;
        return {sq, sq, sq, sq};
    }
    inline XMVECTOR XMVector3Normalize(XMVECTOR v)
    {
        float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len < 1e-8f)
            return {0, 0, 0, 0};
        float inv = 1.0f / len;
        return {v.x * inv, v.y * inv, v.z * inv, 0.0f};
    }
    inline XMVECTOR XMVector3Cross(XMVECTOR a, XMVECTOR b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
    }
    inline XMVECTOR XMVector3TransformCoord(XMVECTOR v, const XMMATRIX& m)
    {
        XMVECTOR result;
        result.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0];
        result.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1];
        result.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2];
        float w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + m.m[3][3];
        if (fabsf(w) > 1e-8f)
        {
            result.x /= w;
            result.y /= w;
            result.z /= w;
        }
        result.w = 0.0f;
        return result;
    }

    // Transform a direction (normal): row-vector * matrix with w=0, so the
    // translation row is ignored and no perspective divide is applied.
    inline XMVECTOR XMVector3TransformNormal(XMVECTOR v, const XMMATRIX& m)
    {
        XMVECTOR r;
        r.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0];
        r.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1];
        r.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2];
        r.w = 0.0f;
        return r;
    }

    // Full 4D transform: row-vector * matrix, w participates, no divide.
    inline XMVECTOR XMVector4Transform(XMVECTOR v, const XMMATRIX& m)
    {
        XMVECTOR r;
        r.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0];
        r.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1];
        r.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2];
        r.w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3];
        return r;
    }

    // Predicate: true iff every component of a is strictly less than b's.
    inline bool XMVector4Less(XMVECTOR a, XMVECTOR b)
    {
        return a.x < b.x && a.y < b.y && a.z < b.z && a.w < b.w;
    }

    // Per-component compare producing a float mask (1.0 = true, 0.0 = false),
    // consumed by XMVectorSelect below. (The stub uses a float mask rather than
    // DirectXMath's bit mask; the two helpers are only ever used as a pair.)
    inline XMVECTOR XMVectorGreaterOrEqual(XMVECTOR a, XMVECTOR b)
    {
        return {a.x >= b.x ? 1.0f : 0.0f, a.y >= b.y ? 1.0f : 0.0f, a.z >= b.z ? 1.0f : 0.0f, a.w >= b.w ? 1.0f : 0.0f};
    }

    // Per-component select: where control is non-zero take b, else a. Pairs with
    // the float mask from XMVectorGreaterOrEqual.
    inline XMVECTOR XMVectorSelect(XMVECTOR a, XMVECTOR b, XMVECTOR control)
    {
        return {control.x != 0.0f ? b.x : a.x, control.y != 0.0f ? b.y : a.y, control.z != 0.0f ? b.z : a.z,
                control.w != 0.0f ? b.w : a.w};
    }

    inline XMVECTOR XMVector3Project(XMVECTOR v, float viewportX, float viewportY, float viewportWidth,
                                     float viewportHeight, float minZ, float maxZ, const XMMATRIX& projection,
                                     const XMMATRIX& view, const XMMATRIX& world)
    {
        XMVECTOR result = XMVector3TransformCoord(v, world);
        result = XMVector3TransformCoord(result, view);
        result = XMVector3TransformCoord(result, projection);
        // Map from [-1,1] to viewport
        XMVECTOR screen;
        screen.x = (result.x * 0.5f + 0.5f) * viewportWidth + viewportX;
        screen.y = (-result.y * 0.5f + 0.5f) * viewportHeight + viewportY;
        screen.z = result.z * (maxZ - minZ) + minZ;
        screen.w = 1.0f;
        return screen;
    }

    inline XMVECTOR XMVector3Unproject(XMVECTOR v, float viewportX, float viewportY, float viewportWidth,
                                       float viewportHeight, float minZ, float maxZ, const XMMATRIX& projection,
                                       const XMMATRIX& view, const XMMATRIX& world)
    {
        // Reverse the projection: viewport -> NDC -> world
        XMVECTOR ndc;
        ndc.x = ((v.x - viewportX) / viewportWidth) * 2.0f - 1.0f;
        ndc.y = -(((v.y - viewportY) / viewportHeight) * 2.0f - 1.0f);
        ndc.z = (v.z - minZ) / (maxZ - minZ);
        ndc.w = 1.0f;

        XMMATRIX combined = XMMatrixMultiply(world, XMMatrixMultiply(view, projection));
        XMMATRIX inv = XMMatrixInverse(nullptr, combined);
        return XMVector3TransformCoord(ndc, inv);
    }

    // Rotation matrices
    inline XMMATRIX XMMatrixRotationX(float angle)
    {
        XMMATRIX mat = XMMatrixIdentity();
        float c = cosf(angle), s = sinf(angle);
        mat.m[1][1] = c;
        mat.m[1][2] = s;
        mat.m[2][1] = -s;
        mat.m[2][2] = c;
        return mat;
    }
    inline XMMATRIX XMMatrixRotationZ(float angle)
    {
        XMMATRIX mat = XMMatrixIdentity();
        float c = cosf(angle), s = sinf(angle);
        mat.m[0][0] = c;
        mat.m[0][1] = s;
        mat.m[1][0] = -s;
        mat.m[1][1] = c;
        return mat;
    }
    inline XMMATRIX XMMatrixRotationRollPitchYaw(float pitch, float yaw, float roll)
    {
        const float cp = cosf(pitch);
        const float sp = sinf(pitch);
        const float cy = cosf(yaw);
        const float sy = sinf(yaw);
        const float cr = cosf(roll);
        const float sr = sinf(roll);

        // DirectXMath's scalar reference formula. This is equivalent to
        // Rz * Rx * Ry for the row-vector convention used by DirectXMath.
        return {cr * cy + sr * sp * sy,
                sr * cp,
                sr * sp * cy - cr * sy,
                0.0f,
                cr * sp * sy - sr * cy,
                cr * cp,
                sr * sy + cr * sp * cy,
                0.0f,
                cp * sy,
                -sp,
                cp * cy,
                0.0f,
                0.0f,
                0.0f,
                0.0f,
                1.0f};
    }

    // Store/Load additional
    inline void XMStoreFloat4x4(XMFLOAT4X4* dest, const XMMATRIX& m)
    {
        memcpy(dest->m, m.m, sizeof(float) * 16);
    }
    inline XMMATRIX XMLoadFloat4x4(const XMFLOAT4X4* src)
    {
        XMMATRIX m;
        memcpy(m.m, src->m, sizeof(float) * 16);
        return m;
    }

    inline XMVECTOR XMVectorLerp(XMVECTOR a, XMVECTOR b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }

    // Component accessors
    inline float XMVectorGetX(XMVECTOR v)
    {
        return v.x;
    }
    inline float XMVectorGetY(XMVECTOR v)
    {
        return v.y;
    }
    inline float XMVectorGetZ(XMVECTOR v)
    {
        return v.z;
    }
    inline float XMVectorGetW(XMVECTOR v)
    {
        return v.w;
    }

    // Component setters: return a copy of `v` with one component replaced
    // (DirectXMath semantics). Mirrors the XMVectorGet* accessors above.
    inline XMVECTOR XMVectorSetX(XMVECTOR v, float x)
    {
        return {x, v.y, v.z, v.w};
    }
    inline XMVECTOR XMVectorSetY(XMVECTOR v, float y)
    {
        return {v.x, y, v.z, v.w};
    }
    inline XMVECTOR XMVectorSetZ(XMVECTOR v, float z)
    {
        return {v.x, v.y, z, v.w};
    }
    inline XMVECTOR XMVectorSetW(XMVECTOR v, float w)
    {
        return {v.x, v.y, v.z, w};
    }

    inline XMMATRIX XMMatrixLookAtLH(XMVECTOR eye, XMVECTOR target, XMVECTOR up)
    {
        XMVECTOR zAxis = XMVector3Normalize(XMVectorSubtract(target, eye));
        XMVECTOR xAxis = XMVector3Normalize(XMVector3Cross(up, zAxis));
        XMVECTOR yAxis = XMVector3Cross(zAxis, xAxis);
        float dx = -(XMVectorGetX(XMVector3Dot(xAxis, eye)));
        float dy = -(XMVectorGetX(XMVector3Dot(yAxis, eye)));
        float dz = -(XMVectorGetX(XMVector3Dot(zAxis, eye)));
        XMMATRIX mat;
        mat.m[0][0] = xAxis.x;
        mat.m[0][1] = yAxis.x;
        mat.m[0][2] = zAxis.x;
        mat.m[0][3] = 0.0f;
        mat.m[1][0] = xAxis.y;
        mat.m[1][1] = yAxis.y;
        mat.m[1][2] = zAxis.y;
        mat.m[1][3] = 0.0f;
        mat.m[2][0] = xAxis.z;
        mat.m[2][1] = yAxis.z;
        mat.m[2][2] = zAxis.z;
        mat.m[2][3] = 0.0f;
        mat.m[3][0] = dx;
        mat.m[3][1] = dy;
        mat.m[3][2] = dz;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    // Transform functions
    inline XMVECTOR XMVector3Transform(XMVECTOR v, const XMMATRIX& m)
    {
        return XMVector3TransformCoord(v, m);
    }

    // Quaternion stubs
    inline XMVECTOR XMQuaternionRotationRollPitchYaw(float pitch, float yaw, float roll)
    {
        float halfPitch = pitch * 0.5f;
        float halfYaw = yaw * 0.5f;
        float halfRoll = roll * 0.5f;
        float sp = sinf(halfPitch), cp = cosf(halfPitch);
        float sy = sinf(halfYaw), cy = cosf(halfYaw);
        float sr = sinf(halfRoll), cr = cosf(halfRoll);
        return {cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
                cy * cp * cr + sy * sp * sr};
    }

    inline XMVECTOR XMQuaternionRotationAxis(XMVECTOR axis, float angle)
    {
        XMVECTOR n = XMVector3Normalize(axis);
        float halfAngle = angle * 0.5f;
        float s = sinf(halfAngle);
        return {n.x * s, n.y * s, n.z * s, cosf(halfAngle)};
    }

    inline XMVECTOR XMQuaternionMultiply(XMVECTOR q1, XMVECTOR q2)
    {
        return {q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
                q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
                q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
                q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z};
    }

    inline XMVECTOR XMQuaternionNormalize(XMVECTOR q)
    {
        float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (len < 1e-8f)
            return {0, 0, 0, 1};
        float inv = 1.0f / len;
        return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    }

    inline XMVECTOR XMQuaternionSlerp(XMVECTOR a, XMVECTOR b, float t)
    {
        return XMVectorLerp(a, b, t); // Simplified linear interpolation
    }

    inline XMMATRIX XMMatrixRotationQuaternion(XMVECTOR q)
    {
        // Simplified quaternion to matrix
        float x = q.x, y = q.y, z = q.z, w = q.w;
        XMMATRIX m = XMMatrixIdentity();
        m.m[0][0] = 1 - 2 * (y * y + z * z);
        m.m[0][1] = 2 * (x * y + w * z);
        m.m[0][2] = 2 * (x * z - w * y);
        m.m[1][0] = 2 * (x * y - w * z);
        m.m[1][1] = 1 - 2 * (x * x + z * z);
        m.m[1][2] = 2 * (y * z + w * x);
        m.m[2][0] = 2 * (x * z + w * y);
        m.m[2][1] = 2 * (y * z - w * x);
        m.m[2][2] = 1 - 2 * (x * x + y * y);
        return m;
    }

    inline XMMATRIX XMMatrixRotationAxis(XMVECTOR axis, float angle)
    {
        float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
        float x = axis.x, y = axis.y, z = axis.z;
        XMMATRIX m = XMMatrixIdentity();
        m.m[0][0] = t * x * x + c;
        m.m[0][1] = t * x * y + s * z;
        m.m[0][2] = t * x * z - s * y;
        m.m[1][0] = t * x * y - s * z;
        m.m[1][1] = t * y * y + c;
        m.m[1][2] = t * y * z + s * x;
        m.m[2][0] = t * x * z + s * y;
        m.m[2][1] = t * y * z - s * x;
        m.m[2][2] = t * z * z + c;
        return m;
    }

    inline XMMATRIX XMMatrixTranslationFromVector(XMVECTOR v)
    {
        return XMMatrixTranslation(v.x, v.y, v.z);
    }

    inline XMMATRIX XMMatrixScalingFromVector(XMVECTOR v)
    {
        return XMMatrixScaling(v.x, v.y, v.z);
    }

    inline bool XMMatrixDecompose(XMVECTOR* outScale, XMVECTOR* outRotQuat, XMVECTOR* outTrans, const XMMATRIX& m)
    {
        // Extract translation
        *outTrans = {m.m[3][0], m.m[3][1], m.m[3][2], 1.0f};
        // Extract scale (column lengths)
        float sx = sqrtf(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
        float sy = sqrtf(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
        float sz = sqrtf(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2]);
        *outScale = {sx, sy, sz, 0.0f};
        *outRotQuat = {0, 0, 0, 1}; // Identity quaternion as stub
        return true;
    }

    // XMMATRIX 16-float constructor
    inline XMMATRIX XMMatrixSet(float m00, float m01, float m02, float m03, float m10, float m11, float m12, float m13,
                                float m20, float m21, float m22, float m23, float m30, float m31, float m32, float m33)
    {
        XMMATRIX mat;
        mat.m[0][0] = m00;
        mat.m[0][1] = m01;
        mat.m[0][2] = m02;
        mat.m[0][3] = m03;
        mat.m[1][0] = m10;
        mat.m[1][1] = m11;
        mat.m[1][2] = m12;
        mat.m[1][3] = m13;
        mat.m[2][0] = m20;
        mat.m[2][1] = m21;
        mat.m[2][2] = m22;
        mat.m[2][3] = m23;
        mat.m[3][0] = m30;
        mat.m[3][1] = m31;
        mat.m[3][2] = m32;
        mat.m[3][3] = m33;
        return mat;
    }

    // Plane operations (used by frustum culling)
    inline XMVECTOR XMPlaneNormalize(XMVECTOR plane)
    {
        float len = sqrtf(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (len > 1e-8f)
        {
            float invLen = 1.0f / len;
            return {plane.x * invLen, plane.y * invLen, plane.z * invLen, plane.w * invLen};
        }
        return plane;
    }

    inline XMVECTOR XMPlaneDotCoord(XMVECTOR plane, XMVECTOR point)
    {
        float d = plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
        return {d, d, d, d};
    }

} // namespace DirectX

#endif // !SPARK_PLATFORM_WINDOWS
