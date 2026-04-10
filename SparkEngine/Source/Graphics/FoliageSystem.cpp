/**
 * @file FoliageSystem.cpp
 * @brief Implementation of FoliageManager — deterministic foliage scattering
 *
 * Scatter algorithm:
 *   1. Compute the number of instances for a volume as
 *      density * densityScale * (2*halfX * 2*halfZ) per species.
 *   2. Use a per-volume xorshift64 RNG seeded from desc.seed so the same
 *      input always produces the same instance set (required for tests
 *      and for reproducible world streaming).
 *   3. For each candidate position, sample terrain height via
 *      ClipmapTerrain::SampleHeight, estimate slope from 4 neighbouring
 *      samples, and reject the candidate if slope or altitude falls
 *      outside the species' or volume's constraints.
 *
 * No GPU state is touched here; instances are stored CPU-side and the
 * render path queries them via GetInstances / GetAllInstances. That keeps
 * unit tests GPU-free and lets the same data flow through NullRHIDevice
 * in headless mode.
 */

#include "FoliageSystem.h"

#include "../Core/Platform.h"
#include "../Engine/ECS/Components.h"
#include "../Utils/Validate.h"
#include "ClipmapTerrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>

using namespace DirectX;

namespace Spark::Graphics
{

    const std::vector<FoliageInstance> FoliageManager::s_emptyInstances;

    // ========================================================================
    // Deterministic RNG (xorshift64)
    // ========================================================================

    namespace
    {
        struct Xorshift64
        {
            uint64_t state;

            explicit Xorshift64(uint64_t seed) : state(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL) {}

            uint64_t Next()
            {
                uint64_t x = state;
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                state = x;
                return x;
            }

            // Uniform float in [0, 1).
            float NextFloat()
            {
                // Use top 24 bits for mantissa precision.
                return static_cast<float>((Next() >> 40)) * (1.0f / 16777216.0f);
            }

            // Uniform float in [lo, hi).
            float NextRange(float lo, float hi) { return lo + (hi - lo) * NextFloat(); }
        };

        // Combine a volume seed with a per-instance counter so two volumes
        // that share a seed still produce independent sequences when queried
        // in the same run.
        uint64_t MixSeed(uint32_t seed, uint32_t salt)
        {
            uint64_t h = static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<uint64_t>(salt) + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
            return h;
        }

        // Slope-resolving height sampler. Returns {height, normal}.
        // epsilon defaults to a 0.5 m step so the gradient is stable at
        // typical terrain resolutions.
        void SampleTerrainHeightAndNormal(float worldX, float worldZ, XMFLOAT3& outNormal, float& outHeight,
                                          float epsilon = 0.5f)
        {
            auto& terrain = Spark::Graphics::ClipmapTerrain::GetInstance();
            const float h = terrain.SampleHeight(worldX, worldZ);
            const float hL = terrain.SampleHeight(worldX - epsilon, worldZ);
            const float hR = terrain.SampleHeight(worldX + epsilon, worldZ);
            const float hD = terrain.SampleHeight(worldX, worldZ - epsilon);
            const float hU = terrain.SampleHeight(worldX, worldZ + epsilon);

            // Central differences → tangent vectors → cross product → normal.
            const float dhdx = (hR - hL) / (2.0f * epsilon);
            const float dhdz = (hU - hD) / (2.0f * epsilon);

            // n = normalize( (-dhdx, 1, -dhdz) )
            XMFLOAT3 n{-dhdx, 1.0f, -dhdz};
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 1e-6f)
            {
                n.x /= len;
                n.y /= len;
                n.z /= len;
            }
            else
            {
                n = {0.0f, 1.0f, 0.0f};
            }
            outNormal = n;
            outHeight = h;
        }

