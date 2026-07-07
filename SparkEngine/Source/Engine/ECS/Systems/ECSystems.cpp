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
#include "ECSystems.h"
#include "../../../Core/Platform.h"
#include "Graphics/GraphicsEngine.h"
#include "Physics/PhysicsSystem.h"
#include "Audio/AudioEngine.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/ECS/Components/FPSComponents.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/PhysicsComponents.h"
#include "../../../Utils/DeferredDeletion.h"
#include "Utils/Cooldown.h"
#include "Utils/DebugHookManager.h"
#include "Utils/MathUtils.h"
#include "Utils/Validate.h"
#include <chrono>
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
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "ECS.Render", 0.0);
        auto ecsSysStart = std::chrono::high_resolution_clock::now();
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

            // Skip inactive entities — use registry.try_get to avoid a second hash lookup
            // through World::GetComponent, since we already hold the registry reference.
            auto* active = registry.try_get<ActiveComponent>(entity);
            if (active && !active->active)
                continue;

            // Log visibility state changes could go here, but mesh submission
            // is per-frame work — only the rendered count is meaningful at INFO level.

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

        auto ecsSysEnd = std::chrono::high_resolution_clock::now();
        double ecsMs = std::chrono::duration<double, std::milli>(ecsSysEnd - ecsSysStart).count();
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.Render", ecsMs);
    }

    // ============================================================================
    // PhysicsUpdateSystem
    // ============================================================================

    void PhysicsUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "ECS.Physics", 0.0);
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
                SPARK_LOG_INFO(Spark::LogCategory::Physics,
                               "PhysicsUpdateSystem: auto-creating physics body for entity %u",
                               static_cast<uint32_t>(entity));
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
                auto* collider = world.GetRegistry().try_get<ColliderComponent>(entity);
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
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.Physics", 0.0);
    }

    // ============================================================================
    // AudioUpdateSystem
    // ============================================================================

    void AudioUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "ECS.Audio", 0.0);
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
            //
            // Guard against spurious spikes: on the entity's first update the stored
            // previousPosition is still its default {0,0,0}, so an entity spawned away
            // from the origin would yield velocity = position/dt (e.g. 6250 m/s), and a
            // teleport (AI/spline systems snapping the transform) produces the same
            // artifact. Any derived speed beyond the speed of sound is physically
            // implausible for a Doppler source and drives X3DAudio into degenerate
            // pitch shifts, so we treat it as a discontinuity and leave Velocity zero.
            if (deltaTime > 1e-6f)
            {
                const float vx = (transform.position.x - audio.previousPosition.x) / deltaTime;
                const float vy = (transform.position.y - audio.previousPosition.y) / deltaTime;
                const float vz = (transform.position.z - audio.previousPosition.z) / deltaTime;

                // Speed of sound (~343 m/s at 20 C). Beyond this, X3DAudio Doppler is
                // undefined, so a jump this large is a first-frame/teleport discontinuity.
                constexpr float kMaxDopplerSpeed = 343.0f;
                if (vx * vx + vy * vy + vz * vz <= kMaxDopplerSpeed * kMaxDopplerSpeed)
                {
                    source->Velocity.x = vx;
                    source->Velocity.y = vy;
                    source->Velocity.z = vz;
                }
                else
                {
                    source->Velocity.x = 0.0f;
                    source->Velocity.y = 0.0f;
                    source->Velocity.z = 0.0f;
                }
            }

            // Update 3D position and store for next frame's velocity derivation
            source->Position = transform.position;
            audio.previousPosition = transform.position;
        }
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.Audio", 0.0);
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
        // Uses persistent m_deadEntities to avoid heap allocation every frame.

        auto healthView = world.GetEntitiesWith<HealthComponent>();
        for (auto entity : healthView)
        {
            auto& health = healthView.get<HealthComponent>(entity);
            if (health.isDead && !health.deathProcessed)
            {
                health.deathProcessed = true;
                m_deadEntities.MarkForDeletion(entity);
            }
        }

        if (m_deadEntities.GetPendingCount() > 0)
        {
            SPARK_LOG_INFO(Spark::LogCategory::ECS, "LifecycleSystem: %zu entities died this frame",
                           m_deadEntities.GetPendingCount());
        }

        if (m_onDeath)
        {
            m_deadEntities.Flush(
                [&](entt::entity& entity)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::ECS, "LifecycleSystem: firing death callback for entity %u",
                                    static_cast<uint32_t>(entity));
                    m_onDeath(entity);
                });
        }
        else
        {
            SPARK_WARN_IF(Spark::LogCategory::ECS, !m_deadEntities.IsEmpty(),
                          "LifecycleSystem: entities died but no death callback is registered");
            m_deadEntities.FlushAll();
        }
    }

    // ============================================================================
    // AnimationUpdateSystem
    // ============================================================================

    void AnimationUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Animation);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "ECS.Animation", 0.0);
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
                    SPARK_LOG_DEBUG(Spark::LogCategory::Animation,
                                    "AnimationUpdateSystem: animation completed for entity %u",
                                    static_cast<uint32_t>(entity));
                }
            }

            // Compute normalized progress [0,1] for blend weights and gameplay sync
            if (anim.duration > 0.0f)
            {
                anim.normalizedTime = anim.currentTime / anim.duration;
            }
        }
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.Animation", 0.0);
    }

    // ============================================================================
    // AIUpdateSystem
    // ============================================================================

    void AIUpdateSystem::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "ECS.AI", 0.0);
        SPARK_WARN_IF(Spark::LogCategory::AI, deltaTime < 0.0f, "Negative deltaTime in AIUpdate");
        auto view = world.GetEntitiesWith<Transform, AIComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& ai = view.get<AIComponent>(entity);

            // Skip dead agents — use registry directly to avoid World wrapper overhead
            auto* health = world.GetRegistry().try_get<HealthComponent>(entity);
            if (health && health->isDead)
            {
                if (ai.state != AIComponent::State::Dead)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::AI, "AIUpdateSystem: entity %u transitioned to Dead state",
                                   static_cast<uint32_t>(entity));
                }
                ai.state = AIComponent::State::Dead;
                continue;
            }

            // Advance cooldown timers
            ai.attackCooldown.Update(deltaTime);
            ai.perceptionCooldown.Update(deltaTime);

            // Tick behavior tree if one is assigned
            if (ai.behaviorTreeHandle)
            {
                if (auto* bt = ai.behaviorTreeHandle.As<Spark::AI::BehaviorTree>())
                {
                    bt->Tick(deltaTime);
                }
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

            // Path following.
            // Direct Transform writes are only valid for agents that are NOT backed by a
            // Dynamic rigid body. A Dynamic body is authority-owned by the physics engine:
            // PhysicsUpdateSystem overwrites its Transform from the simulation every frame,
            // and Physics runs before AI in the frame order, so a Transform write here would
            // be immediately stomped next frame and the two systems would fight (stutter / no
            // movement). Kinematic and bodyless agents are safe — for a Kinematic body
            // PhysicsUpdateSystem reads the Transform *into* the body, and AI runs after it.
            const auto* pathRigidBody = world.GetRegistry().try_get<RigidBodyComponent>(entity);
            const bool dynamicBody = pathRigidBody && pathRigidBody->type == RigidBodyComponent::Type::Dynamic;
            if (!dynamicBody && !ai.currentPath.empty() && ai.currentPathIndex < ai.currentPath.size())
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
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.AI", 0.0);
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
            auto* splineComp = world.GetRegistry().try_get<SplineComponent>(follower.splineEntity);
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
                float xzLenSq = tangent.x * tangent.x + tangent.z * tangent.z;
                float tangentLenSq = xzLenSq + tangent.y * tangent.y;
                if (tangentLenSq > 0.0001f * 0.0001f)
                {
                    float xzLen = std::sqrt(xzLenSq);
                    float yaw = std::atan2(tangent.x, tangent.z) * MathUtils::RAD_TO_DEG;
                    float pitch = std::atan2(-tangent.y, xzLen) * MathUtils::RAD_TO_DEG;
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

            // Skip inactive entities — direct registry access avoids World wrapper overhead
            auto* active = world.GetRegistry().try_get<ActiveComponent>(entity);
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
                m_expiredDecals.MarkForDeletion(entity);
            }
            else
            {
                m_activeDecalCount++;
            }
        }

        // Fire expired callbacks (persistent queue avoids per-frame allocation)
        if (m_onExpired)
        {
            m_expiredDecals.Flush([&](entt::entity& e) { m_onExpired(e); });
        }
        else
        {
            m_expiredDecals.FlushAll();
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

            // Normalize direction and compute movement — single sqrt, derive moveDist algebraically
            float dirLenSq = proj.direction.x * proj.direction.x + proj.direction.y * proj.direction.y +
                             proj.direction.z * proj.direction.z;
            if (dirLenSq > 0.0001f * 0.0001f)
            {
                float dirLen = std::sqrt(dirLenSq);
                float invLen = 1.0f / dirLen;
                float scaledSpeed = proj.speed * deltaTime;
                float moveX = proj.direction.x * invLen * scaledSpeed;
                float moveY = proj.direction.y * invLen * scaledSpeed;
                float moveZ = proj.direction.z * invLen * scaledSpeed;

                transform.position.x += moveX;
                transform.position.y += moveY;
                transform.position.z += moveZ;

                // moveDist = |normalized_dir * scaledSpeed| = scaledSpeed (since dir is normalized)
                proj.distanceTraveled += scaledSpeed;

                // Orient projectile along its direction — reuse xzLen from dirLen components
                float xzLenSq = proj.direction.x * proj.direction.x + proj.direction.z * proj.direction.z;
                float xzLen = std::sqrt(xzLenSq);
                transform.rotation.y = std::atan2(proj.direction.x, proj.direction.z) * MathUtils::RAD_TO_DEG;
                transform.rotation.x = std::atan2(-proj.direction.y, xzLen) * MathUtils::RAD_TO_DEG;
            }

            // Check expiration
            if (proj.IsExpired())
            {
                m_expiredProjectiles.MarkForDeletion(entity);
            }
            else
            {
                m_activeProjectileCount++;
            }
        }

        // Fire expired callbacks (persistent queue avoids per-frame allocation)
        if (m_onExpired)
        {
            m_expiredProjectiles.Flush([&](entt::entity& e) { m_onExpired(e); });
        }
        else
        {
            m_expiredProjectiles.FlushAll();
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
