/**
 * @file AudioMixer.cpp
 * @brief Implementation of the audio mixer, reverb zones, and occlusion
 */

#include "AudioMixer.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark::Audio
{

    AudioMixer::AudioMixer() = default;

    void AudioMixer::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_LOG_INFO(Spark::LogCategory::Audio, "AudioMixer::Initialize - creating default bus hierarchy");
        CreateBus("Master", "");
        CreateBus("SFX", "Master");
        CreateBus("Music", "Master");
        CreateBus("Voice", "Master");
        CreateBus("Ambient", "Master");
        CreateBus("UI", "Master");
    }

    void AudioMixer::Update(const DirectX::XMFLOAT3& listenerPos, float deltaTime)
    {
        (void)deltaTime;
        m_listenerPosition = listenerPos;
    }

    void AudioMixer::CreateBus(const std::string& name, const std::string& parentBus)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, name);
        BusState state;
        state.bus.name = name;
        state.bus.parentBus = parentBus;
        m_buses[name] = std::move(state);
    }

    void AudioMixer::SetBusVolume(const std::string& busName, float volume)
    {
        auto it = m_buses.find(busName);
        if (it != m_buses.end())
        {
            it->second.bus.volume = std::clamp(volume, 0.0f, 1.0f);
        }
    }

    float AudioMixer::GetBusVolume(const std::string& busName) const
    {
        auto it = m_buses.find(busName);
        return it != m_buses.end() ? it->second.bus.volume : 0.0f;
    }

    void AudioMixer::SetBusMuted(const std::string& busName, bool muted)
    {
        auto it = m_buses.find(busName);
        if (it != m_buses.end())
        {
            it->second.bus.muted = muted;
        }
    }

    void AudioMixer::SetBusSolo(const std::string& busName, bool solo)
    {
        auto it = m_buses.find(busName);
        if (it != m_buses.end())
        {
            it->second.bus.solo = solo;
        }
    }

    float AudioMixer::GetEffectiveBusVolume(const std::string& busName) const
    {
        auto it = m_buses.find(busName);
        if (it == m_buses.end())
        {
            return 0.0f;
        }

        float volume = it->second.bus.GetEffectiveVolume();

        // Walk up the parent chain
        std::string parent = it->second.bus.parentBus;
        while (!parent.empty())
        {
            auto parentIt = m_buses.find(parent);
            if (parentIt == m_buses.end())
            {
                break;
            }
            volume *= parentIt->second.bus.GetEffectiveVolume();
            parent = parentIt->second.bus.parentBus;
        }

        return volume;
    }

    std::vector<std::string> AudioMixer::GetBusNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_buses.size());
        for (const auto& [name, state] : m_buses)
        {
            names.push_back(name);
        }
        return names;
    }

    void AudioMixer::AddBusEffect(const std::string& busName, const DSPEffect& effect)
    {
        auto it = m_buses.find(busName);
        if (it != m_buses.end())
        {
            it->second.effects.push_back(effect);
        }
    }

    void AudioMixer::ClearBusEffects(const std::string& busName)
    {
        auto it = m_buses.find(busName);
        if (it != m_buses.end())
        {
            it->second.effects.clear();
        }
    }

    void AudioMixer::AddReverbZone(const ReverbZone& zone)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, zone.name);
        m_reverbZones.push_back(zone);
        // Sort by priority (highest first)
        std::sort(m_reverbZones.begin(), m_reverbZones.end(),
                  [](const ReverbZone& a, const ReverbZone& b) { return a.priority > b.priority; });
    }

    void AudioMixer::RemoveReverbZone(const std::string& name)
    {
        m_reverbZones.erase(std::remove_if(m_reverbZones.begin(), m_reverbZones.end(),
                                           [&name](const ReverbZone& z) { return z.name == name; }),
                            m_reverbZones.end());
    }

    ReverbParameters AudioMixer::GetReverbAtPosition(const DirectX::XMFLOAT3& position) const
    {
        ReverbParameters blended;
        float totalWeight = 0.0f;

        for (const auto& zone : m_reverbZones)
        {
            if (!zone.enabled)
            {
                continue;
            }

            // Check if position is within the zone's outer radius (box check)
            float dx = std::abs(position.x - zone.position.x);
            float dy = std::abs(position.y - zone.position.y);
            float dz = std::abs(position.z - zone.position.z);

            if (dx > zone.halfExtents.x || dy > zone.halfExtents.y || dz > zone.halfExtents.z)
            {
                continue;
            }

            // Calculate blend weight based on distance from center
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float maxDist =
                std::sqrt(zone.halfExtents.x * zone.halfExtents.x + zone.halfExtents.y * zone.halfExtents.y +
                          zone.halfExtents.z * zone.halfExtents.z);

            float weight = 1.0f;
            if (dist > zone.innerRadius && zone.outerRadius > zone.innerRadius)
            {
                weight = 1.0f - (dist - zone.innerRadius) / (zone.outerRadius - zone.innerRadius);
                weight = std::clamp(weight, 0.0f, 1.0f);
            }

            if (weight > 0.0f)
            {
                float priorityWeight = weight * static_cast<float>(zone.priority + 1);
                blended.decayTime += zone.reverb.decayTime * priorityWeight;
                blended.earlyReflections += zone.reverb.earlyReflections * priorityWeight;
                blended.lateReverb += zone.reverb.lateReverb * priorityWeight;
                blended.diffusion += zone.reverb.diffusion * priorityWeight;
                blended.density += zone.reverb.density * priorityWeight;
                blended.roomSize += zone.reverb.roomSize * priorityWeight;
                blended.wetDryMix += zone.reverb.wetDryMix * priorityWeight;
                blended.highFreqDamping += zone.reverb.highFreqDamping * priorityWeight;
                totalWeight += priorityWeight;
            }
        }

        if (totalWeight > 0.0f)
        {
            float invWeight = 1.0f / totalWeight;
            blended.decayTime *= invWeight;
            blended.earlyReflections *= invWeight;
            blended.lateReverb *= invWeight;
            blended.diffusion *= invWeight;
            blended.density *= invWeight;
            blended.roomSize *= invWeight;
            blended.wetDryMix *= invWeight;
            blended.highFreqDamping *= invWeight;
            blended.preset = ReverbPreset::Custom;
        }

        return blended;
    }

    OcclusionResult AudioMixer::CalculateOcclusion(const DirectX::XMFLOAT3& listenerPos,
                                                   const DirectX::XMFLOAT3& sourcePos) const
    {
        OcclusionResult result;

        if (!m_occlusionEnabled)
        {
            return result;
        }

        // Calculate distance-based attenuation as a baseline
        float dx = sourcePos.x - listenerPos.x;
        float dy = sourcePos.y - listenerPos.y;
        float dz = sourcePos.z - listenerPos.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Occlusion would normally use PhysicsSystem::Raycast() to count walls.
        // Without a physics reference, we provide the framework for integration.
        // The PhysicsUpdateSystem or AudioUpdateSystem should call this with
        // raycast results filled in.

        // Apply occlusion based on wall count
        if (result.wallCount > 0)
        {
            result.occlusionFactor = std::clamp(static_cast<float>(result.wallCount) * 0.3f, 0.0f, 1.0f);
            result.volumeScale = 1.0f - result.occlusionFactor * 0.7f;
            result.lowPassCutoff = 22000.0f * (1.0f - result.occlusionFactor * 0.8f);
        }

        (void)distance; // Used by derived implementations

        return result;
    }

    void AudioMixer::SaveSnapshot(const std::string& name)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, name);
        Snapshot snapshot;
        for (const auto& [busName, state] : m_buses)
        {
            snapshot.busVolumes[busName] = state.bus.volume;
        }
        m_snapshots[name] = std::move(snapshot);
    }

    void AudioMixer::RestoreSnapshot(const std::string& name, float blendTime)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, name);
        auto it = m_snapshots.find(name);
        if (it == m_snapshots.end())
        {
            return;
        }

        // Instant restore (blended restore would require per-frame interpolation)
        (void)blendTime;
        for (const auto& [busName, volume] : it->second.busVolumes)
        {
            SetBusVolume(busName, volume);
        }
    }

    std::string AudioMixer::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Audio Mixer ===\n";
        oss << "Buses: " << m_buses.size() << "\n";
        for (const auto& [name, state] : m_buses)
        {
            oss << "  " << name << ": vol=" << state.bus.volume;
            if (state.bus.muted)
            {
                oss << " [MUTED]";
            }
            if (state.bus.solo)
            {
                oss << " [SOLO]";
            }
            if (!state.effects.empty())
            {
                oss << " (" << state.effects.size() << " effects)";
            }
            oss << "\n";
        }
        oss << "Reverb zones: " << m_reverbZones.size() << "\n";
        oss << "Occlusion: " << (m_occlusionEnabled ? "ON" : "OFF") << "\n";
        oss << "Snapshots: " << m_snapshots.size() << "\n";
        return oss.str();
    }

    std::string AudioMixer::Console_ListReverbZones() const
    {
        std::ostringstream oss;
        oss << "=== Reverb Zones ===\n";
        for (const auto& zone : m_reverbZones)
        {
            oss << "  " << zone.name << " pos=(" << zone.position.x << "," << zone.position.y << "," << zone.position.z
                << ")" << " decay=" << zone.reverb.decayTime << "s" << " pri=" << zone.priority
                << (zone.enabled ? "" : " [DISABLED]") << "\n";
        }
        return oss.str();
    }

} // namespace Spark::Audio
