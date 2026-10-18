/**
 * @file D3D11FrustumCulling.h
 * @brief Shared Direct3D matrix and clip-space math for CPU/render contracts.
 *
 * Contract: these pure helpers are thread-safe and allocation-free. They run
 * on the caller's thread, borrow no engine state, and are intended for the
 * render thread's per-frame culling path and CPU-only regression tests.
 */

#pragma once

#include "../Core/Platform.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Spark::Graphics::D3D11RenderMath
{
    /**
     * Return the CPU-side matrix expected by the direct D3D11 basic constant
     * upload. HLSL column-major interpretation supplies the final transpose,
     * so the shader sees the true inverse-transpose normal transform.
     */
    inline DirectX::XMMATRIX BasicNormalMatrixForUpload(const DirectX::XMMATRIX& world) noexcept
    {
        return DirectX::XMMatrixInverse(nullptr, world);
    }

    enum class PlaneIndex : std::uint8_t
    {
        Left = 0,
        Right,
        Top,
        Bottom,
        Near,
        Far,
        Count
    };

    using FrustumPlanes = std::array<DirectX::XMFLOAT4, static_cast<size_t>(PlaneIndex::Count)>;

    /**
     * Extract normalized inward-facing planes for a row-vector D3D clip volume.
     * The view-projection columns are used because DirectXMath multiplies row
     * vectors; the near plane is c2 for the D3D 0 <= z <= w depth range.
     */
    inline FrustumPlanes ExtractFrustumPlanes(const DirectX::XMMATRIX& viewProjection) noexcept
    {
        DirectX::XMFLOAT4X4 matrix{};
        DirectX::XMStoreFloat4x4(&matrix, viewProjection);
        const auto column = [&matrix](size_t index) noexcept
        { return DirectX::XMFLOAT4{matrix.m[0][index], matrix.m[1][index], matrix.m[2][index], matrix.m[3][index]}; };

        const DirectX::XMFLOAT4 c0 = column(0);
        const DirectX::XMFLOAT4 c1 = column(1);
        const DirectX::XMFLOAT4 c2 = column(2);
        const DirectX::XMFLOAT4 c3 = column(3);
        FrustumPlanes planes{
            DirectX::XMFLOAT4{c3.x + c0.x, c3.y + c0.y, c3.z + c0.z, c3.w + c0.w},
            DirectX::XMFLOAT4{c3.x - c0.x, c3.y - c0.y, c3.z - c0.z, c3.w - c0.w},
            DirectX::XMFLOAT4{c3.x - c1.x, c3.y - c1.y, c3.z - c1.z, c3.w - c1.w},
            DirectX::XMFLOAT4{c3.x + c1.x, c3.y + c1.y, c3.z + c1.z, c3.w + c1.w},
            c2,
            DirectX::XMFLOAT4{c3.x - c2.x, c3.y - c2.y, c3.z - c2.z, c3.w - c2.w},
        };

        for (DirectX::XMFLOAT4& plane : planes)
        {
            const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
            if (length > 0.0f)
            {
                const float inverseLength = 1.0f / length;
                plane.x *= inverseLength;
                plane.y *= inverseLength;
                plane.z *= inverseLength;
                plane.w *= inverseLength;
            }
        }
        return planes;
    }

    inline float SignedDistance(const DirectX::XMFLOAT4& plane, const DirectX::XMFLOAT3& point) noexcept
    {
        return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
    }

    inline bool SphereIntersects(const FrustumPlanes& planes, const DirectX::XMFLOAT3& center, float radius) noexcept
    {
        for (const DirectX::XMFLOAT4& plane : planes)
        {
            if (SignedDistance(plane, center) < -radius)
            {
                return false;
            }
        }
        return true;
    }
} // namespace Spark::Graphics::D3D11RenderMath
