/**
 * @file DDGIProbeSystem.cpp
 * @brief Implementation of DDGI probe system with SH accumulation and trilinear query
 *
 * Moves method bodies out of DDGIProbeSystem.h to reduce header compilation cost.
 * All methods were previously inline in the header; this file is the canonical
 * implementation.
 */

#include "DDGIProbeSystem.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // Public Methods
    // =========================================================================

    bool DDGIProbeSystem::Initialize(const DDGISettings& settings)
    {
        m_settings = settings;
        uint32_t totalProbes = settings.countX * settings.countY * settings.countZ;
        m_probes.resize(totalProbes);
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "DDGIProbeSystem initialized: %ux%ux%u = %u probes (spacing=%.2f,%.2f,%.2f)", settings.countX,
                       settings.countY, settings.countZ, totalProbes, settings.spacingX, settings.spacingY,
                       settings.spacingZ);
        return true;
    }

    void DDGIProbeSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DDGIProbeSystem shutting down (%zu probes)", m_probes.size());
        m_probes.clear();
        m_initialized = false;
    }

    void DDGIProbeSystem::GetProbePosition(uint32_t ix, uint32_t iy, uint32_t iz, float& outX, float& outY,
                                           float& outZ) const
    {
        const DDGIProbe& probe = m_probes[ProbeIndex(ix, iy, iz)];
        outX = m_settings.originX + ix * m_settings.spacingX + probe.offsetX;
        outY = m_settings.originY + iy * m_settings.spacingY + probe.offsetY;
        outZ = m_settings.originZ + iz * m_settings.spacingZ + probe.offsetZ;
    }

    void DDGIProbeSystem::UpdateProbe(uint32_t ix, uint32_t iy, uint32_t iz, const DDGIRayResult* rays, int rayCount)
    {
        if (!m_initialized || rayCount <= 0)
            return;

        uint32_t idx = ProbeIndex(ix, iy, iz);
        DDGIProbe& probe = m_probes[idx];

        // Accumulate new SH from ray results
        std::array<float, DDGI_SH_COEFFICIENTS> newR{};
        std::array<float, DDGI_SH_COEFFICIENTS> newG{};
        std::array<float, DDGI_SH_COEFFICIENTS> newB{};

        float validRayWeight = 0.0f;

        for (int r = 0; r < rayCount; ++r)
        {
            const DDGIRayResult& ray = rays[r];
            if (ray.hitDistance < 0.0f)
                continue; // Miss: contributes sky radiance (assumed zero here)

            // Compute ray direction from spherical Fibonacci distribution
            float dirX, dirY, dirZ;
            SphericalFibonacci(r, rayCount, dirX, dirY, dirZ);

            // Evaluate SH basis for this direction
            float basis[DDGI_SH_COEFFICIENTS];
            EvaluateSHBasis(dirX, dirY, dirZ, basis);

            // Cosine-weight: radiance * max(dot(normal, direction), 0)
            float cosWeight = std::max(0.0f, ray.hitNormalX * dirX + ray.hitNormalY * dirY + ray.hitNormalZ * dirZ);

            validRayWeight += cosWeight;

            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                float bw = basis[c] * cosWeight;
                newR[c] += ray.radianceR * bw;
                newG[c] += ray.radianceG * bw;
                newB[c] += ray.radianceB * bw;
            }
        }

        // Normalize by total weight
        if (validRayWeight > 0.0f)
        {
            float invWeight = (4.0f * 3.14159265f) / validRayWeight;
            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                newR[c] *= invWeight;
                newG[c] *= invWeight;
                newB[c] *= invWeight;
            }
        }

        // Hysteresis blend: lerp(new, old, hysteresis)
        float h = m_settings.hysteresis;
        float oneMinusH = 1.0f - h;
        for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
        {
            probe.r[c] = probe.r[c] * h + newR[c] * oneMinusH;
            probe.g[c] = probe.g[c] * h + newG[c] * oneMinusH;
            probe.b[c] = probe.b[c] * h + newB[c] * oneMinusH;
        }
    }

    void DDGIProbeSystem::UpdateProbeBorders()
    {
        if (!m_initialized)
            return;

        // For edge probes, clamp to nearest interior probe's SH data.
        // This prevents interpolation artifacts at grid boundaries.
        uint32_t cx = m_settings.countX;
        uint32_t cy = m_settings.countY;
        uint32_t cz = m_settings.countZ;

        for (uint32_t iy = 0; iy < cy; ++iy)
        {
            for (uint32_t iz = 0; iz < cz; ++iz)
            {
                CopyProbeData(ProbeIndex(0, iy, iz), ProbeIndex(std::min(1u, cx - 1), iy, iz));
                if (cx > 1)
                {
                    CopyProbeData(ProbeIndex(cx - 1, iy, iz), ProbeIndex(cx - 2, iy, iz));
                }
            }
        }

        for (uint32_t ix = 0; ix < cx; ++ix)
        {
            for (uint32_t iz = 0; iz < cz; ++iz)
            {
                CopyProbeData(ProbeIndex(ix, 0, iz), ProbeIndex(ix, std::min(1u, cy - 1), iz));
                if (cy > 1)
                {
                    CopyProbeData(ProbeIndex(ix, cy - 1, iz), ProbeIndex(ix, cy - 2, iz));
                }
            }
        }

        for (uint32_t ix = 0; ix < cx; ++ix)
        {
            for (uint32_t iy = 0; iy < cy; ++iy)
            {
                CopyProbeData(ProbeIndex(ix, iy, 0), ProbeIndex(ix, iy, std::min(1u, cz - 1)));
                if (cz > 1)
                {
                    CopyProbeData(ProbeIndex(ix, iy, cz - 1), ProbeIndex(ix, iy, cz - 2));
                }
            }
        }
    }

    void DDGIProbeSystem::RelocateProbe(uint32_t ix, uint32_t iy, uint32_t iz, float closestHitDist, float hitNormalX,
                                        float hitNormalY, float hitNormalZ)
    {
        if (!m_initialized)
            return;

        uint32_t idx = ProbeIndex(ix, iy, iz);
        DDGIProbe& probe = m_probes[idx];

        if (closestHitDist < m_settings.relocationThreshold)
        {
            // Push probe along the hit normal, clamped to max offset
            float pushDist =
                std::min(m_settings.relocationThreshold - closestHitDist + 0.1f, m_settings.relocationMaxOffset);

            probe.offsetX += hitNormalX * pushDist;
            probe.offsetY += hitNormalY * pushDist;
            probe.offsetZ += hitNormalZ * pushDist;

            // Clamp total offset magnitude
            float offsetLen = std::sqrt(probe.offsetX * probe.offsetX + probe.offsetY * probe.offsetY +
                                        probe.offsetZ * probe.offsetZ);

            if (offsetLen > m_settings.relocationMaxOffset)
            {
                float scale = m_settings.relocationMaxOffset / offsetLen;
                probe.offsetX *= scale;
                probe.offsetY *= scale;
                probe.offsetZ *= scale;
            }

            probe.relocated = true;
            SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "DDGI probe (%u,%u,%u) relocated: offset=(%.3f,%.3f,%.3f)",
                            ix, iy, iz, probe.offsetX, probe.offsetY, probe.offsetZ);
        }
    }

    DDGIIrradiance DDGIProbeSystem::QueryIrradiance(float worldX, float worldY, float worldZ, float normalX,
                                                    float normalY, float normalZ) const
    {
        DDGIIrradiance result{};
        if (!m_initialized)
            return result;

        // Convert world position to grid-local continuous coordinates (guard against zero spacing)
        float gx = (worldX - m_settings.originX) / std::max(m_settings.spacingX, 0.001f);
        float gy = (worldY - m_settings.originY) / std::max(m_settings.spacingY, 0.001f);
        float gz = (worldZ - m_settings.originZ) / std::max(m_settings.spacingZ, 0.001f);

        // Clamp to valid grid range
        float maxX = static_cast<float>(m_settings.countX - 1);
        float maxY = static_cast<float>(m_settings.countY - 1);
        float maxZ = static_cast<float>(m_settings.countZ - 1);
        gx = std::clamp(gx, 0.0f, maxX);
        gy = std::clamp(gy, 0.0f, maxY);
        gz = std::clamp(gz, 0.0f, maxZ);

        // Integer base indices
        uint32_t ix0 = static_cast<uint32_t>(gx);
        uint32_t iy0 = static_cast<uint32_t>(gy);
        uint32_t iz0 = static_cast<uint32_t>(gz);
        uint32_t ix1 = std::min(ix0 + 1, m_settings.countX - 1);
        uint32_t iy1 = std::min(iy0 + 1, m_settings.countY - 1);
        uint32_t iz1 = std::min(iz0 + 1, m_settings.countZ - 1);

        // Fractional parts for trilinear interpolation
        float fx = gx - ix0;
        float fy = gy - iy0;
        float fz = gz - iz0;

        // Evaluate SH irradiance at all 8 corners and trilinearly interpolate
        float weights[8] = {
            (1 - fx) * (1 - fy) * (1 - fz), fx * (1 - fy) * (1 - fz), (1 - fx) * fy * (1 - fz), fx * fy * (1 - fz),
            (1 - fx) * (1 - fy) * fz,       fx * (1 - fy) * fz,       (1 - fx) * fy * fz,       fx * fy * fz,
        };

        uint32_t indices[8] = {
            ProbeIndex(ix0, iy0, iz0), ProbeIndex(ix1, iy0, iz0), ProbeIndex(ix0, iy1, iz0), ProbeIndex(ix1, iy1, iz0),
            ProbeIndex(ix0, iy0, iz1), ProbeIndex(ix1, iy0, iz1), ProbeIndex(ix0, iy1, iz1), ProbeIndex(ix1, iy1, iz1),
        };

        float basis[DDGI_SH_COEFFICIENTS];
        EvaluateSHBasis(normalX, normalY, normalZ, basis);

        // Irradiance convolution weights (Ramamoorthi & Hanrahan 2001)
        static constexpr float A0 = 3.14159265f;
        static constexpr float A1 = 2.09439510f;
        static constexpr float A2 = 0.78539816f;
        static constexpr float convWeights[DDGI_SH_COEFFICIENTS] = {A0, A1, A1, A1, A2, A2, A2, A2, A2};

        for (int corner = 0; corner < 8; ++corner)
        {
            const DDGIProbe& probe = m_probes[indices[corner]];
            float w = weights[corner];

            float ir = 0.0f, ig = 0.0f, ib = 0.0f;
            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                float bw = basis[c] * convWeights[c];
                ir += probe.r[c] * bw;
                ig += probe.g[c] * bw;
                ib += probe.b[c] * bw;
            }

            result.r += std::max(0.0f, ir) * w;
            result.g += std::max(0.0f, ig) * w;
            result.b += std::max(0.0f, ib) * w;
        }

        return result;
    }

    // =========================================================================
    // Private Helpers
    // =========================================================================

    uint32_t DDGIProbeSystem::ProbeIndex(uint32_t ix, uint32_t iy, uint32_t iz) const
    {
        return iz * m_settings.countX * m_settings.countY + iy * m_settings.countX + ix;
    }

    void DDGIProbeSystem::CopyProbeData(uint32_t dstIdx, uint32_t srcIdx)
    {
        m_probes[dstIdx].r = m_probes[srcIdx].r;
        m_probes[dstIdx].g = m_probes[srcIdx].g;
        m_probes[dstIdx].b = m_probes[srcIdx].b;
    }

    void DDGIProbeSystem::EvaluateSHBasis(float nx, float ny, float nz, float outBasis[DDGI_SH_COEFFICIENTS])
    {
        outBasis[0] = 0.282095f;                        // Y_0^0
        outBasis[1] = 0.488603f * ny;                   // Y_1^-1
        outBasis[2] = 0.488603f * nz;                   // Y_1^0
        outBasis[3] = 0.488603f * nx;                   // Y_1^1
        outBasis[4] = 1.092548f * nx * ny;              // Y_2^-2
        outBasis[5] = 1.092548f * ny * nz;              // Y_2^-1
        outBasis[6] = 0.315392f * (3.0f * nz * nz - 1); // Y_2^0
        outBasis[7] = 1.092548f * nx * nz;              // Y_2^1
        outBasis[8] = 0.546274f * (nx * nx - ny * ny);  // Y_2^2
    }

    void DDGIProbeSystem::SphericalFibonacci(int index, int count, float& outX, float& outY, float& outZ)
    {
        if (count <= 0)
        {
            outX = 0.0f;
            outY = 1.0f;
            outZ = 0.0f;
            return;
        }
        static constexpr float GOLDEN_RATIO = 1.6180339887498948f;
        float theta = 2.0f * 3.14159265f * index / GOLDEN_RATIO;
        float phi = std::acos(1.0f - 2.0f * (index + 0.5f) / count);
        float sinPhi = std::sin(phi);
        outX = std::cos(theta) * sinPhi;
        outY = std::cos(phi);
        outZ = std::sin(theta) * sinPhi;
    }

    // =========================================================================
    // GPU Resource Management
    // =========================================================================

    bool DDGIProbeSystem::CreateGPUResources(Spark::RHI::IRHIDevice* device)
    {
        if (!device)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "DDGIProbeSystem::CreateGPUResources: null device");
            return false;
        }

        if (!m_initialized)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "DDGIProbeSystem::CreateGPUResources: system not initialized");
            return false;
        }

        uint32_t probeCount = GetProbeCount();

        // Create structured buffer for probe SH data
        // Each probe stores 9 coefficients x 3 channels = 27 floats
        uint64_t probeDataSize = static_cast<uint64_t>(probeCount) * DDGI_FLOATS_PER_PROBE * sizeof(float);

        Spark::RHI::RHIBufferDesc probeDesc;
        probeDesc.size = probeDataSize;
        probeDesc.stride = DDGI_FLOATS_PER_PROBE * sizeof(float);
        probeDesc.usage = Spark::RHI::RHIBufferUsage::Structured;
        probeDesc.access = Spark::RHI::RHIBufferAccess::Dynamic;
        probeDesc.initialData = nullptr;
        probeDesc.debugName = "DDGI_ProbeBuffer";

        m_gpuProbeBuffer = device->CreateBuffer(probeDesc);
        if (!m_gpuProbeBuffer)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "DDGIProbeSystem::CreateGPUResources: failed to create probe buffer");
            return false;
        }

        // Create constant buffer for grid configuration
        struct alignas(16) DDGIGridConfigCB
        {
            float spacingX, spacingY, spacingZ;
            uint32_t countX;
            uint32_t countY;
            uint32_t countZ;
            float originX, originY, originZ;
            float normalBias;
            float hysteresis;
            float maxRayDistance;
        };

        DDGIGridConfigCB cbData = {};
        cbData.spacingX = m_settings.spacingX;
        cbData.spacingY = m_settings.spacingY;
        cbData.spacingZ = m_settings.spacingZ;
        cbData.countX = m_settings.countX;
        cbData.countY = m_settings.countY;
        cbData.countZ = m_settings.countZ;
        cbData.originX = m_settings.originX;
        cbData.originY = m_settings.originY;
        cbData.originZ = m_settings.originZ;
        cbData.normalBias = m_settings.normalBias;
        cbData.hysteresis = m_settings.hysteresis;
        cbData.maxRayDistance = m_settings.maxRayDistance;

        Spark::RHI::RHIBufferDesc cbDesc;
        cbDesc.size = sizeof(DDGIGridConfigCB);
        cbDesc.stride = 0;
        cbDesc.usage = Spark::RHI::RHIBufferUsage::Constant;
        cbDesc.access = Spark::RHI::RHIBufferAccess::Dynamic;
        cbDesc.initialData = &cbData;
        cbDesc.debugName = "DDGI_GridConfigCB";

        m_gpuConstantBuffer = device->CreateBuffer(cbDesc);
        if (!m_gpuConstantBuffer)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "DDGIProbeSystem::CreateGPUResources: failed to create constant buffer");
            m_gpuProbeBuffer.reset();
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "DDGIProbeSystem: GPU resources created (%u probes, %u floats/probe, structured buffer + CB)",
                       probeCount, DDGI_FLOATS_PER_PROBE);
        return true;
    }

    void DDGIProbeSystem::UploadProbeDataToGPU(Spark::RHI::IRHIDevice* device)
    {
        if (!device || !m_gpuProbeBuffer)
        {
            return;
        }

        if (!m_initialized || m_probes.empty())
        {
            return;
        }

        // Pack probe SH data into a flat float array (R0..R8, G0..G8, B0..B8 per probe)
        uint32_t probeCount = GetProbeCount();
        std::vector<float> packedData(static_cast<size_t>(probeCount) * DDGI_FLOATS_PER_PROBE);

        for (uint32_t i = 0; i < probeCount; ++i)
        {
            const DDGIProbe& probe = m_probes[i];
            float* dst = &packedData[static_cast<size_t>(i) * DDGI_FLOATS_PER_PROBE];

            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                dst[c] = probe.r[c];
            }
            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                dst[DDGI_SH_COEFFICIENTS + c] = probe.g[c];
            }
            for (int c = 0; c < DDGI_SH_COEFFICIENTS; ++c)
            {
                dst[2 * DDGI_SH_COEFFICIENTS + c] = probe.b[c];
            }
        }

        device->UpdateBuffer(m_gpuProbeBuffer.get(), packedData.data(), packedData.size() * sizeof(float));

        SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "DDGIProbeSystem: uploaded %u probes to GPU", probeCount);
    }

} // namespace Spark::Graphics
