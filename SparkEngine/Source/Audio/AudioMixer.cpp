/**
 * @file AudioMixer.cpp
 * @brief Implementation of the audio mixer, reverb zones, and occlusion
 */

#include "AudioMixer.h"
#include "../Utils/Validate.h"
#include "Physics/PhysicsSystem.h"

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

    void AudioMixer::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Audio, "AudioMixer::Shutdown - releasing buses, zones and snapshots");
        m_buses.clear();
        m_reverbZones.clear();
        m_snapshots.clear();
        m_physics = nullptr;
        m_listenerPosition = {0.0f, 0.0f, 0.0f};
    }

    void AudioMixer::Update(const DirectX::XMFLOAT3& listenerPos, float deltaTime)
    {
        (void)deltaTime;
        m_listenerPosition = listenerPos;
    }

    void AudioMixer::SetPhysics(PhysicsSystem* physics)
    {
        m_physics = physics;
    }

    void AudioMixer::CreateBus(const std::string& name, const std::string& parentBus)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, name);
        SPARK_LOG_DEBUG(Spark::LogCategory::Audio, "AudioMixer::CreateBus '%s' parent='%s'", name.c_str(),
                        parentBus.c_str());
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
            SPARK_LOG_DEBUG(Spark::LogCategory::Audio, "AudioMixer::SetBusVolume '%s' vol=%.2f", busName.c_str(),
                            volume);
            it->second.bus.volume = std::clamp(volume, 0.0f, 1.0f);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Audio, "AudioMixer::SetBusVolume bus '%s' not found", busName.c_str());
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
            SPARK_LOG_INFO(Spark::LogCategory::Audio, "AudioMixer::SetBusMuted '%s' muted=%s", busName.c_str(),
                           muted ? "true" : "false");
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

    bool AudioMixer::IsAudibleUnderSolo(const std::string& busName) const
    {
        const bool anySolo =
            std::any_of(m_buses.begin(), m_buses.end(), [](const auto& entry) { return entry.second.bus.solo; });
        if (!anySolo)
        {
            return true;
        }

        // The bus itself, or one of its ancestors, is soloed.
        std::string current = busName;
        for (size_t step = 0; step <= m_buses.size() && !current.empty(); ++step)
        {
            auto it = m_buses.find(current);
            if (it == m_buses.end())
            {
                break;
            }
            if (it->second.bus.solo)
            {
                return true;
            }
            current = it->second.bus.parentBus;
        }

        // The bus is an ancestor of a soloed bus, so it must still pass audio.
        for (const auto& [name, state] : m_buses)
        {
            if (!state.bus.solo)
            {
                continue;
            }
            std::string parent = state.bus.parentBus;
            for (size_t step = 0; step <= m_buses.size() && !parent.empty(); ++step)
            {
                if (parent == busName)
                {
                    return true;
                }
                auto parentIt = m_buses.find(parent);
                if (parentIt == m_buses.end())
                {
                    break;
                }
                parent = parentIt->second.bus.parentBus;
            }
        }

        return false;
    }

    float AudioMixer::GetEffectiveBusVolume(const std::string& busName) const
    {
        auto it = m_buses.find(busName);
        if (it == m_buses.end())
        {
            return 0.0f;
        }

        // Solo used to be stored and never read, so soloing a bus silenced
        // nothing. A soloed selection now mutes every unrelated bus.
        if (!IsAudibleUnderSolo(busName))
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

        // No physics system means occlusion was never measured. Report
        // unoccluded (the only honest answer) rather than inventing walls --
        // IsOcclusionAvailable() tells callers which of the two this is.
        if (!IsOcclusionAvailable())
        {
            return result;
        }

        const float dx = sourcePos.x - listenerPos.x;
        const float dy = sourcePos.y - listenerPos.y;
        const float dz = sourcePos.z - listenerPos.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < 1e-4f)
        {
            return result;
        }

        // Count the static world surfaces between listener and source. Dynamic
        // bodies, triggers and debris deliberately do not occlude.
        const float invDistance = 1.0f / distance;
        const DirectX::XMFLOAT3 direction{dx * invDistance, dy * invDistance, dz * invDistance};
        const auto hits = m_physics->RaycastAllFiltered(listenerPos, direction, distance, CollisionLayers::WorldStatic);
        result.wallCount = static_cast<int>(hits.size());

        if (result.wallCount > 0)
        {
            result.occlusionFactor = std::clamp(static_cast<float>(result.wallCount) * 0.3f, 0.0f, 1.0f);
            result.volumeScale = 1.0f - result.occlusionFactor * 0.7f;
            result.lowPassCutoff = 22000.0f * (1.0f - result.occlusionFactor * 0.8f);
        }

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
        size_t configuredDsp = 0;
        for (const auto& [name, state] : m_buses)
        {
            configuredDsp += state.effects.size();
        }
        if (configuredDsp > 0)
        {
            oss << "DSP chain: " << configuredDsp << " configured, stored only (not applied to voices)\n";
        }
        oss << "Reverb zones: " << m_reverbZones.size() << " (parameters read by consumers; no reverb rendered)\n";
        oss << "Occlusion: " << (m_occlusionEnabled ? "ON" : "OFF")
            << (m_physics ? " (tracing world geometry)" : " (no physics attached - reporting unoccluded)") << "\n";
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
