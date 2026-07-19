/**
 * @file LightingSystemInternalWindowsCulling.cpp
 * @brief Windows/D3D11 light culling and shadow matrix math for LightingSystem
 *
 * CullLights / CalculateCSMSplits / CalculateLightMatrix split out of
 * LightingSystemInternalWindows.cpp (which keeps constant buffer creation,
 * shadow map management, and per-frame buffer updates). Linux stubs live in
 * LightingSystemInternalLinux.cpp.
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"
#include "../Utils/LogMacros.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

void LightingSystem::CullLights(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    // Build the six frustum planes from the view-projection matrix
    // Planes are extracted from the combined VP matrix in clip space
    XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);

    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, viewProj);

    // Extract frustum planes (Gribb/Hartmann method)
    // Each plane as (A, B, C, D) where Ax + By + Cz + D >= 0 means inside
    XMFLOAT4 planes[6];

    // Left plane
    planes[0] = XMFLOAT4(vp._14 + vp._11, vp._24 + vp._21, vp._34 + vp._31, vp._44 + vp._41);

    // Right plane
    planes[1] = XMFLOAT4(vp._14 - vp._11, vp._24 - vp._21, vp._34 - vp._31, vp._44 - vp._41);

    // Bottom plane
    planes[2] = XMFLOAT4(vp._14 + vp._12, vp._24 + vp._22, vp._34 + vp._32, vp._44 + vp._42);

    // Top plane
    planes[3] = XMFLOAT4(vp._14 - vp._12, vp._24 - vp._22, vp._34 - vp._32, vp._44 - vp._42);

    // Near plane
    planes[4] = XMFLOAT4(vp._13, vp._23, vp._33, vp._43);

    // Far plane
    planes[5] = XMFLOAT4(vp._14 - vp._13, vp._24 - vp._23, vp._34 - vp._33, vp._44 - vp._43);

    // Normalize all planes
    for (int i = 0; i < 6; ++i)
    {
        XMVECTOR planeVec = XMLoadFloat4(&planes[i]);
        XMVECTOR normalLength = XMVector3Length(XMVectorSet(planes[i].x, planes[i].y, planes[i].z, 0.0f));
        float len = XMVectorGetX(normalLength);
        if (len > 0.0f)
        {
            planes[i].x /= len;
            planes[i].y /= len;
            planes[i].z /= len;
            planes[i].w /= len;
        }
    }

    // Rebuild light data array with only visible lights
    m_lightDataArray.clear();
    m_lightDataArray.reserve(m_lights.size());
    uint32_t visibleCount = 0;

    for (const auto& light : m_lights)
    {
        if (!light || !light->IsEnabled())
        {
            continue;
        }

        bool visible = true;

        // Directional and environment lights are always visible
        if (light->GetType() == LightType::Point || light->GetType() == LightType::Spot ||
            light->GetType() == LightType::Area)
        {
            const XMFLOAT3& pos = light->GetPosition();
            float range = light->GetRange();

            // Test the light's bounding sphere against each frustum plane
            for (int i = 0; i < 6; ++i)
            {
                float distance = planes[i].x * pos.x + planes[i].y * pos.y + planes[i].z * pos.z + planes[i].w;

                // If the sphere is entirely behind any plane, cull it
                if (distance < -range)
                {
                    visible = false;
                    break;
                }
            }
        }

        if (visible)
        {
            m_lightDataArray.push_back(light->GetShaderData());
            visibleCount++;
        }
    }

    m_metrics.visibleLights = visibleCount;
    m_metrics.culledLights = m_metrics.activeLights - visibleCount;
    SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "LightCull: %u visible, %u culled of %u total", visibleCount,
                    m_metrics.culledLights, m_metrics.activeLights);
}

void LightingSystem::CalculateCSMSplits(float nearPlane, float farPlane, CascadedShadowMap& csm)
{
    csm.splitDistances.clear();
    csm.splitDistances.resize(csm.cascadeCount + 1);

    for (uint32_t i = 0; i < csm.cascadeCount; ++i)
    {
        float p = static_cast<float>(i + 1) / static_cast<float>(csm.cascadeCount);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float uniform = nearPlane + (farPlane - nearPlane) * p;
        float d = csm.splitLambda * (log - uniform) + uniform;
        csm.splitDistances[i + 1] = d;
    }

    csm.splitDistances[0] = nearPlane;
}

XMMATRIX LightingSystem::CalculateLightMatrix(const Light& light, const XMMATRIX& viewMatrix, float nearPlane,
                                              float farPlane)
{
    // For non-directional lights, use the basic light matrix
    if (light.GetType() != LightType::Directional)
    {
        return light.GetLightMatrix();
    }

    // For directional lights, compute a tight-fitting orthographic projection
    // that encompasses the view frustum slice [nearPlane, farPlane].

    // Step 1: Build the inverse view-projection for the frustum slice
    // Use a temporary projection matrix matching the slice
    // We assume a symmetric perspective projection; extract aspect and fov
    // from the current projection indirectly, but we only need the 8 frustum
    // corners in world space. We reconstruct them from the inverse view matrix
    // and the near/far distances with a standard FPS FOV.

    // Compute the 8 corners of the frustum slice in world space
    // NDC corners: x,y in {-1,+1}, z in {0,1} (LH)
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);

    // Extract forward, right, up from the inverse view matrix
    XMVECTOR camRight = invView.r[0];
    XMVECTOR camUp = invView.r[1];
    XMVECTOR camForward = invView.r[2];
    XMVECTOR camPos = invView.r[3];

    // Use a default FOV of 60 degrees and 16:9 aspect if we cannot extract them
    float fovY = XM_PI / 3.0f;
    float aspect = 16.0f / 9.0f;

    float nearHeight = 2.0f * nearPlane * std::tan(fovY * 0.5f);
    float nearWidth = nearHeight * aspect;
    float farHeight = 2.0f * farPlane * std::tan(fovY * 0.5f);
    float farWidth = farHeight * aspect;

    XMVECTOR nearCenter = XMVectorAdd(camPos, XMVectorScale(camForward, nearPlane));
    XMVECTOR farCenter = XMVectorAdd(camPos, XMVectorScale(camForward, farPlane));

    // Compute the 8 frustum corners
    XMVECTOR frustumCorners[8];
    // Near plane corners
    frustumCorners[0] = XMVectorAdd(
        nearCenter, XMVectorAdd(XMVectorScale(camUp, nearHeight * 0.5f), XMVectorScale(camRight, -nearWidth * 0.5f)));
    frustumCorners[1] = XMVectorAdd(
        nearCenter, XMVectorAdd(XMVectorScale(camUp, nearHeight * 0.5f), XMVectorScale(camRight, nearWidth * 0.5f)));
    frustumCorners[2] = XMVectorAdd(
        nearCenter, XMVectorAdd(XMVectorScale(camUp, -nearHeight * 0.5f), XMVectorScale(camRight, -nearWidth * 0.5f)));
    frustumCorners[3] = XMVectorAdd(
        nearCenter, XMVectorAdd(XMVectorScale(camUp, -nearHeight * 0.5f), XMVectorScale(camRight, nearWidth * 0.5f)));
    // Far plane corners
    frustumCorners[4] = XMVectorAdd(
        farCenter, XMVectorAdd(XMVectorScale(camUp, farHeight * 0.5f), XMVectorScale(camRight, -farWidth * 0.5f)));
    frustumCorners[5] = XMVectorAdd(
        farCenter, XMVectorAdd(XMVectorScale(camUp, farHeight * 0.5f), XMVectorScale(camRight, farWidth * 0.5f)));
    frustumCorners[6] = XMVectorAdd(
        farCenter, XMVectorAdd(XMVectorScale(camUp, -farHeight * 0.5f), XMVectorScale(camRight, -farWidth * 0.5f)));
    frustumCorners[7] = XMVectorAdd(
        farCenter, XMVectorAdd(XMVectorScale(camUp, -farHeight * 0.5f), XMVectorScale(camRight, farWidth * 0.5f)));

    // Step 2: Compute the frustum centroid
    XMVECTOR centroid = XMVectorZero();
    for (int i = 0; i < 8; ++i)
    {
        centroid = XMVectorAdd(centroid, frustumCorners[i]);
    }
    centroid = XMVectorScale(centroid, 1.0f / 8.0f);

    // Step 3: Build the light view matrix looking at the centroid
    const XMFLOAT3& lightDir = light.GetDirection();
    XMVECTOR lightDirVec = XMVector3Normalize(XMLoadFloat3(&lightDir));

    // Place the light far enough back along the light direction
    XMVECTOR lightPos = XMVectorSubtract(centroid, XMVectorScale(lightDirVec, farPlane));

    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    // If light direction is nearly parallel to up, choose a different up vector
    if (std::abs(XMVectorGetX(XMVector3Dot(lightDirVec, lightUp))) > 0.99f)
    {
        lightUp = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }

    XMVECTOR lightTarget = XMVectorAdd(lightPos, lightDirVec);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, lightTarget, lightUp);

    // Step 4: Transform frustum corners into light space and find AABB
    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    float minZ = FLT_MAX, maxZ = -FLT_MAX;

    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR transformed = XMVector3TransformCoord(frustumCorners[i], lightView);
        float tx = XMVectorGetX(transformed);
        float ty = XMVectorGetY(transformed);
        float tz = XMVectorGetZ(transformed);

        minX = std::min(minX, tx);
        maxX = std::max(maxX, tx);
        minY = std::min(minY, ty);
        maxY = std::max(maxY, ty);
        minZ = std::min(minZ, tz);
        maxZ = std::max(maxZ, tz);
    }

    // Expand the Z range to capture shadow casters behind the camera frustum
    float zRange = maxZ - minZ;
    minZ -= zRange * 0.5f;

    // Step 5: Build an orthographic projection from the light-space AABB
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);

    return XMMatrixMultiply(lightView, lightProj);
}

#endif // SPARK_PLATFORM_WINDOWS