        // Slope angle in degrees between a surface normal and world up.
        float SlopeAngleDegrees(const XMFLOAT3& normal)
        {
            // cos(theta) = normal . up = normal.y
            float cosTheta = std::clamp(normal.y, -1.0f, 1.0f);
            return std::acos(cosTheta) * (180.0f / 3.14159265358979323846f);
        }
    } // namespace

    // ========================================================================
    // Singleton accessor
    // ========================================================================

    FoliageManager& FoliageManager::GetInstance()
    {
        static FoliageManager s;
        return s;
    }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    void FoliageManager::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        m_species.clear();
        m_speciesByName.clear();
        m_volumes.clear();
        m_nextVolumeId = 1;
        m_visibleInstances = 0;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageManager initialized");
    }

    void FoliageManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        const size_t volumes = m_volumes.size();
        const size_t species = m_species.size();
        m_volumes.clear();
        m_species.clear();
        m_speciesByName.clear();
        m_visibleInstances = 0;
        m_initialized = false;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageManager shutting down (%zu volumes, %zu species)", volumes,
                       species);
    }

    // ========================================================================
    // Species registry
    // ========================================================================

    void FoliageManager::RegisterSpecies(const FoliageSpecies& species)
    {
        if (species.name.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "FoliageManager::RegisterSpecies — empty species name ignored");
            return;
        }

        auto it = m_speciesByName.find(species.name);
        if (it != m_speciesByName.end())
        {
            m_species[it->second] = species;
            return;
        }

        const uint32_t idx = static_cast<uint32_t>(m_species.size());
        m_species.push_back(species);
        m_speciesByName[species.name] = idx;
    }

    const FoliageSpecies* FoliageManager::FindSpecies(const std::string& name) const
    {
        auto it = m_speciesByName.find(name);
        if (it == m_speciesByName.end())
            return nullptr;
        return &m_species[it->second];
    }

    void FoliageManager::UnregisterSpecies(const std::string& name)
    {
        auto it = m_speciesByName.find(name);
        if (it == m_speciesByName.end())
            return;
        const uint32_t removed = it->second;
        // Swap-erase preserves contiguous storage; rebuild the lookup map.
        if (removed != m_species.size() - 1)
        {
            m_species[removed] = m_species.back();
            m_speciesByName[m_species[removed].name] = removed;
        }
        m_species.pop_back();
        m_speciesByName.erase(name);
    }

    // ========================================================================
    // Volume management
    // ========================================================================

    uint32_t FoliageManager::AddVolume(const FoliageVolumeDesc& desc)
    {
        if (desc.speciesNames.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "FoliageManager::AddVolume — volume has no species, ignored");
            return 0;
        }

        Volume vol;
        vol.id = m_nextVolumeId++;
        vol.desc = desc;
        GenerateInstancesForVolume(vol);

        const size_t count = vol.instances.size();
        m_volumes.push_back(std::move(vol));

        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "FoliageManager: added volume %u with %zu instances",
                        m_volumes.back().id, count);
        return m_volumes.back().id;
    }

    void FoliageManager::RemoveVolume(uint32_t volumeId)
    {
        auto it =
            std::find_if(m_volumes.begin(), m_volumes.end(), [volumeId](const Volume& v) { return v.id == volumeId; });
        if (it == m_volumes.end())
            return;
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "FoliageManager: removing volume %u (%zu instances)", volumeId,
                        it->instances.size());
        m_volumes.erase(it);
    }

    bool FoliageManager::HasVolume(uint32_t volumeId) const
    {
        return std::any_of(m_volumes.begin(), m_volumes.end(),
                           [volumeId](const Volume& v) { return v.id == volumeId; });
    }

    std::vector<uint32_t> FoliageManager::GetVolumeIds() const
    {
        std::vector<uint32_t> ids;
        ids.reserve(m_volumes.size());
        for (const auto& v : m_volumes)
            ids.push_back(v.id);
        return ids;
    }

    const FoliageVolumeDesc* FoliageManager::GetVolumeDesc(uint32_t volumeId) const
    {
        for (const auto& v : m_volumes)
        {
            if (v.id == volumeId)
                return &v.desc;
        }
        return nullptr;
    }

    const std::vector<FoliageInstance>& FoliageManager::GetInstances(uint32_t volumeId) const
    {
        for (const auto& v : m_volumes)
        {
            if (v.id == volumeId)
                return v.instances;
        }
        return s_emptyInstances;
    }

    uint32_t FoliageManager::GetTotalInstanceCount() const
    {
        size_t total = 0;
        for (const auto& v : m_volumes)
            total += v.instances.size();
        return static_cast<uint32_t>(total);
    }

    // ========================================================================
    // Scatter implementation
    // ========================================================================

    void FoliageManager::GenerateInstancesForVolume(Volume& volume)
    {
        volume.instances.clear();
        if (!volume.desc.enabled)
            return;

        const float extentX = std::max(0.0f, volume.desc.halfExtents.x);
        const float extentZ = std::max(0.0f, volume.desc.halfExtents.z);
        const float xzArea = (2.0f * extentX) * (2.0f * extentZ);
        if (xzArea <= 0.0f)
            return;

        Xorshift64 rng(MixSeed(volume.desc.seed, volume.id));

        // For each species referenced by the volume, scatter its own count.
        for (uint32_t speciesIdx = 0; speciesIdx < static_cast<uint32_t>(volume.desc.speciesNames.size()); ++speciesIdx)
        {
            const auto& speciesName = volume.desc.speciesNames[speciesIdx];
            const FoliageSpecies* species = FindSpecies(speciesName);
            if (!species)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               "FoliageManager: volume %u references unknown species '%s'", volume.id,
                               speciesName.c_str());
                continue;
            }

            // Count = density * densityScale * area, clamped to a sane upper
            // bound so a runaway density value cannot blow up memory.
            const float rawCount = species->density * volume.desc.densityScale * xzArea;
            const uint32_t count = static_cast<uint32_t>(std::min(rawCount, 1000000.0f));
            if (count == 0)
                continue;

            // Merge species and volume slope/altitude filters.
            const float minSlope = std::max(species->minSlopeAngleDeg, volume.desc.minSlopeAngleDeg);
            const float maxSlope = std::min(species->maxSlopeAngleDeg, volume.desc.maxSlopeAngleDeg);
            const float minAlt = std::max(species->minAltitude, volume.desc.minAltitude);
            const float maxAlt = std::min(species->maxAltitude, volume.desc.maxAltitude);

            for (uint32_t i = 0; i < count; ++i)
            {
                FoliageInstance inst;

                // Sample XZ inside the volume AABB centered on desc.center.
                const float u = rng.NextFloat(); // [0, 1)
                const float v = rng.NextFloat(); // [0, 1)
                const float worldX = volume.desc.center.x + (u * 2.0f - 1.0f) * extentX;
                const float worldZ = volume.desc.center.z + (v * 2.0f - 1.0f) * extentZ;

                // Resolve Y via terrain query (or use the volume center's Y).
                XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
                float height = volume.desc.center.y;
                if (volume.desc.alignToTerrain)
                {
                    SampleTerrainHeightAndNormal(worldX, worldZ, normal, height);
                }

                // Altitude filter.
                if (height < minAlt || height > maxAlt)
                    continue;

                // Slope filter.
                const float slope = SlopeAngleDegrees(normal);
                if (slope < minSlope || slope > maxSlope)
                    continue;

                inst.position = {worldX, height, worldZ};
                inst.normal = species->alignToSurface ? normal : XMFLOAT3{0.0f, 1.0f, 0.0f};
                inst.yawRadians = species->randomYaw ? rng.NextRange(0.0f, 6.283185307f) : 0.0f;
                inst.scale = rng.NextRange(species->minScale, species->maxScale);
                inst.speciesIndex = speciesIdx;
                inst.distanceToCamera = 0.0f;
                inst.visible = true;
                volume.instances.push_back(inst);
            }
        }
    }

    // ========================================================================
    // Per-frame update — distance culling
    // ========================================================================

    void FoliageManager::Update(float /*deltaTime*/, const XMFLOAT3& cameraPosition)
    {
        if (!m_initialized)
            return;

        uint32_t visible = 0;
        for (auto& volume : m_volumes)
        {
            if (!volume.desc.enabled)
            {
                for (auto& inst : volume.instances)
                    inst.visible = false;
                continue;
            }

            for (auto& inst : volume.instances)
            {
                const float dx = cameraPosition.x - inst.position.x;
                const float dy = cameraPosition.y - inst.position.y;
                const float dz = cameraPosition.z - inst.position.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                inst.distanceToCamera = dist;

                // Use the species cull distance.
                float cull = 100.0f;
                if (inst.speciesIndex < volume.desc.speciesNames.size())
                {
                    if (const FoliageSpecies* s = FindSpecies(volume.desc.speciesNames[inst.speciesIndex]))
                        cull = s->cullDistance;
                }
                inst.visible = (dist <= cull);
                if (inst.visible)
                    ++visible;
            }
        }
        m_visibleInstances = visible;
    }

    // ========================================================================
    // ECS integration
    // ========================================================================

    void FoliageManager::UpdateFromECS(::World& world, float /*deltaTime*/)
    {
        if (!m_initialized)
            return;

        auto view = world.GetEntitiesWith<FoliageVolumeComponent, Transform>();
        for (auto entity : view)
        {
            auto& comp = view.get<FoliageVolumeComponent>(entity);
            auto& transform = view.get<Transform>(entity);

            // Skip disabled components.
            if (!comp.enabled)
            {
                if (comp.runtimeVolumeId != 0)
                {
                    RemoveVolume(comp.runtimeVolumeId);
                    comp.runtimeVolumeId = 0;
                }
                continue;
            }

            // If this component has no runtime volume yet, create one.
            if (comp.runtimeVolumeId == 0)
            {
                FoliageVolumeDesc desc;
                desc.center = transform.position;
                desc.halfExtents = comp.halfExtents;
                desc.seed = static_cast<uint32_t>(comp.seed);
                desc.densityScale = comp.densityScale;
                desc.minSlopeAngleDeg = comp.minSlopeAngle;
                desc.maxSlopeAngleDeg = comp.maxSlopeAngle;
                desc.minAltitude = comp.minAltitude;
                desc.maxAltitude = comp.maxAltitude;
                desc.alignToTerrain = comp.alignToSurface;
                desc.enabled = true;
                desc.speciesNames = comp.speciesNames;

                if (!desc.speciesNames.empty())
                {
                    comp.runtimeVolumeId = AddVolume(desc);
                }
            }
        }
    }

    // ========================================================================
    // Diagnostics
    // ========================================================================

    std::string FoliageManager::Console_GetStatus() const
    {
        std::ostringstream os;
        os << "FoliageManager: " << m_species.size() << " species, " << m_volumes.size() << " volumes, "
           << GetTotalInstanceCount() << " instances (" << m_visibleInstances << " visible)";
        return os.str();
    }

} // namespace Spark::Graphics
