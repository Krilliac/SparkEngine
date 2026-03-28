/**
 * @file DestructionSystem.cpp
 * @brief Implementation of the destructible environment system
 */

#include "DestructionSystem.h"
#include "../ECS/Components.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark
{

    DestructionSystem::DestructionSystem() = default;

    void DestructionSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        if (m_initialized)
            return;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "DestructionSystem initializing");
        // Register some default fracture patterns
        FracturePattern woodenCrate;
        woodenCrate.AddPiece({"plank1", "debris_plank", {0.3f, 0, 0}, 0.5f, 8.0f, 3.0f});
        woodenCrate.AddPiece({"plank2", "debris_plank", {-0.3f, 0, 0}, 0.5f, 8.0f, 3.0f});
        woodenCrate.AddPiece({"plank3", "debris_plank", {0, 0.3f, 0}, 0.5f, 8.0f, 4.0f});
        woodenCrate.AddPiece({"plank4", "debris_plank", {0, -0.3f, 0}, 0.5f, 8.0f, 2.0f});
        woodenCrate.SetDestructionSound("sfx_wood_break");
        woodenCrate.SetParticleEffect("fx_wood_splinters");
        RegisterPattern("wooden_crate", woodenCrate);
        SPARK_LOG_DEBUG(Spark::LogCategory::Physics, "Registered default pattern: wooden_crate");

        FracturePattern metalBarrel;
        metalBarrel.AddPiece({"shell_top", "debris_metal_curved", {0, 0.4f, 0}, 2.0f, 12.0f, 6.0f});
        metalBarrel.AddPiece({"shell_bottom", "debris_metal_curved", {0, -0.4f, 0}, 2.0f, 12.0f, 5.0f});
        metalBarrel.AddPiece({"fragment1", "debris_metal_small", {0.2f, 0, 0.2f}, 0.3f, 10.0f, 8.0f});
        metalBarrel.SetDestructionSound("sfx_metal_break");
        metalBarrel.SetParticleEffect("fx_metal_sparks");
        RegisterPattern("metal_barrel", metalBarrel);
        SPARK_LOG_DEBUG(Spark::LogCategory::Physics, "Registered default pattern: metal_barrel");

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
        SPARK_LOG_DEBUG(Spark::LogCategory::Physics, "Registered default pattern: concrete_wall");
        SPARK_LOG_INFO(Spark::LogCategory::Physics, "DestructionSystem initialized with %zu default patterns",
                       m_patterns.size());
    }

    void DestructionSystem::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        // Update debris lifetimes
        for (auto& debris : m_debris)
        {
            debris.remainingLifetime -= deltaTime;
        }

        // Destroy expired debris entities from the ECS world and remove from our list
        m_debris.erase(std::remove_if(m_debris.begin(), m_debris.end(),
                                      [this](const DebrisInstance& d)
                                      {
                                          if (d.remainingLifetime <= 0.0f)
                                          {
                                              if (m_world && d.debrisEntity != 0)
                                              {
                                                  auto entId = static_cast<entt::entity>(d.debrisEntity);
                                                  if (m_world->GetRegistry().valid(entId))
                                                  {
                                                      m_world->DestroyEntity(entId);
                                                  }
                                              }
                                              return true;
                                          }
                                          return false;
                                      }),
                       m_debris.end());

        m_activeDebrisCount = m_debris.size();
    }

    void DestructionSystem::RegisterPattern(const std::string& name, const FracturePattern& pattern)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Physics, name);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "DestructionSystem: registered pattern '%s' (%zu pieces)",
                       name.c_str(), pattern.GetPieces().size());
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

        DestructionEvent event;
        event.entityId = entityId;
        event.position = hitPoint;
        event.impactDir = hitDir;
        event.impactForce = damage;

        // Look up the fracture pattern for this entity
        std::string patternName;
        if (m_world)
        {
            auto entId = static_cast<entt::entity>(entityId);
            auto* destComp = m_world->GetComponent<DestructibleComponent>(entId);
            if (destComp)
            {
                patternName = destComp->patternName;
            }
        }
        event.patternName = patternName;

        // Spawn debris pieces from the fracture pattern
        const FracturePattern* pattern = patternName.empty() ? nullptr : GetPattern(patternName);
        if (pattern && m_world)
        {
            for (const auto& piece : pattern->GetPieces())
            {
                if (m_activeDebrisCount >= m_maxDebris)
                    break;

                // Create a new ECS entity for each debris piece
                auto debrisEntity = m_world->CreateEntity("debris_" + piece.name);

                // Position debris at hit point + piece offset, scattered by impact direction
                auto& transform = m_world->AddComponent<Transform>(debrisEntity);
                transform.position.x = hitPoint.x + piece.localOffset.x;
                transform.position.y = hitPoint.y + piece.localOffset.y;
                transform.position.z = hitPoint.z + piece.localOffset.z;

                // Assign debris mesh
                auto& renderer = m_world->AddComponent<MeshRenderer>(debrisEntity);
                renderer.meshPath = piece.meshName;
                renderer.castShadows = false;

                // Give debris physics so it scatters realistically
                auto& rb = m_world->AddComponent<RigidBodyComponent>(debrisEntity);
                rb.type = RigidBodyComponent::Type::Dynamic;
                rb.mass = piece.mass;
                rb.linearDamping = 0.3f;
                rb.angularDamping = 0.4f;

                // Apply scatter force in the impact direction
                float forceScale = piece.scatterForce * (damage / 100.0f);
                rb.linearVelocity.x = hitDir.x * forceScale + piece.localOffset.x * 2.0f;
                rb.linearVelocity.y = std::abs(hitDir.y) * forceScale + 3.0f;
                rb.linearVelocity.z = hitDir.z * forceScale + piece.localOffset.z * 2.0f;

                DebrisInstance debrisInst;
                debrisInst.entityId = entityId;
                debrisInst.debrisEntity = static_cast<uint32_t>(debrisEntity);
                debrisInst.remainingLifetime = piece.lifetime * m_debrisLifetimeMultiplier;
                m_debris.push_back(debrisInst);
                m_activeDebrisCount = m_debris.size();
            }
        }
        else if (m_activeDebrisCount < m_maxDebris)
        {
            // Fallback: create a single generic debris entity
            DebrisInstance debris;
            debris.entityId = entityId;
            debris.debrisEntity = 0;
            debris.remainingLifetime = 10.0f * m_debrisLifetimeMultiplier;
            m_debris.push_back(debris);
            m_activeDebrisCount = m_debris.size();
        }

        m_totalDestructions++;
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "DestructionSystem: entity %u destroyed (damage=%.1f, pattern='%s', debris=%zu)", entityId,
                       damage, patternName.c_str(), m_debris.size());

        m_destructionCallbacks.Broadcast(event);
    }

    void DestructionSystem::ForceDestroy(uint32_t entityId, float force)
    {
        DirectX::XMFLOAT3 origin{0, 0, 0};
        DirectX::XMFLOAT3 upDir{0, 1, 0};
        ApplyDamage(entityId, force, origin, upDir);
    }

    Spark::Delegate<const DestructionEvent&>::HandlerID DestructionSystem::OnDestruction(
        std::function<void(const DestructionEvent&)> callback)
    {
        return (m_destructionCallbacks += std::move(callback));
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
