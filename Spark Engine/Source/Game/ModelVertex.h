// ModelVertex.h
#pragma once
#include "../Core/Platform.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cmath>               // add this for std::isfinite

struct ModelVertex
{
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT2 TexCoord;

    ModelVertex() noexcept
        : Position(0.f, 0.f, 0.f)
        , Normal(0.f, 0.f, 0.f)
        , TexCoord(0.f, 0.f) {
    }

    ModelVertex(const DirectX::XMFLOAT3& pos,
        const DirectX::XMFLOAT3& norm,
        const DirectX::XMFLOAT2& uv) noexcept
        : Position(pos)
        , Normal(norm)
        , TexCoord(uv)
    {
        // Validate incoming data
        ASSERT(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z));
        ASSERT(std::isfinite(norm.x) && std::isfinite(norm.y) && std::isfinite(norm.z));
        ASSERT(std::isfinite(uv.x) && std::isfinite(uv.y));
    }
};