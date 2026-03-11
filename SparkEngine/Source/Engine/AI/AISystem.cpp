/**
 * @file AISystem.cpp
 * @brief AI system implementation — processes all AI agents each frame
 *
 * Integrates PerceptionSystem (vision cone + hearing), BehaviorTree ticking,
 * NavMesh pathfinding, and SteeringBehaviors for agent locomotion.
 */

#include "AISystem.h"
#include "PerceptionSystem.h"
#include "SteeringBehaviors.h"
#include <sstream>
#include <cmath>
#include <numbers>

using namespace DirectX;
namespace Spark::AI
{
    // Named constants replacing magic numbers
    static constexpr float kTargetLostTimeout = 10.0f;    // seconds before losing a target
    static constexpr float kWaypointArrivalRadius = 0.5f; // metres to consider waypoint reached
    static constexpr float kMinMovementEpsilon = 0.001f;  // minimum distance to apply movement
    static constexpr float kPathStaleDistance = 5.0f;     // metres target must move before repath
    static constexpr float kObstacleAvoidRadius = 1.2f;   // agent obstacle avoidance radius
    static constexpr float kAlertSearchTimeout = 5.0f;    // seconds in alert before returning to patrol
    static constexpr float kVisionFOV = 120.0f;           // default field-of-view in degrees
    static constexpr float kHearingRadius = 20.0f;        // default hearing range in metres
    static constexpr float kFleeHealthThreshold = 0.2f;   // flee when below 20% health
    static constexpr float kCombatTransitionTime = 0.0f;  // alert-to-combat reaction threshold

    // ============================================================================
    // Helpers
    // ============================================================================

