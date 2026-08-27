/**
 * @file LightingSystemInternalLinux.cpp
 * @brief Linux implementation — split from LightingSystemInternal.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "LightingSystem.h"
#include "RHI/RHI.h"

using namespace DirectX;

// ============================================================================
// Private Helpers — Linux/RHI (stubs)
// ============================================================================

HRESULT LightingSystem::CreateConstantBuffers()
{
    return S_OK;
}
HRESULT LightingSystem::CreateShadowMap(uint32_t /*size*/, ShadowMap& /*shadowMap*/)
{
    return S_OK;
}
HRESULT LightingSystem::CreateCascadedShadowMap()
{
    return S_OK;
}
void LightingSystem::UpdateLightBuffer() {}
void LightingSystem::UpdateShadowMaps(const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/) {}
void LightingSystem::CullLights(const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/) {}
void LightingSystem::CalculateCSMSplits(float /*nearPlane*/, float /*farPlane*/, CascadedShadowMap& /*csm*/) {}
XMMATRIX LightingSystem::CalculateLightMatrix(const Light& /*light*/, const XMMATRIX& /*viewMatrix*/,
                                              float /*nearPlane*/, float /*farPlane*/)
{
    return XMMATRIX{};
}
HRESULT LightingSystem::GenerateIrradianceMap(ID3D11ShaderResourceView* /*environmentMap*/)
{
    return S_OK;
}
HRESULT LightingSystem::GeneratePrefilterMap(ID3D11ShaderResourceView* /*environmentMap*/)
{
    return S_OK;
}
HRESULT LightingSystem::GenerateBRDFLUT()
{
    return S_OK;
}
HRESULT LightingSystem::CreateDefaultEnvironment()
{
    return S_OK;
}


#endif // !SPARK_PLATFORM_WINDOWS
