/**
 * @file AISystem.h
 * @brief ECS system for AI agent processing and coordination
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../ECS/Systems/ECSystems.h"
#include "BehaviorTree.h"
#include "NavMesh.h"
#include <unordered_map>
#include <memory>

namespace Spark::AI {

/// ECS component for AI-controlled entities
struct AIComponent {
    std::string behaviorTreeName;
    AIAgentConfig config;
    void* behaviorTreeHandle = nullptr;  ///< Opaque handle to BehaviorTree
    void* navQueryHandle = nullptr;      ///< Opaque handle to NavMeshQuery

    /// Current AI state
    enum class State { Idle, Patrolling, Alert, Combat, Fleeing, Dead };
    State state = State::Idle;

    /// Perception data
    EntityID targetEntity = entt::null;
    XMFLOAT3 lastKnownTargetPos{0, 0, 0};
    float alertTimer = 0.0f;
    float timeSinceLastSeen = 0.0f;

    /// Movement
    std::vector<XMFLOAT3> currentPath;
    size_t currentPathIndex = 0;
    XMFLOAT3 moveTarget{0, 0, 0};
};

/// ECS system that processes all AI agents each frame
class AISystem : public Spark::ECS::ISystem {
public:
    AISystem();
    ~AISystem() override = default;

    void Update(World& world, float deltaTime) override;
    const char* GetName() const override { return "AISystem"; }

    /// Register a behavior tree template
    void RegisterBehavior(const std::string& name, std::unique_ptr<BehaviorTree> tree);

    /// Create a behavior tree instance for an agent
    BehaviorTree* CreateBehaviorInstance(const std::string& templateName);

    /// Console integration
    std::string Console_ListAgents(World& world) const;
    std::string Console_GetAgentInfo(World& world, EntityID entity) const;

private:
    void UpdatePerception(World& world, AIComponent& ai, const Transform& transform, float deltaTime);
    void UpdateMovement(World& world, AIComponent& ai, Transform& transform, float deltaTime);
    void UpdateBehavior(AIComponent& ai, float deltaTime);

    std::unordered_map<std::string, std::unique_ptr<BehaviorTree>> m_behaviorTemplates;
    std::vector<std::unique_ptr<BehaviorTree>> m_behaviorInstances;
};

} // namespace Spark::AI
