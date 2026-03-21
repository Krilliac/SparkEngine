/**
 * @file ECSystems.cpp
 * @brief ECS system implementations — the runtime logic that processes components each frame.
 *
 * Systems run in a fixed order defined by SystemManager registration in SparkEngine.cpp:
 *   Physics -> Animation -> AI -> Audio -> Lifecycle -> Render
 *
 * This order matters:
 * - Physics runs first so transforms reflect the latest simulation state.
 * - Animation runs after physics so procedural animation can override physics poses.
 * - AI runs after animation so behavior trees see up-to-date world state.
 * - Audio runs after AI/physics so 3D positions and Doppler are accurate.
 * - Lifecycle runs near the end so death processing sees the final frame state.
 * - Render runs last to submit draw commands with fully resolved transforms.
 */
#include "../../../Core/Platform.h"
#include "ECSystems.h"
#include "Graphics/GraphicsEngine.h"
#include "Physics/PhysicsSystem.h"
#include "Audio/AudioEngine.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/ECS/Components/FPSComponents.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/PhysicsComponents.h"
#include "Utils/Cooldown.h"
#include "Utils/MathUtils.h"
#include "Utils/Validate.h"
#include <sstream>
#include <cmath>

using namespace DirectX;
namespace Spark::ECS
{

    // ============================================================================
    // RenderSystem
    // ============================================================================

