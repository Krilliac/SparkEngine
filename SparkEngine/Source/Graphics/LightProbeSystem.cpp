/**
 * @file LightProbeSystem.cpp
 * @brief Light probe management with SH interpolation and GPU data packing
 */

#include "LightProbeSystem.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // SH Basis Evaluation (L2 = 9 coefficients)
    // =========================================================================

    /// @brief SH basis constants (Ramamoorthi & Hanrahan 2001)
    static constexpr float SH_C0 = 0.282095f;   // 1 / (2 * sqrt(pi))
    static constexpr float SH_C1 = 0.488603f;   // sqrt(3) / (2 * sqrt(pi))
    static constexpr float SH_C2_0 = 1.092548f; // sqrt(15) / (2 * sqrt(pi))
    static constexpr float SH_C2_1 = 0.315392f; // sqrt(5) / (4 * sqrt(pi))
    static constexpr float SH_C2_2 = 0.546274f; // sqrt(15) / (4 * sqrt(pi))

    /// @brief Evaluate L2 SH basis for direction (nx, ny, nz)
    static void EvaluateSHBasis(float nx, float ny, float nz, float out[9])
    {
        out[0] = SH_C0;
        out[1] = SH_C1 * ny;
        out[2] = SH_C1 * nz;
        out[3] = SH_C1 * nx;
        out[4] = SH_C2_0 * nx * ny;
        out[5] = SH_C2_0 * ny * nz;
        out[6] = SH_C2_1 * (3.0f * nz * nz - 1.0f);
        out[7] = SH_C2_0 * nx * nz;
        out[8] = SH_C2_2 * (nx * nx - ny * ny);
    }

    // =========================================================================
    // SphericalHarmonics struct methods
    // =========================================================================

    SphericalHarmonics SphericalHarmonics::Lerp(const SphericalHarmonics& a, const SphericalHarmonics& b, float t)
    {
        SphericalHarmonics result;
        for (size_t i = 0; i < 9; ++i)
        {
            result.coefficients[i].x = a.coefficients[i].x + (b.coefficients[i].x - a.coefficients[i].x) * t;
            result.coefficients[i].y = a.coefficients[i].y + (b.coefficients[i].y - a.coefficients[i].y) * t;
            result.coefficients[i].z = a.coefficients[i].z + (b.coefficients[i].z - a.coefficients[i].z) * t;
        }
        return result;
    }

    void SphericalHarmonics::Scale(float factor)
    {
        for (auto& c : coefficients)
        {
            c.x *= factor;
            c.y *= factor;
            c.z *= factor;
        }
    }

    void SphericalHarmonics::Accumulate(const SphericalHarmonics& other, float weight)
    {
        for (size_t i = 0; i < 9; ++i)
        {
            coefficients[i].x += other.coefficients[i].x * weight;
            coefficients[i].y += other.coefficients[i].y * weight;
            coefficients[i].z += other.coefficients[i].z * weight;
        }
    }

    // =========================================================================
    // Utility
    // =========================================================================

    static float Distance(const Float3& a, const Float3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // =========================================================================
    // LightProbeSystem
    // =========================================================================

    bool LightProbeSystem::Initialize()
    {
        m_probes.clear();
        m_nextProbeId = 1;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightProbeSystem initialized");
        return true;
    }

    uint32_t LightProbeSystem::AddProbe(const LightProbe& probe)
    {
        uint32_t id = m_nextProbeId++;
        m_probes[id] = probe;
        return id;
    }

    void LightProbeSystem::RemoveProbe(uint32_t probeId)
    {
        m_probes.erase(probeId);
    }

    void LightProbeSystem::SetProbeGrid(const ProbeGrid& grid)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightProbeSystem: setting probe grid %ux%ux%u (%u probes)",
                       grid.countX, grid.countY, grid.countZ, grid.countX * grid.countY * grid.countZ);
        for (uint32_t z = 0; z < grid.countZ; ++z)
        {
            for (uint32_t y = 0; y < grid.countY; ++y)
            {
                for (uint32_t x = 0; x < grid.countX; ++x)
                {
                    LightProbe probe;
                    probe.position.x = grid.origin.x + static_cast<float>(x) * grid.cellSize.x;
                    probe.position.y = grid.origin.y + static_cast<float>(y) * grid.cellSize.y;
                    probe.position.z = grid.origin.z + static_cast<float>(z) * grid.cellSize.z;
                    probe.influenceRadius = std::max({grid.cellSize.x, grid.cellSize.y, grid.cellSize.z}) * 2.0f;
                    AddProbe(probe);
                }
            }
        }
    }

    void LightProbeSystem::AutoPlaceProbes(const Float3& boundsMin, const Float3& boundsMax, float spacing)
    {
        if (spacing <= 0.0f)
            return;

        ProbeGrid grid;
        grid.origin = boundsMin;
        grid.cellSize = {spacing, spacing, spacing};
        grid.countX = std::min(static_cast<uint32_t>((boundsMax.x - boundsMin.x) / spacing) + 1, 32u);
        grid.countY = std::min(static_cast<uint32_t>((boundsMax.y - boundsMin.y) / spacing) + 1, 16u);
        grid.countZ = std::min(static_cast<uint32_t>((boundsMax.z - boundsMin.z) / spacing) + 1, 32u);
        SetProbeGrid(grid);
    }

    SphericalHarmonics LightProbeSystem::SampleIrradiance(const Float3& position) const
    {
        SphericalHarmonics result{};
        float totalWeight = 0.0f;

        for (const auto& [id, probe] : m_probes)
        {
            float dist = Distance(position, probe.position);
            if (dist > probe.influenceRadius)
                continue;

            float normalizedDist = dist / probe.influenceRadius;
            float weight = 1.0f - normalizedDist;
            weight = weight * weight; // Smooth falloff
            result.Accumulate(probe.sh, weight);
            totalWeight += weight;
        }

        if (totalWeight > 0.0f)
            result.Scale(1.0f / totalWeight);

        return result;
    }

    void LightProbeSystem::BakeProbe(uint32_t probeId)
    {
        // Default sky/ground bake: blue sky, brown ground, sun from upper-right
        Float3 skyColor{0.4f, 0.6f, 0.9f};
        Float3 groundColor{0.15f, 0.12f, 0.08f};
        Float3 sunDir{0.5f, 0.707f, 0.5f}; // Normalized ~45 degrees
        Float3 sunColor{1.2f, 1.1f, 0.9f};
        BakeProbe(probeId, skyColor, groundColor, sunDir, sunColor);
    }

    void LightProbeSystem::BakeProbe(uint32_t probeId, const Float3& skyColor, const Float3& groundColor,
                                     const Float3& sunDir, const Float3& sunColor)
    {
        auto it = m_probes.find(probeId);
        if (it == m_probes.end())
            return;

        SphericalHarmonics& sh = it->second.sh;
        sh = SphericalHarmonics{};

        // Generate SH by sampling a hemisphere-split sky/ground + directional sun.
        // Use spherical Fibonacci for quasi-uniform sampling over the sphere.
        static constexpr int SAMPLE_COUNT = 64;
        static constexpr float GOLDEN_RATIO = 1.6180339887498948f;
        static constexpr float PI = 3.14159265f;
        static constexpr float FOUR_PI = 4.0f * PI;

        for (int i = 0; i < SAMPLE_COUNT; ++i)
        {
            float theta = 2.0f * PI * i / GOLDEN_RATIO;
            float phi = std::acos(1.0f - 2.0f * (i + 0.5f) / SAMPLE_COUNT);
            float sinPhi = std::sin(phi);
            float nx = std::cos(theta) * sinPhi;
            float ny = std::cos(phi);
            float nz = std::sin(theta) * sinPhi;

            // Sky/ground gradient based on Y direction
            float skyFactor = std::clamp(ny * 0.5f + 0.5f, 0.0f, 1.0f);
            float r = groundColor.x + (skyColor.x - groundColor.x) * skyFactor;
            float g = groundColor.y + (skyColor.y - groundColor.y) * skyFactor;
            float b = groundColor.z + (skyColor.z - groundColor.z) * skyFactor;

            // Add directional sun contribution
            float sunDot = std::max(0.0f, nx * sunDir.x + ny * sunDir.y + nz * sunDir.z);
            // Narrow sun disk approximation: pow(dot, 16)
            float sunTerm = sunDot * sunDot; // ^2
            sunTerm *= sunTerm;              // ^4
            sunTerm *= sunTerm;              // ^8
            sunTerm *= sunTerm;              // ^16
            r += sunColor.x * sunTerm;
            g += sunColor.y * sunTerm;
            b += sunColor.z * sunTerm;

            // Evaluate SH basis and accumulate
            float basis[9];
            EvaluateSHBasis(nx, ny, nz, basis);

            float weight = FOUR_PI / SAMPLE_COUNT;
            for (int c = 0; c < 9; ++c)
            {
                float bw = basis[c] * weight;
                sh.coefficients[c].x += r * bw;
                sh.coefficients[c].y += g * bw;
                sh.coefficients[c].z += b * bw;
            }
        }

        it->second.baked = true;
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "LightProbe %u baked (pos=%.1f,%.1f,%.1f)", probeId,
                        it->second.position.x, it->second.position.y, it->second.position.z);
    }

    void LightProbeSystem::BakeAllProbes()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "LightProbeSystem: baking all %zu probes", m_probes.size());
        for (auto& [id, probe] : m_probes)
        {
            BakeProbe(id);
        }
    }

    uint32_t LightProbeSystem::GetProbeCount() const
    {
        return static_cast<uint32_t>(m_probes.size());
    }

    void LightProbeSystem::PackProbeDataForGPU(std::vector<ProbeGPUData>& outData) const
    {
        outData.clear();
        outData.reserve(m_probes.size());

        for (const auto& [id, probe] : m_probes)
        {
            ProbeGPUData gpu{};

            for (int c = 0; c < 9; ++c)
            {
                gpu.shR[c] = probe.sh.coefficients[c].x;
                gpu.shG[c] = probe.sh.coefficients[c].y;
                gpu.shB[c] = probe.sh.coefficients[c].z;
            }

            gpu.posX = probe.position.x;
            gpu.posY = probe.position.y;
            gpu.posZ = probe.position.z;
            gpu.radius = probe.influenceRadius;

            outData.push_back(gpu);
        }
    }

    std::string LightProbeSystem::Console_GetStatus() const
    {
        uint32_t bakedCount = 0;
        for (const auto& [id, probe] : m_probes)
        {
            if (probe.baked)
                bakedCount++;
        }

        std::string status = "LightProbeSystem:\n";
        status += "  Total: " + std::to_string(m_probes.size()) + "\n";
        status += "  Baked: " + std::to_string(bakedCount) + "\n";
        return status;
    }

    void LightProbeSystem::Shutdown()
    {
        m_probes.clear();
        m_nextProbeId = 1;
    }

} // namespace Spark::Graphics
