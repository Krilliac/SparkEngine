/**
 * @file VolumetricClouds.cpp
 * @brief CPU reference / GPU feeder for the volumetric cloud layer
 */

#include "VolumetricClouds.h"

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        float Clamp01(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        float Fade(float t)
        {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        uint32_t WrapIndex(int32_t v, uint32_t size)
        {
            const int32_t s = static_cast<int32_t>(size);
            int32_t r = v % s;
            return static_cast<uint32_t>(r < 0 ? r + s : r);
        }

        float WorleyAt(float x, float y, float z, uint32_t gridSize)
        {
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            float minDist = 1.5f;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const uint32_t cx = WrapIndex(ix + dx, gridSize);
                        const uint32_t cy = WrapIndex(iy + dy, gridSize);
                        const uint32_t cz = WrapIndex(iz + dz, gridSize);
                        const uint32_t h = (cx * 73856093u) ^ (cy * 19349663u) ^ (cz * 83492791u);
                        const float ox = ((h & 0xFFu) / 255.0f - 0.5f) * 0.9f;
                        const float oy = (((h >> 8) & 0xFFu) / 255.0f - 0.5f) * 0.9f;
                        const float oz = (((h >> 16) & 0xFFu) / 255.0f - 0.5f) * 0.9f;
                        const float fxw = static_cast<float>(ix + dx) + 0.5f + ox - x;
                        const float fyw = static_cast<float>(iy + dy) + 0.5f + oy - y;
                        const float fzw = static_cast<float>(iz + dz) + 0.5f + oz - z;
                        const float d = std::sqrt(fxw * fxw + fyw * fyw + fzw * fzw);
                        if (d < minDist)
                            minDist = d;
                    }
            return Clamp01(1.0f - minDist);
        }
    } // namespace

    VolumetricCloudSystem& VolumetricCloudSystem::GetInstance()
    {
        static VolumetricCloudSystem instance;
        return instance;
    }

    bool VolumetricCloudSystem::Initialize()
    {
        return Initialize(VolumetricCloudSettings{});
    }

    bool VolumetricCloudSystem::Initialize(const VolumetricCloudSettings& settings)
    {
        m_settings = settings;
        m_time = 0.0f;
        RegenerateNoise();
        RegenerateWeatherMap();
        m_initialized = true;
        return true;
    }

    void VolumetricCloudSystem::Shutdown()
    {
        m_gpuBaseNoise.reset();
        m_gpuDetailNoise.reset();
        m_gpuWeatherMap.reset();
        m_baseNoise.clear();
        m_detailNoise.clear();
        m_weatherMap.clear();
        m_initialized = false;
    }

    void VolumetricCloudSystem::Update(float dt)
    {
        if (!m_initialized || !m_settings.enabled)
            return;
        m_time += dt;
    }

    void VolumetricCloudSystem::SetSettings(const VolumetricCloudSettings& settings)
    {
        const bool needsRegen = settings.cloudType != m_settings.cloudType || settings.coverage != m_settings.coverage;
        m_settings = settings;
        if (needsRegen)
            RegenerateWeatherMap();
    }

    void VolumetricCloudSystem::SetSunDirection(float x, float y, float z)
    {
        const float len = std::sqrt(x * x + y * y + z * z);
        const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
        m_sunDirX = x * inv;
        m_sunDirY = y * inv;
        m_sunDirZ = z * inv;
    }

    void VolumetricCloudSystem::SetSunColor(float r, float g, float b)
    {
        m_sunR = r;
        m_sunG = g;
        m_sunB = b;
    }

    void VolumetricCloudSystem::SetWeather(float coverage, float cloudType, float rain)
    {
        m_settings.coverage = Clamp01(coverage);
        m_settings.cloudType = Clamp01(cloudType);
        m_rainWetness = Clamp01(rain);
        RegenerateWeatherMap();
    }

    float VolumetricCloudSystem::Hash(uint32_t x, uint32_t y, uint32_t z)
    {
        uint32_t h = x * 73856093u ^ y * 19349663u ^ z * 83492791u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h) / static_cast<float>(0xFFFFFFFFu);
    }

    float VolumetricCloudSystem::SmoothNoise3D(const std::vector<float>& grid, uint32_t size, float x, float y, float z)
    {
        const float fx = x - std::floor(x);
        const float fy = y - std::floor(y);
        const float fz = z - std::floor(z);
        const uint32_t x0 = WrapIndex(static_cast<int32_t>(std::floor(x)), size);
        const uint32_t y0 = WrapIndex(static_cast<int32_t>(std::floor(y)), size);
        const uint32_t z0 = WrapIndex(static_cast<int32_t>(std::floor(z)), size);
        const uint32_t x1 = (x0 + 1) % size;
        const uint32_t y1 = (y0 + 1) % size;
        const uint32_t z1 = (z0 + 1) % size;

        auto at = [&](uint32_t ix, uint32_t iy, uint32_t iz) { return grid[iz * size * size + iy * size + ix]; };

        const float u = Fade(fx);
        const float v = Fade(fy);
        const float w = Fade(fz);

        const float c000 = at(x0, y0, z0);
        const float c100 = at(x1, y0, z0);
        const float c010 = at(x0, y1, z0);
        const float c110 = at(x1, y1, z0);
        const float c001 = at(x0, y0, z1);
        const float c101 = at(x1, y0, z1);
        const float c011 = at(x0, y1, z1);
        const float c111 = at(x1, y1, z1);

        const float x00 = Lerp(c000, c100, u);
        const float x10 = Lerp(c010, c110, u);
        const float x01 = Lerp(c001, c101, u);
        const float x11 = Lerp(c011, c111, u);
        const float y0v = Lerp(x00, x10, v);
        const float y1v = Lerp(x01, x11, v);
        return Lerp(y0v, y1v, w);
    }

    void VolumetricCloudSystem::RegenerateNoise()
    {
        m_baseNoise.resize(kBaseSize * kBaseSize * kBaseSize);
        for (uint32_t z = 0; z < kBaseSize; ++z)
            for (uint32_t y = 0; y < kBaseSize; ++y)
                for (uint32_t x = 0; x < kBaseSize; ++x)
                {
                    const uint32_t idx = z * kBaseSize * kBaseSize + y * kBaseSize + x;
                    // Octaved fractal Perlin-lookalike using hashed cell random
                    float amp = 0.5f;
                    float freq = 1.0f;
                    float v = 0.0f;
                    for (int oct = 0; oct < 3; ++oct)
                    {
                        const uint32_t s = static_cast<uint32_t>(std::max(1.0f, std::floor(freq * 8.0f)));
                        v += amp * Hash(x * s + oct * 17u, y * s + oct * 31u, z * s + oct * 53u);
                        amp *= 0.5f;
                        freq *= 2.0f;
                    }
                    m_baseNoise[idx] = Clamp01(v);
                }

        m_detailNoise.resize(kDetailSize * kDetailSize * kDetailSize);
        for (uint32_t z = 0; z < kDetailSize; ++z)
            for (uint32_t y = 0; y < kDetailSize; ++y)
                for (uint32_t x = 0; x < kDetailSize; ++x)
                {
                    const float fx = static_cast<float>(x) * (4.0f / static_cast<float>(kDetailSize));
                    const float fy = static_cast<float>(y) * (4.0f / static_cast<float>(kDetailSize));
                    const float fz = static_cast<float>(z) * (4.0f / static_cast<float>(kDetailSize));
                    m_detailNoise[z * kDetailSize * kDetailSize + y * kDetailSize + x] = WorleyAt(fx, fy, fz, 4);
                }
    }

    void VolumetricCloudSystem::RegenerateWeatherMap()
    {
        m_weatherMap.resize(kWeatherSize * kWeatherSize * 2);
        for (uint32_t y = 0; y < kWeatherSize; ++y)
            for (uint32_t x = 0; x < kWeatherSize; ++x)
            {
                const float nx = static_cast<float>(x) / static_cast<float>(kWeatherSize);
                const float ny = static_cast<float>(y) / static_cast<float>(kWeatherSize);
                // Bubble-like coverage pattern biased by user coverage setting.
                const float bubble = 0.5f + 0.5f * std::sin(nx * 6.283f * 3.0f + m_settings.windDirectionX) *
                                                std::cos(ny * 6.283f * 2.0f + m_settings.windDirectionZ);
                const float coverage = Clamp01(Lerp(m_settings.coverage * 0.5f, 1.0f, bubble));
                const float hash = Hash(x, y, 0);
                const float typeJitter = (hash - 0.5f) * 0.2f;
                const float cloudType = Clamp01(m_settings.cloudType + typeJitter);
                m_weatherMap[(y * kWeatherSize + x) * 2 + 0] = coverage;
                m_weatherMap[(y * kWeatherSize + x) * 2 + 1] = cloudType;
            }
    }

    float VolumetricCloudSystem::SampleBaseNoise(float x, float y, float z) const
    {
        return SmoothNoise3D(m_baseNoise, kBaseSize, x, y, z);
    }

    float VolumetricCloudSystem::SampleDetailNoise(float x, float y, float z) const
    {
        return SmoothNoise3D(m_detailNoise, kDetailSize, x, y, z);
    }

    float VolumetricCloudSystem::SampleWeatherMap(float x, float z) const
    {
        const float fx = x * static_cast<float>(kWeatherSize);
        const float fz = z * static_cast<float>(kWeatherSize);
        const uint32_t x0 = WrapIndex(static_cast<int32_t>(std::floor(fx)), kWeatherSize);
        const uint32_t z0 = WrapIndex(static_cast<int32_t>(std::floor(fz)), kWeatherSize);
        return m_weatherMap[(z0 * kWeatherSize + x0) * 2 + 0];
    }

    float VolumetricCloudSystem::HenyeyGreenstein(float cosTheta, float g)
    {
        const float g2 = g * g;
        const float denom = 1.0f + g2 - 2.0f * g * cosTheta;
        return (1.0f - g2) / (4.0f * kPi * std::pow(std::max(denom, 1e-6f), 1.5f));
    }

    float VolumetricCloudSystem::HeightGradient(float h, float cloudType)
    {
        // Stratus: bottom-heavy; cumulus: central; cumulonimbus: tall
        const float stratus = std::clamp(1.0f - std::abs(h - 0.15f) / 0.25f, 0.0f, 1.0f);
        const float cumulus = std::clamp(1.0f - std::abs(h - 0.5f) / 0.5f, 0.0f, 1.0f);
        const float cumulonimbus = std::clamp(1.0f - std::abs(h - 0.7f) / 0.9f, 0.0f, 1.0f);
        if (cloudType < 0.5f)
            return Lerp(stratus, cumulus, cloudType * 2.0f);
        return Lerp(cumulus, cumulonimbus, (cloudType - 0.5f) * 2.0f);
    }

    float VolumetricCloudSystem::SampleDensity(float x, float y, float z) const
    {
        if (!m_initialized || !m_settings.enabled)
            return 0.0f;
        if (y < m_settings.baseAltitude || y > m_settings.topAltitude)
            return 0.0f;

        const float layerThickness = std::max(1.0f, m_settings.topAltitude - m_settings.baseAltitude);
        const float h = (y - m_settings.baseAltitude) / layerThickness;

        const float windOffsetX = m_sunDirX * 0.0f + m_settings.windDirectionX * m_settings.windSpeed * m_time;
        const float windOffsetZ = m_settings.windDirectionZ * m_settings.windSpeed * m_time;

        const float weatherU = (x + windOffsetX) * m_settings.weatherScale;
        const float weatherV = (z + windOffsetZ) * m_settings.weatherScale;
        const float coverage = SampleWeatherMap(weatherU - std::floor(weatherU), weatherV - std::floor(weatherV));

        const float baseU = (x + windOffsetX) * m_settings.baseNoiseScale;
        const float baseV = y * m_settings.baseNoiseScale;
        const float baseW = (z + windOffsetZ) * m_settings.baseNoiseScale;
        const float base = SampleBaseNoise(baseU * kBaseSize, baseV * kBaseSize, baseW * kBaseSize);

        const float grad = HeightGradient(h, m_settings.cloudType);
        float d = base * grad;
        d = std::max(0.0f, d - (1.0f - coverage));
        d *= coverage;

        if (m_settings.detailStrength > 0.0f)
        {
            const float detail = SampleDetailNoise(x * m_settings.detailNoiseScale * kDetailSize,
                                                   y * m_settings.detailNoiseScale * kDetailSize,
                                                   z * m_settings.detailNoiseScale * kDetailSize);
            const float erosion = Lerp(detail, 1.0f - detail, Clamp01(h));
            d = std::max(0.0f, d - erosion * m_settings.detailStrength * 0.5f);
        }

        // Anvil bias flattens tops of large convective clouds.
        if (m_settings.cloudType > 0.7f && h > 0.8f)
        {
            d *= Lerp(1.0f, 1.0f - m_settings.anvilBias, (h - 0.8f) / 0.2f);
        }

        return Clamp01(d);
    }

    CloudSample VolumetricCloudSystem::SampleAlongRay(float originX, float originY, float originZ, float dirX,
                                                      float dirY, float dirZ, float maxDistance) const
    {
        CloudSample result;
        if (!m_initialized || !m_settings.enabled)
            return result;

        const float len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
        if (len < 1e-6f)
            return result;
        dirX /= len;
        dirY /= len;
        dirZ /= len;

        // Intersect ray with the two slab altitudes; march only between them.
        auto slabDist = [&](float targetY) -> float
        {
            if (std::abs(dirY) < 1e-4f)
                return -1.0f;
            return (targetY - originY) / dirY;
        };
        const float dBottom = slabDist(m_settings.baseAltitude);
        const float dTop = slabDist(m_settings.topAltitude);
        float tMin = std::max(0.0f, std::min(dBottom, dTop));
        float tMax = std::max(dBottom, dTop);
        if (tMax <= 0.0f)
            return result;
        tMax = std::min(tMax, maxDistance);
        if (tMax <= tMin)
            return result;

        const uint32_t steps = std::max<uint32_t>(8u, m_settings.marchSteps);
        const float stepSize = (tMax - tMin) / static_cast<float>(steps);
        const float cosTheta = dirX * m_sunDirX + dirY * m_sunDirY + dirZ * m_sunDirZ;
        const float phase = Lerp(HenyeyGreenstein(cosTheta, m_settings.phaseG0),
                                 HenyeyGreenstein(cosTheta, m_settings.phaseG1), m_settings.phaseBlend) +
                            m_settings.silverLining * std::pow(std::max(cosTheta, 0.0f), 8.0f);

        float transmittance = 1.0f;
        float lumR = 0.0f, lumG = 0.0f, lumB = 0.0f;

        for (uint32_t i = 0; i < steps && transmittance > 0.01f; ++i)
        {
            const float t = tMin + (static_cast<float>(i) + 0.5f) * stepSize;
            const float px = originX + dirX * t;
            const float py = originY + dirY * t;
            const float pz = originZ + dirZ * t;
            const float density = SampleDensity(px, py, pz);
            if (density <= 0.0f)
                continue;

            // Light march towards the sun (short) for self-shadowing.
            float lightTransmittance = 1.0f;
            const uint32_t lSteps = std::max<uint32_t>(2u, m_settings.lightSteps);
            const float lStep = m_settings.lightMarchDistance / static_cast<float>(lSteps);
            for (uint32_t j = 0; j < lSteps; ++j)
            {
                const float lt = (static_cast<float>(j) + 0.5f) * lStep;
                const float lx = px + m_sunDirX * lt;
                const float ly = py + m_sunDirY * lt;
                const float lz = pz + m_sunDirZ * lt;
                const float ld = SampleDensity(lx, ly, lz);
                lightTransmittance *= std::exp(-ld * m_settings.density * lStep);
                if (lightTransmittance < 0.01f)
                    break;
            }

            const float extinction = density * m_settings.density;
            const float stepTrans = std::exp(-extinction * stepSize);
            // Integrated inscattering: L = sigma_s * phase * T_light * T_view * ds (analytic form)
            const float scatter = extinction * phase * lightTransmittance;
            const float integrated = (1.0f - stepTrans) / std::max(extinction, 1e-6f);
            lumR += scatter * m_sunR * transmittance * integrated;
            lumG += scatter * m_sunG * transmittance * integrated;
            lumB += scatter * m_sunB * transmittance * integrated;
            transmittance *= stepTrans;
        }

        // Damp base luminance slightly when wet (rain darkens clouds).
        const float wetAtten = 1.0f - 0.4f * m_rainWetness;
        result.luminanceR = lumR * wetAtten;
        result.luminanceG = lumG * wetAtten;
        result.luminanceB = lumB * wetAtten;
        result.transmittance = transmittance;
        return result;
    }

    bool VolumetricCloudSystem::CreateGPUResources(Spark::RHI::IRHIDevice* device)
    {
        if (!device || !m_initialized)
            return false;

        Spark::RHI::RHITextureDesc base{};
        base.width = kBaseSize;
        base.height = kBaseSize;
        base.depth = kBaseSize;
        base.type = Spark::RHI::RHITextureType::Texture3D;
        base.format = Spark::RHI::PixelFormat::R8_UNORM;
        base.usage = Spark::RHI::RHITextureUsage::ShaderResource;
        base.debugName = "VolumetricClouds_Base3D";
        m_gpuBaseNoise = device->CreateTexture(base);

        Spark::RHI::RHITextureDesc detail{};
        detail.width = kDetailSize;
        detail.height = kDetailSize;
        detail.depth = kDetailSize;
        detail.type = Spark::RHI::RHITextureType::Texture3D;
        detail.format = Spark::RHI::PixelFormat::R8_UNORM;
        detail.usage = Spark::RHI::RHITextureUsage::ShaderResource;
        detail.debugName = "VolumetricClouds_Detail3D";
        m_gpuDetailNoise = device->CreateTexture(detail);

        Spark::RHI::RHITextureDesc weather{};
        weather.width = kWeatherSize;
        weather.height = kWeatherSize;
        weather.depth = 1;
        weather.type = Spark::RHI::RHITextureType::Texture2D;
        weather.format = Spark::RHI::PixelFormat::R8G8_UNORM;
        weather.usage = Spark::RHI::RHITextureUsage::ShaderResource;
        weather.debugName = "VolumetricClouds_Weather2D";
        m_gpuWeatherMap = device->CreateTexture(weather);

        UploadToGPU(device);
        return m_gpuBaseNoise != nullptr;
    }

    void VolumetricCloudSystem::UploadToGPU(Spark::RHI::IRHIDevice* device)
    {
        if (!device)
            return;

        if (m_gpuBaseNoise && !m_baseNoise.empty())
        {
            std::vector<uint8_t> packed;
            packed.reserve(m_baseNoise.size());
            for (float f : m_baseNoise)
                packed.push_back(static_cast<uint8_t>(Clamp01(f) * 255.0f));
            device->UpdateTexture(m_gpuBaseNoise.get(), packed.data(), 0, 0);
        }
        if (m_gpuDetailNoise && !m_detailNoise.empty())
        {
            std::vector<uint8_t> packed;
            packed.reserve(m_detailNoise.size());
            for (float f : m_detailNoise)
                packed.push_back(static_cast<uint8_t>(Clamp01(f) * 255.0f));
            device->UpdateTexture(m_gpuDetailNoise.get(), packed.data(), 0, 0);
        }
        if (m_gpuWeatherMap && !m_weatherMap.empty())
        {
            std::vector<uint8_t> packed;
            packed.reserve(m_weatherMap.size());
            for (float f : m_weatherMap)
                packed.push_back(static_cast<uint8_t>(Clamp01(f) * 255.0f));
            device->UpdateTexture(m_gpuWeatherMap.get(), packed.data(), 0, 0);
        }
    }

} // namespace Spark::Graphics
