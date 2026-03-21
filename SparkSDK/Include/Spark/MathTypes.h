/**
 * @file MathTypes.h
 * @brief Common math type definitions for game modules
 *
 * Provides lightweight math types that game modules can use for
 * positions, rotations, colors, and transforms without depending
 * on DirectXMath or the engine's full Platform.h.
 *
 * On Windows, the engine internally uses DirectXMath (XMFLOAT3, etc.).
 * These SDK types are layout-compatible and can be reinterpret_cast'd
 * to/from their DirectXMath counterparts when needed.
 *
 * ## Usage
 * @code
 *   Spark::Vec3 playerPos{10.0f, 0.0f, -5.0f};
 *   Spark::Color healthColor{1.0f, 0.0f, 0.0f, 1.0f};
 *   Spark::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};  // identity
 * @endcode
 */

#pragma once

namespace Spark
{

    /**
     * @brief 2D floating-point vector
     */
    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    /**
     * @brief 3D floating-point vector (position, direction, scale)
     *
     * Layout-compatible with DirectX::XMFLOAT3.
     */
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /**
     * @brief 4D floating-point vector
     *
     * Layout-compatible with DirectX::XMFLOAT4.
     */
    struct Vec4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };

    /**
     * @brief Quaternion rotation (x, y, z, w)
     *
     * Layout-compatible with DirectX::XMFLOAT4. The w component
     * is the scalar part. Identity quaternion: {0, 0, 0, 1}.
     */
    struct Quat
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    /**
     * @brief RGBA color with floating-point components [0.0, 1.0]
     */
    struct Color
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    /**
     * @brief 4x4 row-major transformation matrix
     *
     * Layout-compatible with DirectX::XMFLOAT4X4.
     */
    struct Mat4x4
    {
        float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    };

    /**
     * @brief Axis-aligned bounding box
     */
    struct AABB
    {
        Vec3 min{};
        Vec3 max{};
    };

    /**
     * @brief 3D ray (origin + direction)
     */
    struct Ray
    {
        Vec3 origin{};
        Vec3 direction{0.0f, 0.0f, 1.0f};
    };

} // namespace Spark
