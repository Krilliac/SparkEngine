/**
 * @file ProbeSystem.cpp
 * @brief Irradiance probe grid management and compute dispatch
 *
 * Manages a 3D grid of irradiance probes that cache GI via spherical harmonics.
 * Probes are placed on a regular grid and updated incrementally each frame by
 * dispatching ProbeUpdate.hlsl (sphere tracing from probe positions).
 *
 * Uses SSAOKernel::GenerateKernel() from ScreenSpaceEffects for generating
 * well-distributed hemisphere sample directions for probe ray tracing.
 */

#include "ProbeSystem.h"
#include "../RHI/RHIDevice.h"
#include "../RHI/RHIResources.h"
#include "../../Utils/LogMacros.h"

#include <algorithm>

namespace Spark::Graphics
{

    ProbeSystem::~ProbeSystem()
    {
        Shutdown();
    }

    bool ProbeSystem::Initialize(RHI::IRHIDevice* device, const ProbeGridConfig& config)
    {
        if (!device)
            return false;

        m_device = device;
        m_config = config;

        uint32_t probeCount = GetProbeCount();
        if (probeCount == 0)
            return false;

        // Create probe structured buffer (RW for compute update)
        RHI::RHIBufferDesc probeBufDesc;
        probeBufDesc.size = sizeof(ProbeData) * probeCount;
        probeBufDesc.stride = sizeof(ProbeData);
        probeBufDesc.usage = RHI::RHIBufferUsage::Structured | RHI::RHIBufferUsage::Storage;
        probeBufDesc.access = RHI::RHIBufferAccess::Dynamic;
        probeBufDesc.debugName = "ProbeGrid_Buffer";
        m_probeBuffer = device->CreateBuffer(probeBufDesc);
        if (!m_probeBuffer)
            return false;

        // Constant buffers for update and interpolation
        RHI::RHIBufferDesc cbDesc;
        cbDesc.size = 256; // Padded to 256 for alignment
        cbDesc.usage = RHI::RHIBufferUsage::Constant;
        cbDesc.access = RHI::RHIBufferAccess::Dynamic;

        cbDesc.debugName = "ProbeUpdate_Constants";
        m_updateConstants = device->CreateBuffer(cbDesc);

        cbDesc.debugName = "ProbeInterpolate_Constants";
        m_interpConstants = device->CreateBuffer(cbDesc);

        // Load compute shaders
        RHI::RHIShaderDesc updateCS;
        updateCS.stage = RHI::RHIShaderStage::Compute;
        updateCS.filePath = "Shaders/HLSL/RayTracing/ProbeUpdate.hlsl";
        updateCS.entryPoint = "CSUpdateProbes";
        updateCS.debugName = "ProbeUpdate_CS";
        m_updateCS = device->CreateShader(updateCS);

        RHI::RHIShaderDesc interpCS;
        interpCS.stage = RHI::RHIShaderStage::Compute;
        interpCS.filePath = "Shaders/HLSL/RayTracing/ProbeInterpolate.hlsl";
        interpCS.entryPoint = "CSInterpolateProbes";
        interpCS.debugName = "ProbeInterpolate_CS";
        m_interpolateCS = device->CreateShader(interpCS);

        // Generate hemisphere sample directions using existing SSAOKernel infrastructure
        // These are used as the ray directions for probe updates
        m_sampleDirections = SSAOKernel::GenerateKernel(config.raysPerProbe, 7919);

        // Build initial probe grid positions
        BuildProbeGrid();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "ProbeSystem initialized: %dx%dx%d grid (%u probes), spacing=%.1f, %d rays/probe",
                       m_config.dimensionsX, m_config.dimensionsY, m_config.dimensionsZ, GetProbeCount(),
                       m_config.spacing, m_config.raysPerProbe);
        return true;
    }

    void ProbeSystem::Shutdown()
    {
        if (!m_device)
            return;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shutting down ProbeSystem");

        m_probeBuffer.reset();
        m_updateConstants.reset();
        m_interpConstants.reset();
        m_updateCS.reset();
        m_interpolateCS.reset();
        m_device = nullptr;
        m_initialized = false;
    }

    uint32_t ProbeSystem::GetProbeCount() const
    {
        return static_cast<uint32_t>(m_config.dimensionsX) * static_cast<uint32_t>(m_config.dimensionsY) *
               static_cast<uint32_t>(m_config.dimensionsZ);
    }

    void ProbeSystem::UpdateGridOrigin(const DirectX::XMFLOAT3& cameraPos)
    {
        // Snap grid origin to spacing increments centered around camera
        float halfX = m_config.spacing * m_config.dimensionsX * 0.5f;
        float halfY = m_config.spacing * m_config.dimensionsY * 0.5f;
        float halfZ = m_config.spacing * m_config.dimensionsZ * 0.5f;

        // Snap to grid (guard against zero spacing)
        float safeSpacing = std::max(m_config.spacing, 0.001f);
        m_config.origin.x = std::floor((cameraPos.x - halfX) / safeSpacing) * safeSpacing;
        m_config.origin.y = std::floor((cameraPos.y - halfY) / safeSpacing) * safeSpacing;
        m_config.origin.z = std::floor((cameraPos.z - halfZ) / safeSpacing) * safeSpacing;

        // Rebuild grid with new origin
        BuildProbeGrid();
    }

    void ProbeSystem::BuildProbeGrid()
    {
        uint32_t probeCount = GetProbeCount();
        std::vector<ProbeData> probes(probeCount);

        uint32_t idx = 0;
        for (int32_t z = 0; z < m_config.dimensionsZ; ++z)
        {
            for (int32_t y = 0; y < m_config.dimensionsY; ++y)
            {
                for (int32_t x = 0; x < m_config.dimensionsX; ++x)
                {
                    probes[idx].position = {m_config.origin.x + static_cast<float>(x) * m_config.spacing,
                                            m_config.origin.y + static_cast<float>(y) * m_config.spacing,
                                            m_config.origin.z + static_cast<float>(z) * m_config.spacing};
                    probes[idx].padding = 0.0f;
                    // SH coefficients start at zero — will be filled by update passes
                    for (auto& sh : probes[idx].shCoeffs)
                        sh = {0, 0, 0, 0};
                    ++idx;
                }
            }
        }

        if (m_probeBuffer && m_device)
        {
            m_device->UpdateBuffer(m_probeBuffer.get(), probes.data(), sizeof(ProbeData) * probeCount);
        }
    }

    void ProbeSystem::UpdateProbes(RHI::IRHICommandList* cmd, const DirectX::XMFLOAT3& lightDir, float lightIntensity)
    {
        if (!m_initialized || !cmd || !m_updateCS)
            return;
        SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "Updating %u probes (lightIntensity=%.2f)", GetProbeCount(),
                        lightIntensity);

        cmd->BeginEvent("ProbeUpdate");

        // Pack constant buffer
        struct
        {
            int32_t probeCount;
            int32_t primitiveCount;
            int32_t raysPerProbe;
            float maxTraceDistance;
            DirectX::XMFLOAT3 lightDir;
            float lightIntensity;
            float hysteresis;
            DirectX::XMFLOAT3 pad;
        } constants = {};

        constants.probeCount = static_cast<int32_t>(GetProbeCount());
        constants.primitiveCount = 0; // Set by caller via SDF scene binding
        constants.raysPerProbe = m_config.raysPerProbe;
        constants.maxTraceDistance = 50.0f;
        constants.lightDir = lightDir;
        constants.lightIntensity = lightIntensity;
        constants.hysteresis = m_config.hysteresis;
        constants.pad = {0, 0, 0};

        m_device->UpdateBuffer(m_updateConstants.get(), &constants, sizeof(constants));
        cmd->SetConstantBuffer(RHI::RHIShaderStage::Compute, 0, m_updateConstants.get());

        // Dispatch: ceil(probeCount / 64) x 1 x 1
        uint32_t dispatchX = (GetProbeCount() + 63) / 64;
        cmd->Dispatch(dispatchX, 1, 1);

        cmd->EndEvent();
    }

    void ProbeSystem::InterpolateProbes(RHI::IRHICommandList* cmd, RHI::IRHITexture* gbufferNormals,
                                        RHI::IRHITexture* gbufferDepth, const DirectX::XMFLOAT4X4& invViewProj,
                                        float giIntensity, uint32_t screenWidth, uint32_t screenHeight,
                                        RHI::IRHITexture* output)
    {
        if (!m_initialized || !cmd || !m_interpolateCS || !output)
            return;

        cmd->BeginEvent("ProbeInterpolate");

        // Pack constant buffer (matches ProbeInterpolateConstants in HLSL)
        struct
        {
            DirectX::XMFLOAT4X4 invViewProj;
            DirectX::XMFLOAT3 gridOrigin;
            float gridSpacing;
            int32_t gridDimX;
            int32_t gridDimY;
            int32_t gridDimZ;
            float giIntensity;
            float screenWidth;
            float screenHeight;
            float pad[2];
        } constants = {};

        constants.invViewProj = invViewProj;
        constants.gridOrigin = m_config.origin;
        constants.gridSpacing = m_config.spacing;
        constants.gridDimX = m_config.dimensionsX;
        constants.gridDimY = m_config.dimensionsY;
        constants.gridDimZ = m_config.dimensionsZ;
        constants.giIntensity = giIntensity;
        constants.screenWidth = static_cast<float>(screenWidth);
        constants.screenHeight = static_cast<float>(screenHeight);
        constants.pad[0] = constants.pad[1] = 0.0f;

        m_device->UpdateBuffer(m_interpConstants.get(), &constants, sizeof(constants));
        cmd->SetConstantBuffer(RHI::RHIShaderStage::Compute, 0, m_interpConstants.get());
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 1, gbufferNormals);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 2, gbufferDepth);

        uint32_t dispatchX = (screenWidth + 7) / 8;
        uint32_t dispatchY = (screenHeight + 7) / 8;
        cmd->Dispatch(dispatchX, dispatchY, 1);

        cmd->EndEvent();
    }

} // namespace Spark::Graphics