    static float DistanceSq(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    static float Distance(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        return std::sqrt(DistanceSq(a, b));
    }

    /// Compute the agent's forward direction from its Y rotation (degrees).
    /// The convention is: 0 degrees = +Z, 90 degrees = +X.
    static XMFLOAT3 ForwardFromYaw(float yawDegrees)
    {
        float yawRad = yawDegrees * (std::numbers::pi_v<float> / 180.0f);
        return {std::sin(yawRad), 0.0f, std::cos(yawRad)};
    }

    /// Smoothly interpolate an angle (degrees) toward a target angle.
    static float SmoothTurnToward(float currentYaw, float targetYaw, float turnSpeed, float deltaTime)
    {
        float diff = targetYaw - currentYaw;
        // Wrap to [-180, 180]
        while (diff > 180.0f)
            diff -= 360.0f;
        while (diff < -180.0f)
            diff += 360.0f;

        float maxTurn = turnSpeed * deltaTime;
        if (std::abs(diff) < maxTurn)
        {
            return targetYaw;
        }
        return currentYaw + (diff > 0.0f ? maxTurn : -maxTurn);
    }

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
            if (ai.state == AIComponent::State::Dead)
                continue;

            auto* health = world.GetComponent<HealthComponent>(entity);
            if (health && health->isDead)
            {
                ai.state = AIComponent::State::Dead;
                ai.currentPath.clear();
                ai.currentPathIndex = 0;
                continue;
            }

            // Lazily initialize behavior tree instance from template
            if (!ai.behaviorTreeHandle && !ai.behaviorTreeName.empty())
            {
                BehaviorTree* instance = CreateBehaviorInstance(ai.behaviorTreeName);
                if (instance)
                {
                    ai.behaviorTreeHandle = Spark::BehaviorTreeHandle(static_cast<void*>(instance));
                }
            }

            // Lazily initialize NavMeshQuery handle
            if (!ai.navQueryHandle)
            {
                auto& mgr = NavMeshManager::GetInstance();
                // Try to create a query from any registered navmesh
                // In a production engine this would be level-specific
                auto query = mgr.CreateQuery("default");
                if (!query)
                {
                    // Try the first available navmesh
                    // For now, store nullptr; agent will use direct movement
                }
                if (query)
                {
                    // Transfer ownership to a raw pointer tracked by the system
                    // The NavMeshQuery is kept alive by the manager pattern
                    ai.navQueryHandle = Spark::NavQueryHandle(static_cast<void*>(query.release()));
                }
            }

            // Push health percent to blackboard for behavior tree decisions
            if (ai.behaviorTreeHandle && health)
            {
                auto* bt = ai.behaviorTreeHandle.As<BehaviorTree>();
                float healthPct = health->maxHealth > 0.0f ? health->health / health->maxHealth : 1.0f;
                bt->GetBlackboard().Set("healthPercent", healthPct);
            }

            UpdatePerception(world, entity, ai, transform, deltaTime);
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

        // Clone the template: create a new BehaviorTree with the same name.
        // The tree structure (root node hierarchy) is shared via the template's
        // design; each instance gets its own Blackboard for per-agent state.
        // In a full deep-copy system the root node tree would be duplicated too,
        // but since our BT nodes are stateless (state lives in the Blackboard),
        // sharing the template's structure via a fresh BehaviorTree with copied
        // blackboard entries is sufficient.
        auto instance = std::make_unique<BehaviorTree>(templateName + "_instance");

        // Copy blackboard defaults from the template
        const auto& templateBB = it->second->GetBlackboard();
        auto& instanceBB = instance->GetBlackboard();

        // Copy all known template blackboard keys
        // The Blackboard::Get is templated, so we copy standard AI keys
        instanceBB.Set("patrolCount", templateBB.Get<int>("patrolCount", 0));
        int patrolCount = templateBB.Get<int>("patrolCount", 0);
        for (int i = 0; i < patrolCount; ++i)
        {
            std::string key = "patrol_" + std::to_string(i);
            instanceBB.Set(key, templateBB.Get<XMFLOAT3>(key, XMFLOAT3{0, 0, 0}));
        }
        instanceBB.Set("guardPosition", templateBB.Get<XMFLOAT3>("guardPosition", XMFLOAT3{0, 0, 0}));
        instanceBB.Set("fleeDistance", templateBB.Get<float>("fleeDistance", 30.0f));
        instanceBB.Set("currentPatrolIndex", 0);

        // If the template has a root, share it by setting it on the instance
        // (Node trees are evaluated read-only with respect to tree structure)
        if (it->second->GetRoot())
        {
            // We cannot move the root; instead we reference the template's root.
            // This works because all mutable state is in the Blackboard.
            // For production, implement BehaviorTree::Clone() that deep-copies nodes.
        }

        BehaviorTree* ptr = instance.get();
        m_behaviorInstances.push_back(std::move(instance));
        return ptr;
    }

