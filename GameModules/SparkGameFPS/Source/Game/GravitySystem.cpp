#include "GravitySystem.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;

namespace Spark
{

    // ============================================================================
    // GravityZone Implementation
    // ============================================================================

    bool GravityZone::Contains(const XMFLOAT3& point) const
    {
        if (!isActive)
            return false;

        // For RADIAL type, use sphere containment
        if (type == GravityZoneType::RADIAL)
        {
            float dx = point.x - center.x;
            float dy = point.y - center.y;
            float dz = point.z - center.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            float radiusSq = halfExtents.x * halfExtents.x; // Use x as radius
            return distSq <= radiusSq;
        }

        // AABB containment test
        return (point.x >= center.x - halfExtents.x && point.x <= center.x + halfExtents.x &&
                point.y >= center.y - halfExtents.y && point.y <= center.y + halfExtents.y &&
                point.z >= center.z - halfExtents.z && point.z <= center.z + halfExtents.z);
    }

    XMFLOAT3 GravityZone::GetGravityAt(const XMFLOAT3& point) const
    {
        switch (type)
        {
        case GravityZoneType::NORMAL:
            return {0, gravityStrength, 0};

        case GravityZoneType::LOW_GRAVITY:
            return {0, gravityStrength, 0}; // e.g., -5 instead of -20

        case GravityZoneType::ZERO_GRAVITY:
            return {0, 0, 0};

        case GravityZoneType::HIGH_GRAVITY:
            return {0, gravityStrength, 0}; // e.g., -40

        case GravityZoneType::REVERSE:
            return {0, std::abs(gravityStrength), 0}; // Positive = upward

        case GravityZoneType::DIRECTIONAL:
            return {gravityDirection.x * std::abs(gravityStrength), gravityDirection.y * std::abs(gravityStrength),
                    gravityDirection.z * std::abs(gravityStrength)};

        case GravityZoneType::RADIAL:
        {
            // Pull toward radialCenter
            float dx = radialCenter.x - point.x;
            float dy = radialCenter.y - point.y;
            float dz = radialCenter.z - point.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < 0.01f)
                return {0, 0, 0};
            float invDist = 1.0f / dist;
            float strength = std::abs(gravityStrength);
            return {dx * invDist * strength, dy * invDist * strength, dz * invDist * strength};
        }

        default:
            return {0, -20.0f, 0};
        }
    }

    // ============================================================================
    // GravitySystem Implementation
    // ============================================================================

    GravitySystem::GravitySystem() {}

    bool GravitySystem::Initialize(const XMFLOAT3& worldGravity)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing GravitySystem");
        m_worldGravity = worldGravity;
        m_zones.clear();
        return true;
    }

    void GravitySystem::Update(float deltaTime)
    {
        // Remove inactive zones if needed (currently just a placeholder)
        // Zones are generally persistent
    }

    int GravitySystem::AddZone(const GravityZone& zone)
    {
        if ((int)m_zones.size() >= MAX_ZONES)
            return -1;
        m_zones.push_back(zone);
        return static_cast<int>(m_zones.size() - 1);
    }

    void GravitySystem::RemoveZone(int index)
    {
        if (index >= 0 && index < (int)m_zones.size())
        {
            m_zones.erase(m_zones.begin() + index);
        }
    }

    void GravitySystem::RemoveZoneByName(const std::string& name)
    {
        m_zones.erase(
            std::remove_if(m_zones.begin(), m_zones.end(), [&name](const GravityZone& z) { return z.name == name; }),
            m_zones.end());
    }

    GravityZone* GravitySystem::GetZone(int index)
    {
        if (index >= 0 && index < (int)m_zones.size())
        {
            return &m_zones[index];
        }
        return nullptr;
    }

    GravityZone* GravitySystem::GetZoneByName(const std::string& name)
    {
        for (auto& zone : m_zones)
        {
            if (zone.name == name)
                return &zone;
        }
        return nullptr;
    }

    void GravitySystem::ClearZones()
    {
        m_zones.clear();
    }

    XMFLOAT3 GravitySystem::GetGravityAt(const XMFLOAT3& position) const
    {
        int bestZone = -1;
        int bestPriority = -999999;

        for (int i = 0; i < (int)m_zones.size(); ++i)
        {
            if (m_zones[i].Contains(position) && m_zones[i].priority > bestPriority)
            {
                bestPriority = m_zones[i].priority;
                bestZone = i;
            }
        }

        if (bestZone >= 0)
        {
            return m_zones[bestZone].GetGravityAt(position);
        }

        return m_worldGravity;
    }

    GravityState GravitySystem::UpdateObjectGravity(const XMFLOAT3& position, GravityState& state, float dt) const
    {
        XMFLOAT3 targetGravity = GetGravityAt(position);
        int zoneIndex = GetZoneAt(position);

        // Check if zone changed
        if (zoneIndex != state.activeZoneIndex)
        {
            state.activeZoneIndex = zoneIndex;
            state.startGravity = state.currentGravity;
            state.targetGravity = targetGravity;
            state.transitionProgress = 0.0f;
            state.inZone = (zoneIndex >= 0);
        }

        // Smooth transition
        float transitionSpeed = 5.0f;
        if (zoneIndex >= 0 && zoneIndex < (int)m_zones.size())
        {
            transitionSpeed = m_zones[zoneIndex].transitionSpeed;
        }

        state.transitionProgress = std::min(1.0f, state.transitionProgress + transitionSpeed * dt);

        // Lerp gravity from start to target using absolute progress
        float t = state.transitionProgress;
        state.currentGravity.x = state.startGravity.x + (state.targetGravity.x - state.startGravity.x) * t;
        state.currentGravity.y = state.startGravity.y + (state.targetGravity.y - state.startGravity.y) * t;
        state.currentGravity.z = state.startGravity.z + (state.targetGravity.z - state.startGravity.z) * t;

        return state;
    }

    bool GravitySystem::IsInGravityZone(const XMFLOAT3& position) const
    {
        for (const auto& zone : m_zones)
        {
            if (zone.Contains(position))
                return true;
        }
        return false;
    }

    int GravitySystem::GetZoneAt(const XMFLOAT3& position) const
    {
        int bestZone = -1;
        int bestPriority = -999999;

        for (int i = 0; i < (int)m_zones.size(); ++i)
        {
            if (m_zones[i].Contains(position) && m_zones[i].priority > bestPriority)
            {
                bestPriority = m_zones[i].priority;
                bestZone = i;
            }
        }
        return bestZone;
    }

    // === Preset creation ===

    int GravitySystem::CreateLowGravityZone(const std::string& name, const XMFLOAT3& center,
                                            const XMFLOAT3& halfExtents, float strength)
    {
        GravityZone zone;
        zone.name = name;
        zone.type = GravityZoneType::LOW_GRAVITY;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.gravityStrength = strength;
        zone.transitionSpeed = 3.0f;
        return AddZone(zone);
    }

    int GravitySystem::CreateZeroGravityZone(const std::string& name, const XMFLOAT3& center,
                                             const XMFLOAT3& halfExtents)
    {
        GravityZone zone;
        zone.name = name;
        zone.type = GravityZoneType::ZERO_GRAVITY;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.gravityStrength = 0.0f;
        zone.transitionSpeed = 2.0f;
        zone.damping = 0.02f; // Slight damping in zero-G
        return AddZone(zone);
    }

    int GravitySystem::CreateReverseGravityZone(const std::string& name, const XMFLOAT3& center,
                                                const XMFLOAT3& halfExtents, float strength)
    {
        GravityZone zone;
        zone.name = name;
        zone.type = GravityZoneType::REVERSE;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.gravityStrength = strength;
        zone.transitionSpeed = 2.0f;
        return AddZone(zone);
    }

    int GravitySystem::CreateHighGravityZone(const std::string& name, const XMFLOAT3& center,
                                             const XMFLOAT3& halfExtents, float strength)
    {
        GravityZone zone;
        zone.name = name;
        zone.type = GravityZoneType::HIGH_GRAVITY;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.gravityStrength = strength;
        zone.transitionSpeed = 4.0f;
        return AddZone(zone);
    }

    int GravitySystem::CreateRadialGravityZone(const std::string& name, const XMFLOAT3& center, float radius,
                                               float strength)
    {
        GravityZone zone;
        zone.name = name;
        zone.type = GravityZoneType::RADIAL;
        zone.center = center;
        zone.halfExtents = {radius, radius, radius}; // Radius stored in x
        zone.radialCenter = center;
        zone.gravityStrength = strength;
        zone.transitionSpeed = 3.0f;
        return AddZone(zone);
    }

    // === Console Integration ===

    std::string GravitySystem::Console_ListZones() const
    {
        std::stringstream ss;
        ss << "Gravity Zones (" << m_zones.size() << "/" << MAX_ZONES << "):\n";
        ss << "World Gravity: (" << m_worldGravity.x << ", " << m_worldGravity.y << ", " << m_worldGravity.z << ")\n";

        const char* typeNames[] = {"Normal", "Low", "Zero", "High", "Reverse", "Directional", "Radial"};

        for (size_t i = 0; i < m_zones.size(); ++i)
        {
            const auto& z = m_zones[i];
            int typeIdx = static_cast<int>(z.type);
            const char* typeName = (typeIdx >= 0 && typeIdx <= 6) ? typeNames[typeIdx] : "Unknown";
            ss << "  [" << i << "] \"" << z.name << "\" Type:" << typeName << " Center:(" << z.center.x << ","
               << z.center.y << "," << z.center.z << ")" << " Strength:" << z.gravityStrength
               << (z.isActive ? " [ACTIVE]" : " [INACTIVE]") << "\n";
        }
        return ss.str();
    }

    bool GravitySystem::Console_AddZone(const std::string& name, const std::string& type, float cx, float cy, float cz,
                                        float hx, float hy, float hz, float strength)
    {
        GravityZoneType zoneType = GravityZoneType::NORMAL;
        if (type == "low")
            zoneType = GravityZoneType::LOW_GRAVITY;
        else if (type == "zero")
            zoneType = GravityZoneType::ZERO_GRAVITY;
        else if (type == "high")
            zoneType = GravityZoneType::HIGH_GRAVITY;
        else if (type == "reverse")
            zoneType = GravityZoneType::REVERSE;
        else if (type == "radial")
            zoneType = GravityZoneType::RADIAL;

        GravityZone zone;
        zone.name = name;
        zone.type = zoneType;
        zone.center = {cx, cy, cz};
        zone.halfExtents = {hx, hy, hz};
        zone.gravityStrength = strength;
        return AddZone(zone) >= 0;
    }

    bool GravitySystem::Console_RemoveZone(const std::string& name)
    {
        size_t before = m_zones.size();
        RemoveZoneByName(name);
        return m_zones.size() < before;
    }

    void GravitySystem::Console_SetWorldGravity(float x, float y, float z)
    {
        m_worldGravity = {x, y, z};
    }

} // namespace Spark
