/**
 * @file WaterRenderer.cpp
 * @brief Implementation of Gerstner wave water surface simulation
 * @author Spark Engine Team
 * @date 2026
 */

#include "WaterRenderer.h"
#include "../Utils/AngleUtils.h"

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr float kPI = Spark::AngleUtils::PI;
    static constexpr float kTwoPI = Spark::AngleUtils::TWO_PI;
    static constexpr float kFiniteDiffEpsilon = 0.05f;

    // =========================================================================
    // Initialization
    // =========================================================================

    bool WaterRenderer::Initialize()
    {
        if (m_initialized)
        {
            return true;
        }

        GenerateDefaultWaves();
        m_initialized = true;
        return true;
    }

    void WaterRenderer::Shutdown()
    {
        m_planes.clear();
        m_waves.clear();
        m_time = 0.0f;
        m_nextID = 1;
        m_initialized = false;
    }

    // =========================================================================
    // Settings
    // =========================================================================

    void WaterRenderer::SetSettings(const WaterSettings& settings)
    {
        m_settings = settings;
        m_settings.waveCount = std::clamp(m_settings.waveCount, 1, 16);
        GenerateDefaultWaves();
    }

    // =========================================================================
    // Water Plane Management
    // =========================================================================

    uint32_t WaterRenderer::AddWaterPlane(const XMFLOAT3& center, const XMFLOAT2& size)
    {
        uint32_t id = m_nextID++;
        WaterPlane plane;
        plane.center = center;
        plane.size = size;
        GenerateGrid(plane);
        m_planes.emplace(id, std::move(plane));
        return id;
    }

    void WaterRenderer::RemoveWaterPlane(uint32_t waterID)
    {
        m_planes.erase(waterID);
    }

    // =========================================================================
    // Grid Generation
    // =========================================================================

    void WaterRenderer::GenerateGrid(WaterPlane& plane) const
    {
        int res = plane.gridResolution;
        int vertCount = res * res;
        plane.vertices.resize(static_cast<size_t>(vertCount));
        plane.basePositions.resize(static_cast<size_t>(vertCount));

        float halfW = plane.size.x * 0.5f;
        float halfD = plane.size.y * 0.5f;
        float stepX = plane.size.x / static_cast<float>(res - 1);
        float stepZ = plane.size.y / static_cast<float>(res - 1);

        for (int z = 0; z < res; ++z)
        {
            for (int x = 0; x < res; ++x)
            {
                int idx = z * res + x;
                float posX = plane.center.x - halfW + static_cast<float>(x) * stepX;
                float posZ = plane.center.z - halfD + static_cast<float>(z) * stepZ;

                plane.basePositions[idx] = {posX, plane.center.y, posZ};

                plane.vertices[idx].position = plane.basePositions[idx];
                plane.vertices[idx].normal = {0.0f, 1.0f, 0.0f};
                plane.vertices[idx].texCoord = {static_cast<float>(x) / static_cast<float>(res - 1),
                                                static_cast<float>(z) / static_cast<float>(res - 1)};
            }
        }
    }

    // =========================================================================
    // Default Wave Generation
    // =========================================================================

    void WaterRenderer::GenerateDefaultWaves()
    {
        m_waves.clear();
        int count = m_settings.waveCount;

        // Generate a spread of wave components with decreasing amplitude and increasing frequency
        for (int i = 0; i < count; ++i)
        {
            GerstnerWave wave;

            // Spread directions across the XZ plane
            float angle = (static_cast<float>(i) / static_cast<float>(count)) * kPI * 0.8f - kPI * 0.4f;
            wave.direction = {std::cos(angle), std::sin(angle)};

            // Each successive wave is smaller and higher frequency
            float scale = 1.0f / static_cast<float>(1 + i);
            wave.amplitude = m_settings.waveAmplitude * scale;
            wave.wavelength = 10.0f * scale / m_settings.waveFrequency;
            wave.wavelength = std::max(wave.wavelength, 0.5f);
            wave.speed = m_settings.waveSpeed * std::sqrt(wave.wavelength);
            wave.steepness = std::clamp(0.3f + 0.1f * static_cast<float>(i), 0.0f, 1.0f);

            m_waves.push_back(wave);
        }
    }

    // =========================================================================
    // Gerstner Wave Computation
    // =========================================================================

    XMFLOAT3 WaterRenderer::ComputeGerstnerDisplacement(float baseX, float baseZ, float time) const
    {
        float dispX = 0.0f;
        float dispY = 0.0f;
        float dispZ = 0.0f;

        for (const auto& wave : m_waves)
        {
            float k = kTwoPI / wave.wavelength;
            float phase = k * (wave.direction.x * baseX + wave.direction.y * baseZ) - wave.speed * k * time;

            float cosPhase = std::cos(phase);
            float sinPhase = std::sin(phase);

            float q = wave.steepness / (k * wave.amplitude * static_cast<float>(m_waves.size()));
            q = std::clamp(q, 0.0f, 1.0f);

            dispX += q * wave.amplitude * wave.direction.x * cosPhase;
            dispY += wave.amplitude * sinPhase;
            dispZ += q * wave.amplitude * wave.direction.y * cosPhase;
        }

        return {baseX + dispX, dispY, baseZ + dispZ};
    }

    XMFLOAT3 WaterRenderer::ComputeGerstnerNormal(float baseX, float baseZ, float time) const
    {
        // Finite differences for normal estimation
        XMFLOAT3 center = ComputeGerstnerDisplacement(baseX, baseZ, time);
        XMFLOAT3 right = ComputeGerstnerDisplacement(baseX + kFiniteDiffEpsilon, baseZ, time);
        XMFLOAT3 forward = ComputeGerstnerDisplacement(baseX, baseZ + kFiniteDiffEpsilon, time);

        // Tangent vectors
        float tx = right.x - center.x;
        float ty = right.y - center.y;
        float tz = right.z - center.z;

        float bx = forward.x - center.x;
        float by = forward.y - center.y;
        float bz = forward.z - center.z;

        // Cross product (tangent x bitangent)
        float nx = ty * bz - tz * by;
        float ny = tz * bx - tx * bz;
        float nz = tx * by - ty * bx;

        // Normalize
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0001f)
        {
            float inv = 1.0f / len;
            nx *= inv;
            ny *= inv;
            nz *= inv;
        }
        else
        {
            nx = 0.0f;
            ny = 1.0f;
            nz = 0.0f;
        }

        return {nx, ny, nz};
    }

    // =========================================================================
    // Update
    // =========================================================================

    void WaterRenderer::Update(float deltaTime)
    {
        if (!m_settings.enabled)
        {
            return;
        }

        m_time += deltaTime;

        for (auto& [id, plane] : m_planes)
        {
            int count = static_cast<int>(plane.vertices.size());
            for (int i = 0; i < count; ++i)
            {
                const XMFLOAT3& base = plane.basePositions[i];
                XMFLOAT3 displaced = ComputeGerstnerDisplacement(base.x, base.z, m_time);
                displaced.y += plane.center.y;

                plane.vertices[i].position = displaced;
                plane.vertices[i].normal = ComputeGerstnerNormal(base.x, base.z, m_time);
            }
        }
    }

    // =========================================================================
    // Queries
    // =========================================================================

    float WaterRenderer::GetWaterHeight(float worldX, float worldZ) const
    {
        XMFLOAT3 disp = ComputeGerstnerDisplacement(worldX, worldZ, m_time);

        // Find the water plane that contains this point (use the first one found)
        for (const auto& [id, plane] : m_planes)
        {
            float halfW = plane.size.x * 0.5f;
            float halfD = plane.size.y * 0.5f;

            if (worldX >= plane.center.x - halfW && worldX <= plane.center.x + halfW &&
                worldZ >= plane.center.z - halfD && worldZ <= plane.center.z + halfD)
            {
                return plane.center.y + disp.y;
            }
        }

        return disp.y;
    }

    std::span<const WaterVertex> WaterRenderer::GetMeshVertices(uint32_t waterID) const
    {
        auto it = m_planes.find(waterID);
        if (it == m_planes.end())
        {
            return {};
        }
        return it->second.vertices;
    }

} // namespace Spark::Graphics