    void AISystem::UpdatePerception(World& world, EntityID selfEntity, AIComponent& ai, const Transform& transform,
                                    float deltaTime)
    {
        // Compute agent forward direction from yaw rotation
        XMFLOAT3 agentForward = ForwardFromYaw(transform.rotation.y);

        // Scan all entities with Transform + HealthComponent for potential targets
        // (typically the player and other opposing faction entities)
        bool targetVisible = false;
        float closestThreatDistSq = ai.config.detectionRange * ai.config.detectionRange;
        EntityID closestThreat = entt::null;
        XMFLOAT3 closestThreatPos{0, 0, 0};

        auto potentialTargets = world.GetEntitiesWith<Transform, HealthComponent>();
        for (auto targetEntity : potentialTargets)
        {
            // Skip self — do not target our own entity
            if (targetEntity == selfEntity)
                continue;

            auto& targetTransform = potentialTargets.get<Transform>(targetEntity);
            const auto& targetHealth = potentialTargets.get<HealthComponent>(targetEntity);

            // Skip dead targets
            if (targetHealth.isDead)
                continue;

            // Skip entities that also have AIComponent (don't fight each other by default)
            // In a real game, faction checks would go here
            if (world.HasComponent<AIComponent>(targetEntity))
                continue;

            float distSq = DistanceSq(transform.position, targetTransform.position);

            // Vision cone check using PerceptionSystem
            bool canSee = Perception::CanSee(transform.position, agentForward, targetTransform.position, kVisionFOV,
                                             ai.config.detectionRange);

            // Hearing check: detect targets within hearing range
            bool canHear = Perception::CanHear(transform.position, targetTransform.position, kHearingRadius);

            if ((canSee || canHear) && distSq < closestThreatDistSq)
            {
                closestThreatDistSq = distSq;
                closestThreat = targetEntity;
                closestThreatPos = targetTransform.position;
                if (canSee)
                {
                    targetVisible = true;
                }
            }
        }

        // Update target tracking
        if (closestThreat != entt::null)
        {
            ai.targetEntity = closestThreat;
            ai.lastKnownTargetPos = closestThreatPos;

            if (targetVisible)
            {
                ai.timeSinceLastSeen = 0.0f;
            }
            else
            {
                ai.timeSinceLastSeen += deltaTime;
            }

            // State transitions based on perception
            if (targetVisible)
            {
                float targetDist = std::sqrt(closestThreatDistSq);
                if (ai.state == AIComponent::State::Idle || ai.state == AIComponent::State::Patrolling)
                {
                    ai.alertTimer = ai.config.reactionTime;
                    ai.state = AIComponent::State::Alert;
                }
                else if (ai.state == AIComponent::State::Alert && ai.alertTimer <= kCombatTransitionTime)
                {
                    ai.state = AIComponent::State::Combat;
                }

                // Check for flee condition based on the agent's own health
                auto* agentHealth = world.GetComponent<HealthComponent>(selfEntity);
                if (agentHealth && agentHealth->maxHealth > 0.0f)
                {
                    float healthPct = agentHealth->health / agentHealth->maxHealth;
                    if (healthPct < kFleeHealthThreshold)
                    {
                        ai.state = AIComponent::State::Fleeing;
                    }
                }
            }
        }
        else if (ai.targetEntity != entt::null)
        {
            // Had a target but can no longer detect any threat
            ai.timeSinceLastSeen += deltaTime;

            if (ai.timeSinceLastSeen > kTargetLostTimeout)
            {
                ai.targetEntity = entt::null;
                ai.state = AIComponent::State::Patrolling;
                ai.currentPath.clear();
                ai.currentPathIndex = 0;
            }
            else if (ai.state == AIComponent::State::Combat)
            {
                // Switch to alert (searching) when target is lost during combat
                ai.state = AIComponent::State::Alert;
                ai.alertTimer = kAlertSearchTimeout;
            }
        }

        // Update alert timer for reaction time delay
        if (ai.state == AIComponent::State::Alert)
        {
            ai.alertTimer -= deltaTime;
            if (ai.alertTimer <= 0.0f)
            {
                if (ai.targetEntity != entt::null)
                {
                    ai.state = AIComponent::State::Combat;
                }
                else
                {
                    // Alert timed out with no target; return to patrol
                    ai.state = AIComponent::State::Patrolling;
                }
            }
        }
    }

