/**
 * @file MovementSystem.cpp
 * @brief Implementation of the priority-based movement generator stack
 */

#include "MovementSystem.h"
#include "../ECS/Components.h"
#include "../ECS/Components/CoreComponents.h"
#include "../../Utils/MathUtils.h"
#include <cmath>
#include <algorithm>

namespace Spark::AI
{

    // =============================================================================
    // Helpers
    // =============================================================================

    static constexpr float kArrivalThreshold = 0.3f;
    static constexpr float kDefaultMoveSpeed = 5.0f;

    static float DistanceXZ(float x1, float z1, float x2, float z2)
    {
        float dx = x2 - x1;
        float dz = z2 - z1;
        return std::sqrt(dx * dx + dz * dz);
    }

    static float Distance3D(float x1, float y1, float z1, float x2, float y2, float z2)
    {
        float dx = x2 - x1;
        float dy = y2 - y1;
        float dz = z2 - z1;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    /// Move position toward target by step, return remaining distance
    static float MoveToward(float& posX, float& posY, float& posZ, float targetX, float targetY, float targetZ,
                            float step)
    {
        float dx = targetX - posX;
        float dy = targetY - posY;
        float dz = targetZ - posZ;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (dist < 0.001f)
        {
            return 0.0f;
        }

        if (step >= dist)
        {
            posX = targetX;
            posY = targetY;
            posZ = targetZ;
            return 0.0f;
        }

        float ratio = step / dist;
        posX += dx * ratio;
        posY += dy * ratio;
        posZ += dz * ratio;
        return dist - step;
    }

    // =============================================================================
    // IdleGenerator
    // =============================================================================

    void IdleGenerator::Initialize([[maybe_unused]] EntityID entity) {}

    bool IdleGenerator::Update([[maybe_unused]] EntityID entity, [[maybe_unused]] float deltaTime)
    {
        return true; // Idle never completes on its own
    }

    void IdleGenerator::Finalize([[maybe_unused]] EntityID entity) {}
    void IdleGenerator::Reset([[maybe_unused]] EntityID entity) {}

    // =============================================================================
    // RandomWanderGenerator
    // =============================================================================

    void RandomWanderGenerator::Initialize([[maybe_unused]] EntityID entity)
    {
        hasTarget = false;
        waitTimer = 0.0f;
    }

    bool RandomWanderGenerator::Update([[maybe_unused]] EntityID entity, float deltaTime)
    {
        if (!hasTarget)
        {
            waitTimer += deltaTime;
            if (waitTimer >= waitTime)
            {
                // Pick a random point within radius (simple pseudo-random using origin)
                float angle = static_cast<float>(std::fmod(waitTimer * 1234.5f, 6.2831853f));
                float r = radius * 0.5f + radius * 0.5f * static_cast<float>(std::fmod(waitTimer * 567.8f, 1.0f));
                targetX = originX + std::cos(angle) * r;
                targetY = originY;
                targetZ = originZ + std::sin(angle) * r;
                hasTarget = true;
            }
            return true;
        }

        // Movement is applied by MovementSystem::Update which reads generator target
        // and writes to the entity's Transform via ECS
        float dist = Distance3D(originX, originY, originZ, targetX, targetY, targetZ);
        if (dist < kArrivalThreshold)
        {
            hasTarget = false;
            waitTimer = 0.0f;
        }

        return true; // Wander never completes
    }

    void RandomWanderGenerator::Finalize([[maybe_unused]] EntityID entity) {}

    void RandomWanderGenerator::Reset([[maybe_unused]] EntityID entity)
    {
        hasTarget = false;
        waitTimer = 0.0f;
    }

    // =============================================================================
    // ChaseGenerator
    // =============================================================================

    void ChaseGenerator::Initialize([[maybe_unused]] EntityID entity) {}

    bool ChaseGenerator::Update([[maybe_unused]] EntityID entity, [[maybe_unused]] float deltaTime)
    {
        // Chase runs until explicitly stopped by the AI layer.
        // Actual position update happens in MovementSystem::Update via Transform.
        return true;
    }

    void ChaseGenerator::Finalize([[maybe_unused]] EntityID entity) {}
    void ChaseGenerator::Reset([[maybe_unused]] EntityID entity) {}

    // =============================================================================
    // FollowGenerator
    // =============================================================================

    void FollowGenerator::Initialize([[maybe_unused]] EntityID entity) {}

    bool FollowGenerator::Update([[maybe_unused]] EntityID entity, [[maybe_unused]] float deltaTime)
    {
        return true; // Follow runs until explicitly stopped
    }

    void FollowGenerator::Finalize([[maybe_unused]] EntityID entity) {}
    void FollowGenerator::Reset([[maybe_unused]] EntityID entity) {}

    // =============================================================================
    // FleeGenerator
    // =============================================================================

    void FleeGenerator::Initialize([[maybe_unused]] EntityID entity)
    {
        elapsed = 0.0f;
    }

    bool FleeGenerator::Update([[maybe_unused]] EntityID entity, float deltaTime)
    {
        elapsed += deltaTime;
        return elapsed < duration; // Completes when duration expires
    }

    void FleeGenerator::Finalize([[maybe_unused]] EntityID entity) {}

    void FleeGenerator::Reset([[maybe_unused]] EntityID entity)
    {
        elapsed = 0.0f;
    }

    // =============================================================================
    // PointGenerator
    // =============================================================================

    void PointGenerator::Initialize([[maybe_unused]] EntityID entity) {}

    bool PointGenerator::Update([[maybe_unused]] EntityID entity, [[maybe_unused]] float deltaTime)
    {
        // PointGenerator completes when the entity reaches the target.
        // The actual movement is applied by MovementSystem::Update reading Transform.
        return true;
    }

    void PointGenerator::Finalize([[maybe_unused]] EntityID entity) {}
    void PointGenerator::Reset([[maybe_unused]] EntityID entity) {}

    // =============================================================================
    // PatrolGenerator
    // =============================================================================

    void PatrolGenerator::Initialize([[maybe_unused]] EntityID entity)
    {
        currentIndex = 0;
    }

    bool PatrolGenerator::Update([[maybe_unused]] EntityID entity, [[maybe_unused]] float deltaTime)
    {
        if (waypoints.empty())
        {
            return false;
        }
        return true; // Patrol runs indefinitely when looping
    }

    void PatrolGenerator::Finalize([[maybe_unused]] EntityID entity) {}

    void PatrolGenerator::Reset([[maybe_unused]] EntityID entity)
    {
        currentIndex = 0;
    }

    // =============================================================================
    // HomeGenerator
    // =============================================================================

    void HomeGenerator::Initialize([[maybe_unused]] EntityID entity) {}

    bool HomeGenerator::Update([[maybe_unused]] EntityID entity, [[maybe_unused]] float deltaTime)
    {
        // Completes when entity reaches home position (checked by MovementSystem)
        return true;
    }

    void HomeGenerator::Finalize([[maybe_unused]] EntityID entity) {}
    void HomeGenerator::Reset([[maybe_unused]] EntityID entity) {}

    // =============================================================================
    // MotionController
    // =============================================================================

    void MotionController::SetGenerator(std::unique_ptr<IMovementGenerator> generator)
    {
        if (!generator)
        {
            return;
        }

        auto slot = static_cast<int>(generator->GetSlot());
        if (slot < 0 || slot >= kMovementSlotCount)
        {
            return;
        }

        // Finalize the outgoing generator if one exists
        if (m_slots[slot])
        {
            m_slots[slot]->Finalize(0);
        }

        EntityID entity = 0; // Entity ID provided at Update time; use 0 for init
        generator->Initialize(entity);
        m_slots[slot] = std::move(generator);
    }

    void MotionController::ClearSlot(MovementSlot slot)
    {
        auto idx = static_cast<int>(slot);
        if (idx >= 0 && idx < kMovementSlotCount && m_slots[idx])
        {
            m_slots[idx]->Finalize(0);
            m_slots[idx].reset();
        }
    }

    void MotionController::ClearAll()
    {
        for (int i = 0; i < kMovementSlotCount; ++i)
        {
            if (m_slots[i])
            {
                m_slots[i]->Finalize(0);
                m_slots[i].reset();
            }
        }
    }

    IMovementGenerator* MotionController::GetActiveGenerator() const
    {
        // Highest-priority slot with a generator wins
        for (int i = kMovementSlotCount - 1; i >= 0; --i)
        {
            if (m_slots[i])
            {
                return m_slots[i].get();
            }
        }
        return nullptr;
    }

    MovementType MotionController::GetActiveMovementType() const
    {
        auto* gen = GetActiveGenerator();
        return gen ? gen->GetType() : MovementType::Idle;
    }

    void MotionController::Update(EntityID entity, float deltaTime)
    {
        auto* gen = GetActiveGenerator();
        if (!gen)
        {
            return;
        }

        bool keepRunning = gen->Update(entity, deltaTime);
        if (!keepRunning)
        {
            auto slot = static_cast<int>(gen->GetSlot());
            m_slots[slot]->Finalize(entity);
            m_slots[slot].reset();
        }
    }

    bool MotionController::HasMovement(MovementSlot slot) const
    {
        auto idx = static_cast<int>(slot);
        return idx >= 0 && idx < kMovementSlotCount && m_slots[idx] != nullptr;
    }

    // =============================================================================
    // MovementSystem — Singleton
    // =============================================================================

    MovementSystem& MovementSystem::GetInstance()
    {
        static MovementSystem instance;
        return instance;
    }

    void MovementSystem::Initialize()
    {
        m_initialized = true;
    }

    void MovementSystem::Update(World& world, float deltaTime)
    {
        if (!m_initialized)
        {
            return;
        }

        // Tick every entity that has a MotionController registered
        for (auto& [entityId, controller] : m_controllers)
        {
            controller.Update(entityId, deltaTime);

            // Apply movement from the active generator to the entity's Transform
            auto* gen = controller.GetActiveGenerator();
            if (!gen)
                continue;

            auto entId = static_cast<entt::entity>(entityId);
            auto* transform = world.GetComponent<Transform>(entId);
            if (!transform)
                continue;

            float speed = kDefaultMoveSpeed;

            switch (gen->GetType())
            {
            case MovementType::RandomWander:
            {
                auto* wander = static_cast<RandomWanderGenerator*>(gen);
                if (!wander->hasTarget)
                    break;
                float step = speed * deltaTime;
                MoveToward(transform->position.x, transform->position.y, transform->position.z, wander->targetX,
                           wander->targetY, wander->targetZ, step);
                // Update origin tracking so distance check works
                wander->originX = transform->position.x;
                wander->originY = transform->position.y;
                wander->originZ = transform->position.z;
                // Face movement direction
                float dx = wander->targetX - transform->position.x;
                float dz = wander->targetZ - transform->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                break;
            }

            case MovementType::Chase:
            {
                auto* chase = static_cast<ChaseGenerator*>(gen);
                auto* targetTf = world.GetComponent<Transform>(static_cast<entt::entity>(chase->target));
                if (!targetTf)
                    break;
                float dist = Distance3D(transform->position.x, transform->position.y, transform->position.z,
                                        targetTf->position.x, targetTf->position.y, targetTf->position.z);
                if (dist > chase->minDist)
                {
                    float step = speed * deltaTime;
                    MoveToward(transform->position.x, transform->position.y, transform->position.z,
                               targetTf->position.x, targetTf->position.y, targetTf->position.z, step);
                }
                // Face the target
                float dx = targetTf->position.x - transform->position.x;
                float dz = targetTf->position.z - transform->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                break;
            }

            case MovementType::Follow:
            {
                auto* follow = static_cast<FollowGenerator*>(gen);
                auto* targetTf = world.GetComponent<Transform>(static_cast<entt::entity>(follow->target));
                if (!targetTf)
                    break;
                // Compute offset position behind target at desired distance
                float angleRad = follow->angle * MathUtils::DEG_TO_RAD;
                float followX = targetTf->position.x - std::sin(angleRad) * follow->distance;
                float followY = targetTf->position.y;
                float followZ = targetTf->position.z - std::cos(angleRad) * follow->distance;
                float step = speed * deltaTime;
                MoveToward(transform->position.x, transform->position.y, transform->position.z, followX, followY,
                           followZ, step);
                // Face the target
                float dx = targetTf->position.x - transform->position.x;
                float dz = targetTf->position.z - transform->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                break;
            }

            case MovementType::Flee:
            {
                auto* flee = static_cast<FleeGenerator*>(gen);
                auto* threatTf = world.GetComponent<Transform>(static_cast<entt::entity>(flee->threatEntity));
                if (!threatTf)
                    break;
                // Move directly away from threat
                float dx = transform->position.x - threatTf->position.x;
                float dy = transform->position.y - threatTf->position.y;
                float dz = transform->position.z - threatTf->position.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist < flee->fleeDistance && dist > 0.001f)
                {
                    float step = speed * deltaTime;
                    float ratio = step / dist;
                    transform->position.x += dx * ratio;
                    transform->position.y += dy * ratio;
                    transform->position.z += dz * ratio;
                }
                // Face away from threat
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                break;
            }

            case MovementType::Point:
            {
                auto* point = static_cast<PointGenerator*>(gen);
                float step = point->speed * deltaTime;
                float remaining = MoveToward(transform->position.x, transform->position.y, transform->position.z,
                                             point->targetX, point->targetY, point->targetZ, step);
                // Face movement direction
                float dx = point->targetX - transform->position.x;
                float dz = point->targetZ - transform->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                (void)remaining;
                break;
            }

            case MovementType::Patrol:
            {
                auto* patrol = static_cast<PatrolGenerator*>(gen);
                if (patrol->waypoints.empty())
                    break;
                int idx = patrol->currentIndex % static_cast<int>(patrol->waypoints.size());
                const auto& wp = patrol->waypoints[idx];
                float step = patrol->speed * deltaTime;
                float remaining = MoveToward(transform->position.x, transform->position.y, transform->position.z, wp[0],
                                             wp[1], wp[2], step);
                // Face waypoint
                float dx = wp[0] - transform->position.x;
                float dz = wp[2] - transform->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                // Advance to next waypoint if arrived
                if (remaining < kArrivalThreshold)
                {
                    patrol->currentIndex++;
                    if (patrol->currentIndex >= static_cast<int>(patrol->waypoints.size()))
                    {
                        if (patrol->loop)
                            patrol->currentIndex = 0;
                        else
                            patrol->currentIndex = static_cast<int>(patrol->waypoints.size()) - 1;
                    }
                }
                break;
            }

            case MovementType::Home:
            {
                auto* home = static_cast<HomeGenerator*>(gen);
                float step = home->speed * deltaTime;
                MoveToward(transform->position.x, transform->position.y, transform->position.z, home->homeX,
                           home->homeY, home->homeZ, step);
                float dx = home->homeX - transform->position.x;
                float dz = home->homeZ - transform->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                    transform->rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                break;
            }

            case MovementType::Idle:
            case MovementType::Charge:
            case MovementType::Spline:
            case MovementType::Waypoint:
            case MovementType::Count:
                break;
            }
        }
    }

