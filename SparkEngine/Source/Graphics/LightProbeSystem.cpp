/**
 * @file LightProbeSystem.cpp
 * @brief Light probe management with SH interpolation
 */

#include "LightProbeSystem.h"
#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

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

    static float Distance(const Float3& a, const Float3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    bool LightProbeSystem::Initialize()
    {
        m_probes.clear();
        m_nextProbeId = 1;
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
        auto it = m_probes.find(probeId);
        if (it == m_probes.end())
            return;

        // Stub: set ambient-only SH (L0 band = uniform white light)
        it->second.sh = SphericalHarmonics{};
        it->second.sh.coefficients[0] = {0.5f, 0.5f, 0.5f};
        it->second.baked = true;
    }

    void LightProbeSystem::BakeAllProbes()
    {
        for (auto& [id, probe] : m_probes)
            BakeProbe(id);
    }

    uint32_t LightProbeSystem::GetProbeCount() const
    {
        return static_cast<uint32_t>(m_probes.size());
    }

    std::string LightProbeSystem::Console_GetStatus() const
    {
        uint32_t bakedCount = 0;
        for (const auto& [id, probe] : m_probes)
            if (probe.baked)
                bakedCount++;

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