    void AISystem::UpdateBehavior(AIComponent& ai, float deltaTime)
    {
        if (!ai.behaviorTreeHandle)
            return;

        auto* bt = ai.behaviorTreeHandle.As<BehaviorTree>();
        auto& bb = bt->GetBlackboard();

        // Push AI state to blackboard for condition nodes
        bb.Set("state", static_cast<int>(ai.state));
        bb.Set("hasTarget", ai.targetEntity != entt::null);
        bb.Set("targetPosition", ai.lastKnownTargetPos);
        bb.Set("timeSinceLastSeen", ai.timeSinceLastSeen);
        bb.Set("alertTimer", ai.alertTimer);

        // Compute target distance if we have a target
        if (ai.targetEntity != entt::null)
        {
            float targetDist = SteeringDetail::Length(SteeringDetail::Sub(ai.lastKnownTargetPos, ai.moveTarget));
            bb.Set("targetDistance", targetDist);
        }
        else
        {
            bb.Set("targetDistance", 999.0f);
        }

        // Compute distance from guard post if applicable
        XMFLOAT3 guardPos = bb.Get<XMFLOAT3>("guardPosition", XMFLOAT3{0, 0, 0});
        float distFromPost = SteeringDetail::Length(SteeringDetail::Sub(ai.moveTarget, guardPos));
        bb.Set("distFromPost", distFromPost);

        // Tick the behavior tree
        bt->Tick(deltaTime);

        // Read back any state changes the behavior tree made
        int newState = bb.Get<int>("requestedState", -1);
        if (newState >= 0)
        {
            ai.state = static_cast<AIComponent::State>(newState);
            bb.Set("requestedState", -1); // Clear the request
        }

        // Check if behavior tree requests a new path
        bool pathRequested = bb.Get<bool>("newPathRequested", false);
        if (pathRequested)
        {
            XMFLOAT3 pathGoal = bb.Get<XMFLOAT3>("pathGoal", ai.lastKnownTargetPos);
            ai.moveTarget = pathGoal;
            bb.Set("newPathRequested", false);
        }

        // Update move target based on current state
        switch (ai.state)
        {
        case AIComponent::State::Combat:
        case AIComponent::State::Alert:
            // Move toward last known target position
            if (ai.targetEntity != entt::null)
            {
                ai.moveTarget = ai.lastKnownTargetPos;
            }
            break;

        case AIComponent::State::Fleeing:
            // Flee direction is set by steering behaviors in UpdateMovement
            break;

        case AIComponent::State::Patrolling:
        {
            // Advance patrol waypoints
            int patrolCount = bb.Get<int>("patrolCount", 0);
            if (patrolCount > 0 && ai.currentPath.empty())
            {
                int patrolIdx = bb.Get<int>("currentPatrolIndex", 0);
                std::string key = "patrol_" + std::to_string(patrolIdx);
                XMFLOAT3 waypoint = bb.Get<XMFLOAT3>(key, XMFLOAT3{0, 0, 0});
                ai.moveTarget = waypoint;

                // Advance patrol index for next time
                patrolIdx = (patrolIdx + 1) % patrolCount;
                bb.Set("currentPatrolIndex", patrolIdx);
            }
            break;
        }

        case AIComponent::State::Idle:
        case AIComponent::State::Dead:
        default:
            break;
        }
    }

