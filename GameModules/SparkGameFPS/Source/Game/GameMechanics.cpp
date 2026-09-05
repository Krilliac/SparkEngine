#include "GameMechanics.h"
#include "Core/Platform.h"
#include "Player.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;

namespace Spark
{

    // ============================================================================
    // DamageZone Implementation
    // ============================================================================

    bool DamageZone::Contains(const XMFLOAT3& point) const
    {
        if (!isActive)
            return false;
        return (point.x >= center.x - halfExtents.x && point.x <= center.x + halfExtents.x &&
                point.y >= center.y - halfExtents.y && point.y <= center.y + halfExtents.y &&
                point.z >= center.z - halfExtents.z && point.z <= center.z + halfExtents.z);
    }

    // ============================================================================
    // DamageZoneSystem Implementation
    // ============================================================================

    DamageZoneSystem::DamageZoneSystem() {}

    bool DamageZoneSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing DamageZoneSystem");
        m_zones.clear();
        m_damageTickTimer = 0.0f;
        return true;
    }

    void DamageZoneSystem::Update(float deltaTime, Player* player)
    {
        SPARK_WARN_IF(Spark::LogCategory::Game, !player, "DamageZoneSystem::Update called with null player");
        if (!player || !player->IsAlive())
            return;

        m_damageTickTimer += deltaTime;

        XMFLOAT3 playerPos = player->GetPosition();

        for (const auto& zone : m_zones)
        {
            if (!zone.isActive || !zone.Contains(playerPos))
                continue;

            // Instant kill zones
            if (zone.instantKill)
            {
                player->TakeDamage(99999.0f);
                return;
            }

            // Periodic damage
            if (m_damageTickTimer >= zone.tickInterval)
            {
                float damage = zone.damagePerSecond * zone.tickInterval;

                switch (zone.type)
                {
                case DamageZoneType::LAVA:
                    player->TakeDamage(damage);
                    break;

                case DamageZoneType::ACID:
                    player->TakeDamage(damage * 0.7f); // Acid is slower but persistent
                    break;

                case DamageZoneType::ELECTRIC:
                    // Electric does burst damage every tick
                    player->TakeDamage(damage * 1.5f);
                    break;

                case DamageZoneType::RADIATION:
                    player->TakeDamage(damage * 0.5f); // Slow constant damage
                    break;

                case DamageZoneType::VOID_ZONE:
                    player->TakeDamage(99999.0f); // Instant kill
                    break;
                }
            }
        }

        // Reset tick timer
        if (m_damageTickTimer >= 0.5f)
        {
            m_damageTickTimer = 0.0f;
        }
    }

    int DamageZoneSystem::AddZone(const DamageZone& zone)
    {
        if ((int)m_zones.size() >= MAX_DAMAGE_ZONES)
            return -1;
        m_zones.push_back(zone);
        return static_cast<int>(m_zones.size() - 1);
    }

    void DamageZoneSystem::RemoveZone(int index)
    {
        if (index >= 0 && index < (int)m_zones.size())
        {
            m_zones.erase(m_zones.begin() + index);
        }
    }

    void DamageZoneSystem::RemoveZoneByName(const std::string& name)
    {
        m_zones.erase(
            std::remove_if(m_zones.begin(), m_zones.end(), [&name](const DamageZone& z) { return z.name == name; }),
            m_zones.end());
    }

    DamageZone* DamageZoneSystem::GetZone(int index)
    {
        if (index >= 0 && index < (int)m_zones.size())
        {
            return &m_zones[index];
        }
        return nullptr;
    }

    void DamageZoneSystem::ClearZones()
    {
        m_zones.clear();
    }

    bool DamageZoneSystem::IsInDamageZone(const XMFLOAT3& position) const
    {
        for (const auto& zone : m_zones)
        {
            if (zone.Contains(position))
                return true;
        }
        return false;
    }

    DamageZoneType DamageZoneSystem::GetZoneTypeAt(const XMFLOAT3& position) const
    {
        for (const auto& zone : m_zones)
        {
            if (zone.Contains(position))
                return zone.type;
        }
        return DamageZoneType::LAVA; // default
    }

    int DamageZoneSystem::CreateLavaZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents)
    {
        DamageZone zone;
        zone.name = name;
        zone.type = DamageZoneType::LAVA;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.damagePerSecond = 50.0f;
        zone.tickInterval = 0.25f;
        zone.slowFactor = 0.3f;
        return AddZone(zone);
    }

    int DamageZoneSystem::CreateAcidZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents)
    {
        DamageZone zone;
        zone.name = name;
        zone.type = DamageZoneType::ACID;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.damagePerSecond = 20.0f;
        zone.tickInterval = 0.5f;
        zone.slowFactor = 0.5f;
        return AddZone(zone);
    }

    int DamageZoneSystem::CreateElectricZone(const std::string& name, const XMFLOAT3& center,
                                             const XMFLOAT3& halfExtents)
    {
        DamageZone zone;
        zone.name = name;
        zone.type = DamageZoneType::ELECTRIC;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.damagePerSecond = 35.0f;
        zone.tickInterval = 1.0f; // Shocks every second
        zone.slowFactor = 0.7f;
        return AddZone(zone);
    }

    int DamageZoneSystem::CreateVoidZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents)
    {
        DamageZone zone;
        zone.name = name;
        zone.type = DamageZoneType::VOID_ZONE;
        zone.center = center;
        zone.halfExtents = halfExtents;
        zone.instantKill = true;
        return AddZone(zone);
    }

    std::string DamageZoneSystem::Console_ListZones() const
    {
        std::stringstream ss;
        ss << "Damage Zones (" << m_zones.size() << "/" << MAX_DAMAGE_ZONES << "):\n";

        const char* typeNames[] = {"Lava", "Acid", "Electric", "Radiation", "Void"};

        for (size_t i = 0; i < m_zones.size(); ++i)
        {
            const auto& z = m_zones[i];
            int typeIdx = static_cast<int>(z.type);
            const char* typeName = (typeIdx >= 0 && typeIdx <= 4) ? typeNames[typeIdx] : "Unknown";
            ss << "  [" << i << "] \"" << z.name << "\" Type:" << typeName << " DPS:" << z.damagePerSecond
               << " Center:(" << z.center.x << "," << z.center.y << "," << z.center.z << ")"
               << (z.instantKill ? " [INSTANT KILL]" : "") << "\n";
        }
        return ss.str();
    }

} // namespace Spark
