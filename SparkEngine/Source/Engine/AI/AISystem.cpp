/**
 * @file AISystem.cpp
 * @brief AI system implementation — processes all AI agents each frame
 */

#include "AISystem.h"
#include <sstream>
#include <cmath>
#include <numbers>

using namespace DirectX;
namespace Spark::AI
{
    // Named constants replacing magic numbers
    static constexpr float kTargetLostTimeout = 10.0f;      // seconds before losing a target
    static constexpr float kWaypointArrivalRadius = 0.5f;    // meters to consider waypoint reached
    static constexpr float kMinMovementEpsilon = 0.001f;     // minimum distance to apply movement

    // ============================================================================
    // AISystem
    // ============================================================================

    AISystem::AISystem() = default;

    void AISystem::Update(World& world, float deltaTime)
    {
        auto view = world.GetEntitiesWith<Transform, AIComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& ai = view.get<AIComponent>(entity);

            // Skip dead agents
            auto* health = world.GetComponent<HealthComponent>(entity);
            if (health && health->isDead)
            {
                ai.state = AIComponent::State::Dead;
                continue;
            }

            UpdatePerception(world, ai, transform, deltaTime);
            UpdateBehavior(ai, deltaTime);
            UpdateMovement(world, ai, transform, deltaTime);
        }
    }

    void AISystem::RegisterBehavior(const std::string& name, std::unique_ptr<BehaviorTree> tree)
    {
        m_behaviorTemplates[name] = std::move(tree);
    }

    BehaviorTree* AISystem::CreateBehaviorInstance(const std::string& templateName)
    {
        auto it = m_behaviorTemplates.find(templateName);
        if (it == m_behaviorTemplates.end())
            return nullptr;

        // Create a new behavior tree instance (in production, this would deep-copy the template)
        auto instance = std::make_unique<BehaviorTree>(templateName);
        BehaviorTree* ptr = instance.get();
        m_behaviorInstances.push_back(std::move(instance));
        return ptr;
    }

    void AISystem::UpdatePerception(World& world, AIComponent& ai, const Transform& transform, float deltaTime)
    {
        if (ai.targetEntity != entt::null)
        {
            ai.timeSinceLastSeen += deltaTime;

            auto* targetTransform = world.GetComponent<Transform>(ai.targetEntity);
            if (targetTransform)
            {
                float dx = targetTransform->position.x - transform.position.x;
                float dy = targetTransform->position.y - transform.position.y;
                float dz = targetTransform->position.z - transform.position.z;
                float distSq = dx * dx + dy * dy + dz * dz;

                if (distSq < ai.config.detectionRange * ai.config.detectionRange)
                {
                    ai.lastKnownTargetPos = targetTransform->position;
                    ai.timeSinceLastSeen = 0.0f;

                    if (ai.state == AIComponent::State::Idle || ai.state == AIComponent::State::Patrolling)
                    {
                        ai.alertTimer = ai.config.reactionTime;
                        ai.state = AIComponent::State::Alert;
                    }
                }
            }

            // Lose target after not seeing them for a while
            if (ai.timeSinceLastSeen > kTargetLostTimeout)
            {
                ai.targetEntity = entt::null;
                ai.state = AIComponent::State::Patrolling;
            }
        }

        // Update alert timer
        if (ai.state == AIComponent::State::Alert)
        {
            ai.alertTimer -= deltaTime;
            if (ai.alertTimer <= 0.0f)
            {
                ai.state = AIComponent::State::Combat;
            }
        }
    }

    void AISystem::UpdateBehavior(AIComponent& ai, float deltaTime)
    {
        if (!ai.behaviorTreeHandle)
            return;

        auto* bt = ai.behaviorTreeHandle.As<BehaviorTree>();
        auto& bb = bt->GetBlackboard();

        // Push AI state to blackboard
        bb.Set("state", static_cast<int>(ai.state));
        bb.Set("hasTarget", ai.targetEntity != entt::null);
        bb.Set("targetPosition", ai.lastKnownTargetPos);
        bb.Set("timeSinceLastSeen", ai.timeSinceLastSeen);

        bt->Tick(deltaTime);
    }

    void AISystem::UpdateMovement(World& world, AIComponent& ai, Transform& transform, float deltaTime)
    {
        if (ai.currentPath.empty() || ai.currentPathIndex >= ai.currentPath.size())
            return;

        const auto& target = ai.currentPath[ai.currentPathIndex];
        float dx = target.x - transform.position.x;
        float dz = target.z - transform.position.z;
        float distSq = dx * dx + dz * dz;

        // Reached waypoint?
        if (distSq < kWaypointArrivalRadius * kWaypointArrivalRadius)
        {
            ai.currentPathIndex++;
            if (ai.currentPathIndex >= ai.currentPath.size())
            {
                ai.currentPath.clear();
                ai.currentPathIndex = 0;
                return;
            }
        }

        // Move toward current waypoint
        float dist = std::sqrt(distSq);
        if (dist > kMinMovementEpsilon)
        {
            float speed = ai.config.moveSpeed * deltaTime;
            float moveX = (dx / dist) * speed;
            float moveZ = (dz / dist) * speed;
            transform.position.x += moveX;
            transform.position.z += moveZ;

            // Face movement direction
            transform.rotation.y = std::atan2(dx, dz) * (180.0f / std::numbers::pi_v<float>);
        }
    }

    std::string AISystem::Console_ListAgents(World& world) const
    {
        std::ostringstream ss;
        int count = 0;
        auto view = world.GetEntitiesWith<AIComponent>();
        for (auto entity : view)
        {
            count++;
            auto& ai = view.get<AIComponent>(entity);
            ss << "  Agent " << static_cast<uint32_t>(entity) << " [";
            switch (ai.state)
            {
            case AIComponent::State::Idle:
                ss << "Idle";
                break;
            case AIComponent::State::Patrolling:
                ss << "Patrolling";
                break;
            case AIComponent::State::Alert:
                ss << "Alert";
                break;
            case AIComponent::State::Combat:
                ss << "Combat";
                break;
            case AIComponent::State::Fleeing:
                ss << "Fleeing";
                break;
            case AIComponent::State::Dead:
                ss << "Dead";
                break;
            }
            ss << "] BT: " << ai.behaviorTreeName << "\n";
        }
        ss << "Total: " << count << " agents\n";
        return ss.str();
    }

    std::string AISystem::Console_GetAgentInfo(World& world, EntityID entity) const
    {
        auto* ai = world.GetComponent<AIComponent>(entity);
        if (!ai)
            return "Entity has no AIComponent\n";

        std::ostringstream ss;
        ss << "=== AI Agent " << static_cast<uint32_t>(entity) << " ===\n";
        ss << "Behavior: " << ai->behaviorTreeName << "\n";
        ss << "Detection Range: " << ai->config.detectionRange << "\n";
        ss << "Attack Range: " << ai->config.attackRange << "\n";
        ss << "Move Speed: " << ai->config.moveSpeed << "\n";
        ss << "Accuracy: " << ai->config.accuracy << "\n";
        ss << "Path Points: " << ai->currentPath.size() << "\n";
        ss << "Has Target: " << (ai->targetEntity != entt::null ? "Yes" : "No") << "\n";
        return ss.str();
    }

    // ============================================================================
    // Pre-built FPS Behaviors
    // ============================================================================

    namespace FPSBehaviors
    {

        std::unique_ptr<BehaviorTree> CreatePatrolBehavior(const std::vector<XMFLOAT3>& patrolPoints)
        {
            auto tree = std::make_unique<BehaviorTree>("PatrolBehavior");
            auto root = std::make_unique<SelectorNode>("Root");

            // Combat sub-tree: if enemy detected, engage
            auto combatSeq = std::make_unique<SequenceNode>("CombatSequence");
            combatSeq->AddChild(std::make_unique<ConditionNode>("HasTarget", [](const Blackboard& bb)
                                                                { return bb.Get<bool>("hasTarget", false); }));
            combatSeq->AddChild(std::make_unique<ActionNode>("EngageTarget", [](float dt, Blackboard& bb)
                                                             { return NodeStatus::Running; }));

            // Patrol sub-tree: walk between waypoints
            auto patrolSeq = std::make_unique<SequenceNode>("PatrolSequence");
            patrolSeq->AddChild(std::make_unique<ActionNode>("MoveToWaypoint", [](float dt, Blackboard& bb)
                                                             { return NodeStatus::Running; }));
            patrolSeq->AddChild(std::make_unique<WaitNode>(2.0f));

            root->AddChild(std::move(combatSeq));
            root->AddChild(std::move(patrolSeq));
            tree->SetRoot(std::move(root));

            // Store patrol points in blackboard
            for (size_t i = 0; i < patrolPoints.size(); ++i)
            {
                tree->GetBlackboard().Set("patrol_" + std::to_string(i), patrolPoints[i]);
            }
            tree->GetBlackboard().Set("patrolCount", static_cast<int>(patrolPoints.size()));

            return tree;
        }

        std::unique_ptr<BehaviorTree> CreateCombatBehavior(const AIAgentConfig& config)
        {
            auto tree = std::make_unique<BehaviorTree>("CombatBehavior");
            auto root = std::make_unique<SelectorNode>("Root");

            // Flee if low health
            auto fleeSeq = std::make_unique<SequenceNode>("FleeSequence");
            fleeSeq->AddChild(std::make_unique<ConditionNode>("LowHealth", [](const Blackboard& bb)
                                                              { return bb.Get<float>("healthPercent", 1.0f) < 0.2f; }));
            fleeSeq->AddChild(
                std::make_unique<ActionNode>("Flee", [](float dt, Blackboard& bb) { return NodeStatus::Running; }));

            // Take cover if under fire
            auto coverSeq = std::make_unique<SequenceNode>("CoverSequence");
            coverSeq->AddChild(std::make_unique<ConditionNode>("UnderFire", [](const Blackboard& bb)
                                                               { return bb.Get<bool>("underFire", false); }));
            coverSeq->AddChild(std::make_unique<ActionNode>("FindCover", [](float dt, Blackboard& bb)
                                                            { return NodeStatus::Running; }));

            // Attack if in range
            auto attackSeq = std::make_unique<SequenceNode>("AttackSequence");
            attackSeq->AddChild(std::make_unique<ConditionNode>(
                "InAttackRange", [&config](const Blackboard& bb)
                { return bb.Get<float>("targetDistance", 999.0f) < config.attackRange; }));
            attackSeq->AddChild(
                std::make_unique<ActionNode>("Attack", [](float dt, Blackboard& bb) { return NodeStatus::Running; }));

            // Chase target
            auto chaseAction = std::make_unique<ActionNode>("ChaseTarget", [](float dt, Blackboard& bb)
                                                            { return NodeStatus::Running; });

            root->AddChild(std::move(fleeSeq));
            root->AddChild(std::move(coverSeq));
            root->AddChild(std::move(attackSeq));
            root->AddChild(std::move(chaseAction));

            tree->SetRoot(std::move(root));
            return tree;
        }

        std::unique_ptr<BehaviorTree> CreateGuardBehavior(const XMFLOAT3& guardPosition, float guardRadius)
        {
            auto tree = std::make_unique<BehaviorTree>("GuardBehavior");
            auto root = std::make_unique<SelectorNode>("Root");

            // Combat if enemy in range
            auto combatSeq = std::make_unique<SequenceNode>("CombatSequence");
            combatSeq->AddChild(std::make_unique<ConditionNode>("EnemyDetected", [](const Blackboard& bb)
                                                                { return bb.Get<bool>("hasTarget", false); }));
            combatSeq->AddChild(
                std::make_unique<ActionNode>("Engage", [](float dt, Blackboard& bb) { return NodeStatus::Running; }));

            // Return to guard position
            auto returnSeq = std::make_unique<SequenceNode>("ReturnToPost");
            returnSeq->AddChild(
                std::make_unique<ConditionNode>("FarFromPost", [guardRadius](const Blackboard& bb)
                                                { return bb.Get<float>("distFromPost", 0.0f) > guardRadius; }));
            returnSeq->AddChild(std::make_unique<ActionNode>("MoveToPost", [](float dt, Blackboard& bb)
                                                             { return NodeStatus::Running; }));

            // Idle
            auto idle =
                std::make_unique<ActionNode>("Idle", [](float dt, Blackboard& bb) { return NodeStatus::Running; });

            root->AddChild(std::move(combatSeq));
            root->AddChild(std::move(returnSeq));
            root->AddChild(std::move(idle));

            tree->SetRoot(std::move(root));
            tree->GetBlackboard().Set("guardPosition", guardPosition);
            return tree;
        }

        std::unique_ptr<BehaviorTree> CreateFleeBehavior(float fleeDistance)
        {
            auto tree = std::make_unique<BehaviorTree>("FleeBehavior");

            auto root = std::make_unique<SequenceNode>("FleeSequence");
            root->AddChild(
                std::make_unique<ActionNode>("RunAway", [](float dt, Blackboard& bb) { return NodeStatus::Running; }));

            tree->SetRoot(std::move(root));
            tree->GetBlackboard().Set("fleeDistance", fleeDistance);
            return tree;
        }

    } // namespace FPSBehaviors

} // namespace Spark::AI
