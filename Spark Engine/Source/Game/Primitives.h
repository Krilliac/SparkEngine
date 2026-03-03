// Primitives.h
#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include "../Graphics/Mesh.h"    // MeshData
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT2;

// Procedural primitive mesh generators
namespace Primitives
{
    MeshData CreateCube(float size = 1.0f);
    MeshData CreatePlane(float width = 1.0f, float depth = 1.0f);
    MeshData CreateSphere(float radius = 0.5f, int slices = 16, int stacks = 16);
}
