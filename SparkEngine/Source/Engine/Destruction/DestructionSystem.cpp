/**
 * @file DestructionSystem.cpp
 * @brief Implementation of the destructible environment system
 */

#include "DestructionSystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <sstream>

namespace Spark
{

    DestructionSystem::DestructionSystem() = default;

    void DestructionSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        SPARK_LOG_INFO(Spark::LogCategory::Physics, "DestructionSystem initializing");
        // Register some default fracture patterns
        FracturePattern woodenCrate;
        woodenCrate.AddPiece({"plank1", "debris_plank", {0.3f, 0, 0}, 0.5f, 8.0f, 3.0f});
        woodenCrate.AddPiece({"plank2", "debris_plank", {-0.3f, 0, 0}, 0.5f, 8.0f, 3.0f});
        woodenCrate.AddPiece({"plank3", "debris_plank", {0, 0.3f, 0}, 0.5f, 8.0f, 4.0f});
        woodenCrate.AddPiece({"plank4", "debris_plank", {0, -0.3f, 0}, 0.5f, 8.0f, 2.0f});
        woodenCrate.SetDestructionSound("sfx_wood_break");
        woodenCrate.SetParticleEffect("fx_wood_splinters");
        RegisterPattern("wooden_crate", woodenCrate);

        FracturePattern metalBarrel;
        metalBarrel.AddPiece({"shell_top", "debris_metal_curved", {0, 0.4f, 0}, 2.0f, 12.0f, 6.0f});
        metalBarrel.AddPiece({"shell_bottom", "debris_metal_curved", {0, -0.4f, 0}, 2.0f, 12.0f, 5.0f});
        metalBarrel.AddPiece({"fragment1", "debris_metal_small", {0.2f, 0, 0.2f}, 0.3f, 10.0f, 8.0f});
        metalBarrel.SetDestructionSound("sfx_metal_break");
        metalBarrel.SetParticleEffect("fx_metal_sparks");
        RegisterPattern("metal_barrel", metalBarrel);

        FracturePattern concreteWall;
        for (int i = 0; i < 8; ++i)
        {
            float x = (i % 4) * 0.3f - 0.45f;
            float y = (i / 4) * 0.5f - 0.25f;
            concreteWall.AddPiece({"chunk_" + std::to_string(i), "debris_concrete", {x, y, 0}, 3.0f, 15.0f, 4.0f});
        }
        concreteWall.SetDestructionSound("sfx_concrete_break");
        concreteWall.SetParticleEffect("fx_concrete_dust");
        RegisterPattern("concrete_wall", concreteWall);
    }

    void DestructionSystem::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        // Update debris lifetimes
        for (auto& debris : m_debris)
        {
            debris.remainingLifetime -= deltaTime;
        }

        // Remove expired debris
        size_t before = m_debris.size();
        m_debris.erase(std::remove_if(m_debris.begin(), m_debris.end(),
                                      [](const DebrisInstance& d) { return d.remainingLifetime <= 0.0f; }),
                       m_debris.end());

        m_activeDebrisCount = m_debris.size();

        (void)before;
    }

    void DestructionSystem::RegisterPattern(const std::string& name, const FracturePattern& pattern)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Physics, name);
        m_patterns[name] = pattern;
    }

    const FracturePattern* DestructionSystem::GetPattern(const std::string& name) const
    {
        auto it = m_patterns.find(name);
        return it != m_patterns.end() ? &it->second : nullptr;
    }

    void DestructionSystem::ApplyDamage(uint32_t entityId, float damage, const DirectX::XMFLOAT3& hitPoint,
                                        const DirectX::XMFLOAT3& hitDir)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        SPARK_WARN_IF(Spark::LogCategory::Physics, damage < 0.0f, "ApplyDamage called with negative damage value");
        // Damage application is handled via DestructibleComponent::ApplyDamage
        // This method handles the fracturing/debris spawning after destruction

        // The ECS system or game code calls DestructibleComponent::ApplyDamage first,
        // then if isDestroyed becomes true, calls this to spawn debris

        DestructionEvent event;
        event.entityId = entityId;
        event.position = hitPoint;
        event.impactDir = hitDir;
        event.impactForce = damage;

        // Spawn debris pieces if under the limit
        // (Actual debris entity creation would happen via the ECS World)
        if (m_activeDebrisCount < m_maxDebris)
        {
            // Placeholder: real implementation creates debris entities
            DebrisInstance debris;
            debris.entityId = entityId;
            debris.remainingLifetime = 10.0f * m_debrisLifetimeMultiplier;
            m_debris.push_back(debris);
            m_activeDebrisCount = m_debris.size();
        }

        m_totalDestructions++;

        for (const auto& callback : m_destructionCallbacks)
        {
            callback(event);
        }
    }

    void DestructionSystem::ForceDestroy(uint32_t entityId, float force)
    {
        DirectX::XMFLOAT3 origin{0, 0, 0};
        DirectX::XMFLOAT3 upDir{0, 1, 0};
        ApplyDamage(entityId, force, origin, upDir);
    }

    void DestructionSystem::OnDestruction(std::function<void(const DestructionEvent&)> callback)
    {
        m_destructionCallbacks.push_back(std::move(callback));
    }

    std::string DestructionSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Destruction System ===\n";
        oss << "Patterns: " << m_patterns.size() << "\n";
        for (const auto& [name, pattern] : m_patterns)
        {
            oss << "  " << name << ": " << pattern.GetPieces().size() << " pieces\n";
        }
        oss << "Active debris: " << m_activeDebrisCount << "/" << m_maxDebris << "\n";
        oss << "Total destructions: " << m_totalDestructions << "\n";
        oss << "Debris lifetime multiplier: " << m_debrisLifetimeMultiplier << "x\n";
        return oss.str();
    }

} // namespace Spark