    void MovementSystem::Shutdown()
    {
        for (auto& [entityId, controller] : m_controllers)
        {
            controller.ClearAll();
        }
        m_controllers.clear();
        m_initialized = false;
    }

    // =============================================================================
    // Movement Commands
    // =============================================================================

    void MovementSystem::MoveIdle(EntityID entity)
    {
        auto& ctrl = GetOrCreateController(entity);
        ctrl.SetGenerator(std::make_unique<IdleGenerator>());
    }

    void MovementSystem::MoveChase(EntityID entity, EntityID target, float minDist, float maxDist)
    {
        auto gen = std::make_unique<ChaseGenerator>();
        gen->target = target;
        gen->minDist = minDist;
        gen->maxDist = maxDist;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::MoveFollow(EntityID entity, EntityID target, float distance, float angle)
    {
        auto gen = std::make_unique<FollowGenerator>();
        gen->target = target;
        gen->distance = distance;
        gen->angle = angle;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::MoveFlee(EntityID entity, EntityID threat, float distance, float duration)
    {
        auto gen = std::make_unique<FleeGenerator>();
        gen->threatEntity = threat;
        gen->fleeDistance = distance;
        gen->duration = duration;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::MovePoint(EntityID entity, float x, float y, float z, float speed)
    {
        auto gen = std::make_unique<PointGenerator>();
        gen->targetX = x;
        gen->targetY = y;
        gen->targetZ = z;
        gen->speed = speed;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::MovePatrol(EntityID entity, const std::vector<std::array<float, 3>>& waypoints, bool loop)
    {
        auto gen = std::make_unique<PatrolGenerator>();
        gen->waypoints = waypoints;
        gen->loop = loop;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::MoveHome(EntityID entity, float x, float y, float z)
    {
        auto gen = std::make_unique<HomeGenerator>();
        gen->homeX = x;
        gen->homeY = y;
        gen->homeZ = z;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::MoveRandomWander(EntityID entity, float radius, float waitTime)
    {
        auto gen = std::make_unique<RandomWanderGenerator>();
        gen->radius = radius;
        gen->waitTime = waitTime;
        GetOrCreateController(entity).SetGenerator(std::move(gen));
    }

    void MovementSystem::StopMovement(EntityID entity, MovementSlot slot)
    {
        auto it = m_controllers.find(entity);
        if (it != m_controllers.end())
        {
            it->second.ClearSlot(slot);
        }
    }

    void MovementSystem::StopAllMovement(EntityID entity)
    {
        auto it = m_controllers.find(entity);
        if (it != m_controllers.end())
        {
            it->second.ClearAll();
        }
    }

    MovementType MovementSystem::GetMovementType(EntityID entity) const
    {
        auto it = m_controllers.find(entity);
        if (it == m_controllers.end())
        {
            return MovementType::Idle;
        }
        return it->second.GetActiveMovementType();
    }

    MotionController& MovementSystem::GetOrCreateController(EntityID entity)
    {
        return m_controllers[entity];
    }

} // namespace Spark::AI
