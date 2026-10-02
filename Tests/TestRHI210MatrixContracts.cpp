/**
 * @file TestRHI210MatrixContracts.cpp
 * @brief CPU-only contracts for the RHI-210 D3D11 matrix and culling math.
 */

#include "TestFramework.h"

#include "Core/Platform.h"
#include "Graphics/D3D11FrustumCulling.h"

#include <array>
#include <cmath>

using namespace DirectX;

TEST(RHI210_BasicNormalUploadUsesInverseTransposeForRotatedNonUniformWorld)
{
    const XMMATRIX world = XMMatrixScaling(2.0f, 3.0f, 0.5f) * XMMatrixRotationY(XMConvertToRadians(35.0f));

    // GraphicsDeviceResourcesWindowsBasicState maps the constant buffer directly.
    // D3D11's column-major interpretation supplies the single transpose needed by
    // the embedded basic vertex shader's row-vector normal multiply. Thus
    // CPU upload = inverse, while
    // the shader-visible matrix = transpose(upload) = true inverse transpose.
    const XMMATRIX cpuUpload = Spark::Graphics::D3D11RenderMath::BasicNormalMatrixForUpload(world);
    const XMMATRIX shaderVisible = XMMatrixTranspose(cpuUpload);
    XMFLOAT4X4 actual;
    XMFLOAT4X4 expected;
    XMStoreFloat4x4(&actual, shaderVisible);
    XMStoreFloat4x4(&expected, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
    for (size_t row = 0; row < 4; ++row)
    {
        for (size_t column = 0; column < 4; ++column)
        {
            EXPECT_NEAR(actual.m[row][column], expected.m[row][column], 1.0e-5f);
        }
    }
    const XMVECTOR normal = XMVector3Normalize(XMVectorSet(0.3f, 0.8f, -0.2f, 0.0f));
    const XMVECTOR tangentA = XMVector3Normalize(XMVector3Cross(normal, XMVectorSet(0, 1, 0, 0)));
    const XMVECTOR tangentB = XMVector3Normalize(XMVector3Cross(normal, tangentA));
    const XMVECTOR transformedNormal = XMVector3Normalize(XMVector3TransformNormal(normal, shaderVisible));
    const XMVECTOR transformedTangentA = XMVector3TransformNormal(tangentA, world);
    const XMVECTOR transformedTangentB = XMVector3TransformNormal(tangentB, world);

    EXPECT_NEAR(XMVectorGetX(XMVector3Dot(transformedNormal, transformedTangentA)), 0.0f, 1.0e-5f);
    EXPECT_NEAR(XMVectorGetX(XMVector3Dot(transformedNormal, transformedTangentB)), 0.0f, 1.0e-5f);
    const XMMATRIX oldIncorrectShaderVisible = XMMatrixTranspose(XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
    const XMVECTOR oldNormal = XMVector3Normalize(XMVector3TransformNormal(normal, oldIncorrectShaderVisible));
    EXPECT_GT(std::fabs(XMVectorGetX(XMVector3Dot(oldNormal, transformedTangentA))), 1.0e-3f);
}

TEST(RHI210_D3D11FrustumPlanesUseD3DClipConvention)
{
    const XMMATRIX view =
        XMMatrixLookAtLH(XMVectorSet(0.0f, 2.0f, -20.0f, 1.0f), XMVectorSet(0, 2, 0, 1), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 640.0f / 360.0f, 1.0f, 10.0f);
    const auto planes = Spark::Graphics::D3D11RenderMath::ExtractFrustumPlanes(XMMatrixMultiply(view, projection));
    const XMMATRIX inverseViewProjection = XMMatrixInverse(nullptr, XMMatrixMultiply(view, projection));
    // Construct points in D3D NDC and unproject them. This is independent of
    // plane extraction and keeps each probe inside the other five planes.
    const std::array<XMFLOAT3, 6> inside = {{{-0.99f, 0.0f, 0.5f},
                                             {0.99f, 0.0f, 0.5f},
                                             {0.0f, 0.99f, 0.5f},
                                             {0.0f, -0.99f, 0.5f},
                                             {0.0f, 0.0f, 0.001f},
                                             {0.0f, 0.0f, 0.999f}}};
    const std::array<XMFLOAT3, 6> outside = {{{-1.01f, 0.0f, 0.5f},
                                              {1.01f, 0.0f, 0.5f},
                                              {0.0f, 1.01f, 0.5f},
                                              {0.0f, -1.01f, 0.5f},
                                              {0.0f, 0.0f, -0.001f},
                                              {0.0f, 0.0f, 1.001f}}};
    for (size_t index = 0; index < inside.size(); ++index)
    {
        XMFLOAT3 worldInside;
        XMFLOAT3 worldOutside;
        XMStoreFloat3(&worldInside, XMVector3TransformCoord(XMLoadFloat3(&inside[index]), inverseViewProjection));
        XMStoreFloat3(&worldOutside, XMVector3TransformCoord(XMLoadFloat3(&outside[index]), inverseViewProjection));
        EXPECT_TRUE(Spark::Graphics::D3D11RenderMath::SphereIntersects(planes, worldInside, 0.0f));
        EXPECT_FALSE(Spark::Graphics::D3D11RenderMath::SphereIntersects(planes, worldOutside, 0.0f));
    }
}

TEST(RHI210_FPSArenaObjectsRemainVisibleWithColumnPlanes)
{
    const XMFLOAT3 eye{0.0f, 2.0f, -20.0f};
    const XMFLOAT3 target{0.0f, 2.0f, 0.0f};
    const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 640.0f / 360.0f, 0.1f, 1000.0f);
    const auto planes = Spark::Graphics::D3D11RenderMath::ExtractFrustumPlanes(XMMatrixMultiply(view, projection));

    // Authored positions from Assets/Scenes/level1.scene.  These three objects
    // were among the content lost by the former Windows row extraction.
    const std::array<XMFLOAT3, 3> arenaObjects = {
        XMFLOAT3{12.0f, 1.5f, -12.0f},  // Pillar_SE
        XMFLOAT3{-12.0f, 1.5f, -12.0f}, // Pillar_SW
        XMFLOAT3{0.0f, 1.0f, -15.0f},   // Mid_Barrier_S
    };
    for (const XMFLOAT3& position : arenaObjects)
    {
        EXPECT_TRUE(Spark::Graphics::D3D11RenderMath::SphereIntersects(planes, position, 5.0f));
    }
}
