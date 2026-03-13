// ECSystems.cpp
#include "../../../Core/Platform.h"
#include "ECSystems.h"
#include "Graphics/GraphicsEngine.h"
#include "Physics/PhysicsSystem.h"
#include "Audio/AudioEngine.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/ECS/Components/FPSComponents.h"
#include "Utils/Cooldown.h"
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
        m_renderedCount = 0;
        if (!m_graphics)
            return;

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

            // Compute the world matrix, walking the parent hierarchy if present
            XMMATRIX worldMtx = transform.GetWorldMatrix(registry);
            XMStoreFloat4x4(&renderer.cachedWorldMatrix, worldMtx);
            renderer.worldMatrixDirty = false;

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
        if (!m_physics)
            return;

        auto view = world.GetEntitiesWith<Transform, RigidBodyComponent>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& rb = view.get<RigidBodyComponent>(entity);

            if (!rb.physicsBodyHandle)
                continue;

            auto* physBody = rb.physicsBodyHandle.As<PhysicsBody>();
            if (!physBody)
                continue;

            if (rb.type == RigidBodyComponent::Type::Dynamic)
            {
                // Read position/rotation back from physics simulation
                DirectX::XMFLOAT3 physPos = physBody->GetPosition();
                DirectX::XMFLOAT3 physRot = physBody->GetRotation();
                transform.position = physPos;
                transform.rotation = physRot;

                // Update velocity cache
                rb.linearVelocity = physBody->GetLinearVelocity();
                rb.angularVelocity = physBody->GetAngularVelocity();
            }
            else if (rb.type == RigidBodyComponent::Type::Kinematic)
            {
                // Push transform to physics
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
        if (!m_audio)
            return;

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

            // Compute velocity from position delta for Doppler effects
            if (deltaTime > 0.0f)
            {
                source->Velocity.x = (transform.position.x - audio.previousPosition.x) / deltaTime;
                source->Velocity.y = (transform.position.y - audio.previousPosition.y) / deltaTime;
                source->Velocity.z = (transform.position.z - audio.previousPosition.z) / deltaTime;
            }

            // Update position and track for next frame's velocity calculation
            source->Position = transform.position;
            audio.previousPosition = transform.position;
        }
    }

    // ============================================================================
    // LifecycleSystem
    // ============================================================================

    void LifecycleSystem::Update(World& world, float deltaTime)
    {
        // Collect dead entities first, then fire callbacks to avoid
        // iterator invalidation if the callback destroys the entity.
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

        if (m_onDeath)
        {
            for (auto entity : deadEntities)
            {
                m_onDeath(entity);
            }
        }
    }

    // ============================================================================
    // AnimationUpdateSystem
    // ============================================================================

    void AnimationUpdateSystem::Update(World& world, float deltaTime)
    {
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

                if (distSq < 0.25f)
                { // Waypoint reached
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
                    transform.rotation.y = std::atan2(dx, dz) * (180.0f / 3.14159265f);
                }
            }
        }
    }

    // ============================================================================
    // SplineFollowerSystem
    // ============================================================================

    void SplineFollowerSystem::Update(World& world, float deltaTime)
    {
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
                    float yaw = std::atan2(tangent.x, tangent.z) * (180.0f / 3.14159265f);
                    float pitch = std::atan2(-tangent.y, std::sqrt(tangent.x * tangent.x + tangent.z * tangent.z)) *
                                  (180.0f / 3.14159265f);
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

            // Apply gravity for ballistic projectiles
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
                transform.rotation.y = std::atan2(proj.direction.x, proj.direction.z) * (180.0f / 3.14159265f);
                transform.rotation.x = std::atan2(-proj.direction.y, std::sqrt(proj.direction.x * proj.direction.x +
                                                                               proj.direction.z * proj.direction.z)) *
                                       (180.0f / 3.14159265f);
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