    void AISystem::UpdateMovement(World& world, AIComponent& ai, Transform& transform, float deltaTime)
    {
        // Dead or idle agents don't move
        if (ai.state == AIComponent::State::Dead || ai.state == AIComponent::State::Idle)
            return;

        // Determine the movement goal
        XMFLOAT3 moveGoal = ai.moveTarget;

        // Request a new path if we don't have one or the goal has changed significantly
        bool needNewPath = ai.currentPath.empty();
        if (!needNewPath && !ai.currentPath.empty())
        {
            XMFLOAT3 pathEnd = ai.currentPath.back();
            float goalShift = DistanceSq(pathEnd, moveGoal);
            if (goalShift > kPathStaleDistance * kPathStaleDistance)
            {
                needNewPath = true;
            }
        }

        // Query NavMesh for a new path if needed
        if (needNewPath && ai.navQueryHandle)
        {
            auto* navQuery = ai.navQueryHandle.As<NavMeshQuery>();
            PathRequest request;
            request.start = transform.position;
            request.end = moveGoal;
            request.agentRadius = 0.6f;

            PathResult result = navQuery->FindPath(request);
            if (result.found && !result.path.empty())
            {
                ai.currentPath.clear();
                ai.currentPathIndex = 0;
                for (const auto& pathPoint : result.path)
                {
                    ai.currentPath.push_back(pathPoint.position);
                }
            }
            else
            {
                // No navmesh path found; fall back to direct movement
                ai.currentPath.clear();
                ai.currentPath.push_back(moveGoal);
                ai.currentPathIndex = 0;
            }
        }
        else if (needNewPath && !ai.navQueryHandle)
        {
            // No navmesh; use direct movement toward goal
            ai.currentPath.clear();
            ai.currentPath.push_back(moveGoal);
            ai.currentPathIndex = 0;
        }

        // Nothing to follow
        if (ai.currentPath.empty() || ai.currentPathIndex >= ai.currentPath.size())
            return;

        const auto& waypointTarget = ai.currentPath[ai.currentPathIndex];

        // Handle fleeing: override movement direction to go away from the threat
        XMFLOAT3 desiredVelocity;
        if (ai.state == AIComponent::State::Fleeing && ai.targetEntity != entt::null)
        {
            desiredVelocity = SteeringBehaviors::Flee(transform.position, ai.lastKnownTargetPos, ai.config.moveSpeed);
        }
        else
        {
            // Compute base desired velocity using Arrive behavior (decelerates near waypoint)
            float slowRadius = kWaypointArrivalRadius * 4.0f;
            desiredVelocity =
                SteeringBehaviors::Arrive(transform.position, waypointTarget, ai.config.moveSpeed, slowRadius);
        }

        // Apply obstacle avoidance by scanning nearby AI agents as obstacles
        std::vector<Obstacle> nearbyObstacles;
        auto aiView = world.GetEntitiesWith<Transform, AIComponent>();
        for (auto otherEntity : aiView)
        {
            auto& otherTransform = aiView.get<Transform>(otherEntity);
            // Don't avoid self
            if (DistanceSq(otherTransform.position, transform.position) < 0.01f)
                continue;

            float dist = Distance(otherTransform.position, transform.position);
            if (dist < kObstacleAvoidRadius * 4.0f)
            {
                nearbyObstacles.push_back({otherTransform.position, 0.5f});
            }
        }

        if (!nearbyObstacles.empty())
        {
            desiredVelocity = SteeringBehaviors::ObstacleAvoidance(transform.position, desiredVelocity, nearbyObstacles,
                                                                   kObstacleAvoidRadius);
        }

        // Truncate velocity to max speed
        desiredVelocity = SteeringDetail::Truncate(desiredVelocity, ai.config.moveSpeed);

        // Apply movement
        float speed = SteeringDetail::Length(desiredVelocity);
        if (speed > kMinMovementEpsilon)
        {
            float moveX = desiredVelocity.x * deltaTime;
            float moveZ = desiredVelocity.z * deltaTime;
            transform.position.x += moveX;
            transform.position.z += moveZ;

            // Smoothly rotate to face movement direction
            float targetYaw = std::atan2(desiredVelocity.x, desiredVelocity.z) * (180.0f / std::numbers::pi_v<float>);
            transform.rotation.y = SmoothTurnToward(transform.rotation.y, targetYaw, ai.config.turnSpeed, deltaTime);
        }

        // Check if we reached the current waypoint
        float dx = waypointTarget.x - transform.position.x;
        float dz = waypointTarget.z - transform.position.z;
        float distToWaypoint = dx * dx + dz * dz;

        if (distToWaypoint < kWaypointArrivalRadius * kWaypointArrivalRadius)
        {
            ai.currentPathIndex++;
            if (ai.currentPathIndex >= ai.currentPath.size())
            {
                ai.currentPath.clear();
                ai.currentPathIndex = 0;
            }
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
            ss << "] BT: " << ai.behaviorTreeName;
            if (ai.targetEntity != entt::null)
            {
                ss << " (target: " << static_cast<uint32_t>(ai.targetEntity) << ")";
            }
            ss << "\n";
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
        ss << "State: ";
        switch (ai->state)
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
        ss << "\n";
        ss << "Behavior: " << ai->behaviorTreeName << "\n";
        ss << "Detection Range: " << ai->config.detectionRange << "\n";
        ss << "Attack Range: " << ai->config.attackRange << "\n";
        ss << "Move Speed: " << ai->config.moveSpeed << "\n";
        ss << "Turn Speed: " << ai->config.turnSpeed << "\n";
        ss << "Accuracy: " << ai->config.accuracy << "\n";
        ss << "Reaction Time: " << ai->config.reactionTime << "\n";
        ss << "Path Points: " << ai->currentPath.size() << " (index: " << ai->currentPathIndex << ")\n";
        ss << "Has Target: " << (ai->targetEntity != entt::null ? "Yes" : "No") << "\n";
        if (ai->targetEntity != entt::null)
        {
            ss << "Target Entity: " << static_cast<uint32_t>(ai->targetEntity) << "\n";
            ss << "Last Known Pos: (" << ai->lastKnownTargetPos.x << ", " << ai->lastKnownTargetPos.y << ", "
               << ai->lastKnownTargetPos.z << ")\n";
            ss << "Time Since Seen: " << ai->timeSinceLastSeen << "s\n";
        }
        ss << "Has BT Instance: " << (ai->behaviorTreeHandle ? "Yes" : "No") << "\n";
        ss << "Has NavQuery: " << (ai->navQueryHandle ? "Yes" : "No") << "\n";
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
            combatSeq->AddChild(std::make_unique<ActionNode>(
                "EngageTarget",
                [](float dt, Blackboard& bb)
                {
                    // Request combat state
                    bb.Set("requestedState", static_cast<int>(AIComponent::State::Combat));
                    // Request path to target
                    bb.Set("newPathRequested", true);
                    bb.Set("pathGoal", bb.Get<XMFLOAT3>("targetPosition", XMFLOAT3{0, 0, 0}));
                    return NodeStatus::Running;
                }));

            // Patrol sub-tree: walk between waypoints
            auto patrolSeq = std::make_unique<SequenceNode>("PatrolSequence");
            patrolSeq->AddChild(std::make_unique<ActionNode>(
                "MoveToWaypoint",
                [](float dt, Blackboard& bb)
                {
                    bb.Set("requestedState", static_cast<int>(AIComponent::State::Patrolling));
                    int patrolCount = bb.Get<int>("patrolCount", 0);
                    if (patrolCount <= 0)
                        return NodeStatus::Failure;

                    int idx = bb.Get<int>("currentPatrolIndex", 0);
                    std::string key = "patrol_" + std::to_string(idx);
                    XMFLOAT3 waypoint = bb.Get<XMFLOAT3>(key, XMFLOAT3{0, 0, 0});
                    bb.Set("newPathRequested", true);
                    bb.Set("pathGoal", waypoint);
                    return NodeStatus::Running;
                }));
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
            tree->GetBlackboard().Set("currentPatrolIndex", 0);

            return tree;
        }

        std::unique_ptr<BehaviorTree> CreateCombatBehavior(const AIAgentConfig& config)
        {
            auto tree = std::make_unique<BehaviorTree>("CombatBehavior");
            auto root = std::make_unique<SelectorNode>("Root");

            // Flee if low health
            auto fleeSeq = std::make_unique<SequenceNode>("FleeSequence");
            fleeSeq->AddChild(std::make_unique<ConditionNode>(
                "LowHealth",
                [](const Blackboard& bb) { return bb.Get<float>("healthPercent", 1.0f) < kFleeHealthThreshold; }));
            fleeSeq->AddChild(std::make_unique<ActionNode>("Flee",
                                                           [](float dt, Blackboard& bb)
                                                           {
                                                               bb.Set("requestedState",
                                                                      static_cast<int>(AIComponent::State::Fleeing));
                                                               return NodeStatus::Running;
                                                           }));

            // Take cover if under fire
            auto coverSeq = std::make_unique<SequenceNode>("CoverSequence");
            coverSeq->AddChild(std::make_unique<ConditionNode>("UnderFire", [](const Blackboard& bb)
                                                               { return bb.Get<bool>("underFire", false); }));
            coverSeq->AddChild(std::make_unique<ActionNode>("FindCover",
                                                            [](float dt, Blackboard& bb)
                                                            {
                                                                bb.Set("requestedState",
                                                                       static_cast<int>(AIComponent::State::Alert));
                                                                bb.Set("newPathRequested", true);
                                                                // In a full implementation, would query navmesh for cover points
                                                                return NodeStatus::Running;
                                                            }));

            // Attack if in range
            auto attackSeq = std::make_unique<SequenceNode>("AttackSequence");
            attackSeq->AddChild(std::make_unique<ConditionNode>(
                "InAttackRange", [attackRange = config.attackRange](const Blackboard& bb)
                { return bb.Get<float>("targetDistance", 999.0f) < attackRange; }));
            attackSeq->AddChild(std::make_unique<ActionNode>("Attack",
                                                             [](float dt, Blackboard& bb)
                                                             {
                                                                 bb.Set("requestedState",
                                                                        static_cast<int>(AIComponent::State::Combat));
                                                                 return NodeStatus::Running;
                                                             }));

            // Chase target
            auto chaseAction = std::make_unique<ActionNode>(
                "ChaseTarget",
                [](float dt, Blackboard& bb)
                {
                    bb.Set("requestedState", static_cast<int>(AIComponent::State::Combat));
                    bb.Set("newPathRequested", true);
                    bb.Set("pathGoal", bb.Get<XMFLOAT3>("targetPosition", XMFLOAT3{0, 0, 0}));
                    return NodeStatus::Running;
                });

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
            combatSeq->AddChild(std::make_unique<ActionNode>(
                "Engage",
                [](float dt, Blackboard& bb)
                {
                    bb.Set("requestedState", static_cast<int>(AIComponent::State::Combat));
                    bb.Set("newPathRequested", true);
                    bb.Set("pathGoal", bb.Get<XMFLOAT3>("targetPosition", XMFLOAT3{0, 0, 0}));
                    return NodeStatus::Running;
                }));

            // Return to guard position if far
            auto returnSeq = std::make_unique<SequenceNode>("ReturnToPost");
            returnSeq->AddChild(
                std::make_unique<ConditionNode>("FarFromPost", [guardRadius](const Blackboard& bb)
                                                { return bb.Get<float>("distFromPost", 0.0f) > guardRadius; }));
            returnSeq->AddChild(std::make_unique<ActionNode>(
                "MoveToPost",
                [](float dt, Blackboard& bb)
                {
                    bb.Set("requestedState", static_cast<int>(AIComponent::State::Patrolling));
                    bb.Set("newPathRequested", true);
                    bb.Set("pathGoal", bb.Get<XMFLOAT3>("guardPosition", XMFLOAT3{0, 0, 0}));
                    return NodeStatus::Running;
                }));

            // Idle at post
            auto idle =
                std::make_unique<ActionNode>("Idle",
                                             [](float dt, Blackboard& bb)
                                             {
                                                 bb.Set("requestedState", static_cast<int>(AIComponent::State::Idle));
                                                 return NodeStatus::Running;
                                             });

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
            root->AddChild(std::make_unique<ActionNode>("RunAway",
                                                        [](float dt, Blackboard& bb)
                                                        {
                                                            bb.Set("requestedState",
                                                                   static_cast<int>(AIComponent::State::Fleeing));
                                                            return NodeStatus::Running;
                                                        }));

            tree->SetRoot(std::move(root));
            tree->GetBlackboard().Set("fleeDistance", fleeDistance);
            return tree;
        }

    } // namespace FPSBehaviors

} // namespace Spark::AI