    void RenderSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        m_renderedCount = 0;
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_graphics);

        const auto& registry = world.GetRegistry();
        auto view = world.GetEntitiesWith<Transform, MeshRenderer>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& renderer = view.get<MeshRenderer>(entity);

            if (!renderer.visible)
                continue;

            // Check if entity is active
            auto* active = world.GetComponent<ActiveComponent>(entity);
            if (active && !active->active)
                continue;

            // Compute the world matrix, walking the parent hierarchy if present.
            // Cache it on the component so other systems (physics debug draw, culling)
            // can read the matrix without recomputing it.
            XMMATRIX worldMtx = transform.GetWorldMatrix(registry);
            XMStoreFloat4x4(&renderer.cachedWorldMatrix, worldMtx);
            renderer.worldMatrixDirty = false; // Signal to other systems that the cached matrix is current

            // Submit draw call to GraphicsEngine
            m_graphics->SubmitMeshForRendering(renderer.meshPath, renderer.materialPath, worldMtx,
                                               renderer.castShadows);

            m_renderedCount++;
        }
    }

    // ============================================================================
    // PhysicsUpdateSystem
    // ============================================================================

    void PhysicsUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Physics, m_physics);
        SPARK_WARN_IF(Spark::LogCategory::Physics, deltaTime > 0.1f,
                      "Large deltaTime in PhysicsUpdate — possible frame spike");

        auto view = world.GetEntitiesWith<Transform, RigidBodyComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& rb = view.get<RigidBodyComponent>(entity);

            // Auto-create physics body if one doesn't exist yet
            if (!rb.physicsBodyHandle)
            {
                PhysicsBodyDesc desc;
                desc.position = transform.position;
                desc.rotation = transform.rotation;
                desc.mass = (rb.type == RigidBodyComponent::Type::Dynamic) ? rb.mass : 0.0f;
                desc.material.friction = rb.friction;
                desc.material.restitution = rb.restitution;
                desc.material.linearDamping = rb.linearDamping;
                desc.material.angularDamping = rb.angularDamping;
                desc.isTrigger = rb.isTrigger;
                desc.entityId = static_cast<uint32_t>(entity);

                switch (rb.type)
                {
                case RigidBodyComponent::Type::Static:
                    desc.type = PhysicsBodyType::Static;
                    break;
                case RigidBodyComponent::Type::Kinematic:
                    desc.type = PhysicsBodyType::Kinematic;
                    break;
                case RigidBodyComponent::Type::Dynamic:
                default:
                    desc.type = PhysicsBodyType::Dynamic;
                    break;
                }

                // Use ColliderComponent shape if present, otherwise default box
                auto* collider = world.GetComponent<ColliderComponent>(entity);
                if (collider)
                {
                    switch (collider->shape)
                    {
                    case ColliderComponent::Shape::Sphere:
                        desc.shape.type = CollisionShapeType::Sphere;
                        desc.shape.radius = collider->radius;
                        break;
                    case ColliderComponent::Shape::Capsule:
                        desc.shape.type = CollisionShapeType::Capsule;
                        desc.shape.radius = collider->radius;
                        desc.shape.height = collider->height;
                        break;
                    case ColliderComponent::Shape::Box:
                    default:
                        desc.shape.type = CollisionShapeType::Box;
                        desc.shape.dimensions = collider->halfExtents;
                        break;
                    }
                }
                else
                {
                    desc.shape.type = CollisionShapeType::Box;
                    desc.shape.dimensions = {0.5f, 0.5f, 0.5f};
                }

                auto body = m_physics->CreateBody(desc);
                if (body)
                {
                    rb.physicsBodyHandle = Spark::PhysicsHandle(body.get());
                }
                continue;
            }

            auto* physBody = rb.physicsBodyHandle.As<PhysicsBody>();
            if (!physBody)
                continue;

            if (rb.type == RigidBodyComponent::Type::Dynamic)
            {
                // Dynamic bodies are authority-owned by the physics engine:
                // read the simulated position/rotation back into the ECS Transform.
                DirectX::XMFLOAT3 physPos = physBody->GetPosition();
                DirectX::XMFLOAT3 physRot = physBody->GetRotation();
                transform.position = physPos;
                transform.rotation = physRot;

                // Cache velocity so gameplay code (damage, knockback) can read it
                // without querying the physics engine directly.
                rb.linearVelocity = physBody->GetLinearVelocity();
                rb.angularVelocity = physBody->GetAngularVelocity();
            }
            else if (rb.type == RigidBodyComponent::Type::Kinematic)
            {
                // Kinematic bodies are authority-owned by game code:
                // push the ECS Transform into the physics engine so it can
                // resolve collisions with dynamic bodies.
                physBody->SetPosition(transform.position);
                physBody->SetRotation(transform.rotation);
            }
        }
    }

    // ============================================================================
    // AudioUpdateSystem
    // ============================================================================

    void AudioUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Audio, m_audio);

        auto view = world.GetEntitiesWith<Transform, AudioSourceComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& audio = view.get<AudioSourceComponent>(entity);

            if (!audio.is3D || !audio.audioSourceHandle)
                continue;

            auto* source = audio.audioSourceHandle.As<AudioSource>();
            if (!source)
                continue;

            // XAudio2 uses source velocity relative to the listener to compute
            // Doppler frequency shift. We derive velocity from the position delta
            // rather than reading physics velocity, because non-physics entities
            // (e.g. scripted movers) also need accurate Doppler.
            if (deltaTime > 0.0f)
            {
                source->Velocity.x = (transform.position.x - audio.previousPosition.x) / deltaTime;
                source->Velocity.y = (transform.position.y - audio.previousPosition.y) / deltaTime;
                source->Velocity.z = (transform.position.z - audio.previousPosition.z) / deltaTime;
            }

            // Update 3D position and store for next frame's velocity derivation
            source->Position = transform.position;
            audio.previousPosition = transform.position;
        }
    }

    // ============================================================================
    // LifecycleSystem
    // ============================================================================

    void LifecycleSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        // Two-phase death processing: collect first, then fire callbacks.
        // This avoids iterator invalidation if a death callback destroys
        // the entity or modifies HealthComponent on other entities.
        std::vector<entt::entity> deadEntities;

        auto healthView = world.GetEntitiesWith<HealthComponent>();
        for (auto entity : healthView)
        {
            auto& health = healthView.get<HealthComponent>(entity);
            if (health.isDead && !health.deathProcessed)
            {
                health.deathProcessed = true;
                deadEntities.push_back(entity);
            }
        }

        if (!deadEntities.empty())
        {
            SPARK_LOG_INFO(Spark::LogCategory::ECS, "LifecycleSystem: %zu entities died this frame",
                           deadEntities.size());
        }

        if (m_onDeath)
        {
            for (auto entity : deadEntities)
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::ECS, "LifecycleSystem: firing death callback for entity %u",
                                static_cast<uint32_t>(entity));
                m_onDeath(entity);
            }
        }
        else
        {
            SPARK_WARN_IF(Spark::LogCategory::ECS, !deadEntities.empty(),
                          "LifecycleSystem: entities died but no death callback is registered");
        }
    }

    // ============================================================================
    // AnimationUpdateSystem
    // ============================================================================

    void AnimationUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Animation);
        SPARK_WARN_IF(Spark::LogCategory::Animation, deltaTime < 0.0f, "Negative deltaTime in AnimationUpdate");
        auto view = world.GetEntitiesWith<Transform, AnimationController>();
        for (auto entity : view)
        {
            auto& anim = view.get<AnimationController>(entity);
            if (!anim.playing)
                continue;

            // Advance animation time
            anim.currentTime += deltaTime * anim.playbackSpeed;

            // Handle animation looping and completion
            if (anim.duration > 0.0f && anim.currentTime >= anim.duration)
            {
                if (anim.loop)
                {
                    anim.currentTime = std::fmod(anim.currentTime, anim.duration);
                }
                else
                {
                    anim.currentTime = anim.duration;
                    anim.playing = false;
                }
            }

            // Compute normalized progress [0,1] for blend weights and gameplay sync
            if (anim.duration > 0.0f)
            {
                anim.normalizedTime = anim.currentTime / anim.duration;
            }
        }
    }

    // ============================================================================
    // AIUpdateSystem
    // ============================================================================

    void AIUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        SPARK_WARN_IF(Spark::LogCategory::AI, deltaTime < 0.0f, "Negative deltaTime in AIUpdate");
        auto view = world.GetEntitiesWith<Transform, AIComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& ai = view.get<AIComponent>(entity);

            // Skip dead agents
            auto* health = world.GetComponent<HealthComponent>(entity);
            if (health && health->isDead)
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::AI, "AIUpdateSystem: entity %u marked as dead",
                                static_cast<uint32_t>(entity));
                ai.state = AIComponent::State::Dead;
                continue;
            }

            // Advance cooldown timers
            ai.attackCooldown.Update(deltaTime);
            ai.perceptionCooldown.Update(deltaTime);

            // Tick behavior tree if one is assigned
            if (ai.behaviorTreeHandle)
            {
                auto* bt = ai.behaviorTreeHandle.As<Spark::AI::BehaviorTree>();
                bt->Tick(deltaTime);
            }

            // Update perception timers
            if (ai.targetEntity != entt::null)
            {
                ai.timeSinceLastSeen += deltaTime;
            }
            if (ai.state == AIComponent::State::Alert)
            {
                ai.alertTimer += deltaTime;
            }

            // Path following
            if (!ai.currentPath.empty() && ai.currentPathIndex < ai.currentPath.size())
            {
                const auto& target = ai.currentPath[ai.currentPathIndex];
                float dx = target.x - transform.position.x;
                float dz = target.z - transform.position.z;
                float distSq = dx * dx + dz * dz;

                // 0.25 = 0.5m squared; waypoint is "reached" when agent is within 0.5m.
                // Using squared distance avoids an sqrt per-agent-per-frame.
                if (distSq < 0.25f)
                {
                    ai.currentPathIndex++;
                    if (ai.currentPathIndex >= ai.currentPath.size())
                    {
                        ai.currentPath.clear();
                        ai.currentPathIndex = 0;
                    }
                }
                else
                {
                    float dist = std::sqrt(distSq);
                    float speed = ai.config.moveSpeed * deltaTime;
                    transform.position.x += (dx / dist) * speed;
                    transform.position.z += (dz / dist) * speed;
                    transform.rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                }
            }
        }
    }

    // ============================================================================
    // SplineFollowerSystem
    // ============================================================================

    void SplineFollowerSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        m_activeFollowerCount = 0;

        auto view = world.GetEntitiesWith<Transform, SplineFollowerComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& follower = view.get<SplineFollowerComponent>(entity);

            if (!follower.playing || follower.finished)
                continue;

            // Look up the referenced spline entity
            if (follower.splineEntity == entt::null)
                continue;
            auto* splineComp = world.GetComponent<SplineComponent>(follower.splineEntity);
            if (!splineComp)
                continue;

            const SplinePath& path = splineComp->path;
            float totalLength = path.GetTotalLength();
            if (totalLength <= 0.0f)
                continue;

            // Advance distance
            float delta = follower.speed * deltaTime;
            if (follower.reverse)
                follower.currentDistance -= delta;
            else
                follower.currentDistance += delta;

            // Handle loop modes
            switch (follower.loopMode)
            {
            case SplineLoopMode::Once:
                if (follower.currentDistance >= totalLength)
                {
                    follower.currentDistance = totalLength;
                    follower.finished = true;
                }
                else if (follower.currentDistance < 0.0f)
                {
                    follower.currentDistance = 0.0f;
                    follower.finished = true;
                }
                break;

            case SplineLoopMode::Loop:
                if (follower.currentDistance >= totalLength)
                    follower.currentDistance = std::fmod(follower.currentDistance, totalLength);
                else if (follower.currentDistance < 0.0f)
                    follower.currentDistance = totalLength + std::fmod(follower.currentDistance, totalLength);
                break;

            case SplineLoopMode::PingPong:
                if (follower.currentDistance >= totalLength)
                {
                    follower.currentDistance = totalLength;
                    follower.reverse = true;
                }
                else if (follower.currentDistance <= 0.0f)
                {
                    follower.currentDistance = 0.0f;
                    follower.reverse = false;
                }
                break;
            }

            // Evaluate position
            XMFLOAT3 position = path.GetPointAtDistance(follower.currentDistance);
            transform.position = position;

            // Orient to path tangent
            if (follower.orientToPath)
            {
                XMFLOAT3 tangent = path.GetTangentAtDistance(follower.currentDistance);
                float tangentLen = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
                if (tangentLen > 0.0001f)
                {
                    float yaw = std::atan2(tangent.x, tangent.z) * MathUtils::RAD_TO_DEG;
                    float pitch = std::atan2(-tangent.y, std::sqrt(tangent.x * tangent.x + tangent.z * tangent.z)) *
                                  MathUtils::RAD_TO_DEG;
                    transform.rotation.x = pitch;
                    transform.rotation.y = yaw;
                }
            }

            m_activeFollowerCount++;
        }
    }

    // ============================================================================
    // ParticleUpdateSystem
    // ============================================================================

    void ParticleUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        m_activeParticleCount = 0;
        m_activeEmitterCount = 0;

        auto view = world.GetEntitiesWith<Transform, ParticleEmitterComponent>();
        for (auto entity : view)
        {
            auto& emitter = view.get<ParticleEmitterComponent>(entity);

            // Auto-play on first frame
            if (emitter.autoPlay && !emitter.isPlaying)
            {
                emitter.isPlaying = true;
            }

            if (!emitter.isPlaying)
                continue;

            // Check if entity is active
            auto* active = world.GetComponent<ActiveComponent>(entity);
            if (active && !active->active)
                continue;

            m_activeEmitterCount++;

            // Particle count is managed by the underlying particle system via
            // the emitter handle. Here we track active emitters for profiling.
        }
    }

    // ============================================================================
    // DecalSystem
    // ============================================================================

    void DecalSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        m_activeDecalCount = 0;
        std::vector<entt::entity> expiredDecals;

        auto view = world.GetEntitiesWith<Transform, DecalComponent>();
        for (auto entity : view)
        {
            auto& decal = view.get<DecalComponent>(entity);

            // Permanent decals (lifetime == 0) never expire
            if (decal.lifetime <= 0.0f)
            {
                m_activeDecalCount++;
                continue;
            }

            // Count down remaining lifetime
            decal.remainingLifetime -= deltaTime;

            if (decal.remainingLifetime <= 0.0f)
            {
                decal.remainingLifetime = 0.0f;
                expiredDecals.push_back(entity);
            }
            else
            {
                m_activeDecalCount++;
            }
        }

        // Fire expired callbacks
        if (m_onExpired)
        {
            for (auto entity : expiredDecals)
            {
                m_onExpired(entity);
            }
        }
    }

    // ============================================================================
    // ProjectileSystem
    // ============================================================================

    void ProjectileSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::ECS);
        SPARK_WARN_IF(Spark::LogCategory::ECS, deltaTime < 0.0f, "Negative deltaTime in ProjectileSystem");
        m_activeProjectileCount = 0;
        std::vector<entt::entity> expiredProjectiles;

        auto view = world.GetEntitiesWith<Transform, ProjectileComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& proj = view.get<ProjectileComponent>(entity);

            // Skip hitscan — they resolve instantly on the frame they're fired
            if (proj.movementType == ProjectileComponent::MovementType::Hitscan)
                continue;

            // Advance age
            proj.age += deltaTime;

            // Apply gravity to the direction vector (not position directly),
            // so the projectile follows a natural parabolic arc.
            constexpr float kGravity = 9.81f;
            proj.direction.y -= kGravity * proj.gravityScale * deltaTime;

            // Normalize direction and compute movement
            float dirLen = std::sqrt(proj.direction.x * proj.direction.x + proj.direction.y * proj.direction.y +
                                     proj.direction.z * proj.direction.z);
            if (dirLen > 0.0001f)
            {
                float invLen = 1.0f / dirLen;
                float moveX = proj.direction.x * invLen * proj.speed * deltaTime;
                float moveY = proj.direction.y * invLen * proj.speed * deltaTime;
                float moveZ = proj.direction.z * invLen * proj.speed * deltaTime;

                transform.position.x += moveX;
                transform.position.y += moveY;
                transform.position.z += moveZ;

                float moveDist = std::sqrt(moveX * moveX + moveY * moveY + moveZ * moveZ);
                proj.distanceTraveled += moveDist;

                // Orient projectile along its direction
                transform.rotation.y = std::atan2(proj.direction.x, proj.direction.z) * MathUtils::RAD_TO_DEG;
                transform.rotation.x = std::atan2(-proj.direction.y, std::sqrt(proj.direction.x * proj.direction.x +
                                                                               proj.direction.z * proj.direction.z)) *
                                       MathUtils::RAD_TO_DEG;
            }

            // Check expiration
            if (proj.IsExpired())
            {
                expiredProjectiles.push_back(entity);
            }
            else
            {
                m_activeProjectileCount++;
            }
        }

        // Fire expired callbacks
        if (m_onExpired)
        {
            for (auto entity : expiredProjectiles)
            {
                m_onExpired(entity);
            }
        }
    }

    // ============================================================================
    // AbilityUpdateSystem
    // ============================================================================

    void AbilityUpdateSystem::Update(World& world, float deltaTime)
    {
        m_activeCount = 0;

        auto view = world.GetRegistry().view<AbilityComponent>();
        for (auto entity : view)
        {
            auto& ability = view.get<AbilityComponent>(entity);
            ability.Update(deltaTime);
            ++m_activeCount;
        }
    }

    // ============================================================================
    // SystemManager Console Integration
    // ============================================================================

    std::string SystemManager::Console_ListSystems() const
    {
        std::ostringstream ss;
        ss << "=== ECS Systems (" << m_systems.size() << ") ===\n";
        for (const auto& system : m_systems)
        {
            ss << "  " << system->GetName() << " [" << (system->IsEnabled() ? "ENABLED" : "DISABLED") << "]\n";
        }
        return ss.str();
    }

} // namespace Spark::ECS
