// ECSystems.cpp
#include "../../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "ECSystems.h"
#include "Graphics/GraphicsEngine.h"
#include "Physics/PhysicsSystem.h"
#include "Audio/AudioEngine.h"
#include "Engine/AI/BehaviorTree.h"
#include <sstream>
#include <cmath>

using namespace DirectX;
namespace Spark::ECS {

// ============================================================================
// RenderSystem
// ============================================================================

void RenderSystem::Update(World& world, float deltaTime)
{
    m_renderedCount = 0;
    if (!m_graphics) return;

    auto view = world.GetEntitiesWith<Transform, MeshRenderer>();
    for (auto entity : view)
    {
        auto& transform = view.get<Transform>(entity);
        auto& renderer = view.get<MeshRenderer>(entity);

        if (!renderer.visible) continue;

        // Check if entity is active
        auto* active = world.GetComponent<ActiveComponent>(entity);
        if (active && !active->active) continue;

        // Build world matrix from Transform components
        XMMATRIX scaleMtx = XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z);
        XMMATRIX rotMtx = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(transform.rotation.x),
            XMConvertToRadians(transform.rotation.y),
            XMConvertToRadians(transform.rotation.z));
        XMMATRIX transMtx = XMMatrixTranslation(
            transform.position.x, transform.position.y, transform.position.z);

        XMMATRIX worldMtx = scaleMtx * rotMtx * transMtx;
        XMStoreFloat4x4(&renderer.cachedWorldMatrix, worldMtx);
        renderer.worldMatrixDirty = false;

        m_renderedCount++;
    }
}

// ============================================================================
// PhysicsUpdateSystem
// ============================================================================

void PhysicsUpdateSystem::Update(World& world, float deltaTime)
{
    if (!m_physics) return;

    auto view = world.GetEntitiesWith<Transform, RigidBodyComponent>();
    for (auto entity : view)
    {
        auto& transform = view.get<Transform>(entity);
        auto& rb = view.get<RigidBodyComponent>(entity);

        if (!rb.physicsBodyHandle) continue;

        auto* physBody = rb.physicsBodyHandle.As<PhysicsBody>();

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
    if (!m_audio) return;

    auto view = world.GetEntitiesWith<Transform, AudioSourceComponent>();
    for (auto entity : view)
    {
        auto& transform = view.get<Transform>(entity);
        auto& audio = view.get<AudioSourceComponent>(entity);

        if (!audio.is3D || !audio.audioSourceHandle) continue;

        auto* source = audio.audioSourceHandle.As<AudioSource>();

        // Compute velocity from position delta for Doppler effects
        if (deltaTime > 0.0f) {
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
    // Process health/death - only fire callback once per death event
    auto healthView = world.GetEntitiesWith<HealthComponent>();
    for (auto entity : healthView)
    {
        auto& health = healthView.get<HealthComponent>(entity);
        if (health.isDead && !health.deathProcessed && m_onDeath)
        {
            health.deathProcessed = true;
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
        if (!anim.playing) continue;

        // Advance animation time
        anim.currentTime += deltaTime * anim.playbackSpeed;

        // Handle animation looping and completion
        if (anim.duration > 0.0f && anim.currentTime >= anim.duration) {
            if (anim.loop) {
                anim.currentTime = std::fmod(anim.currentTime, anim.duration);
            } else {
                anim.currentTime = anim.duration;
                anim.playing = false;
            }
        }

        // Compute normalized progress [0,1] for blend weights and gameplay sync
        if (anim.duration > 0.0f) {
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
        if (health && health->isDead) {
            ai.state = AIComponent::State::Dead;
            continue;
        }

        // Tick behavior tree if one is assigned
        if (ai.behaviorTreeHandle) {
            auto* bt = ai.behaviorTreeHandle.As<Spark::AI::BehaviorTree>();
            bt->Tick(deltaTime);
        }

        // Update perception timers
        if (ai.targetEntity != entt::null) {
            ai.timeSinceLastSeen += deltaTime;
        }
        if (ai.state == AIComponent::State::Alert) {
            ai.alertTimer += deltaTime;
        }

        // Path following
        if (!ai.currentPath.empty() && ai.currentPathIndex < ai.currentPath.size()) {
            const auto& target = ai.currentPath[ai.currentPathIndex];
            float dx = target.x - transform.position.x;
            float dz = target.z - transform.position.z;
            float distSq = dx * dx + dz * dz;

            if (distSq < 0.25f) {  // Waypoint reached
                ai.currentPathIndex++;
                if (ai.currentPathIndex >= ai.currentPath.size()) {
                    ai.currentPath.clear();
                    ai.currentPathIndex = 0;
                }
            } else {
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
// SystemManager Console Integration
// ============================================================================

std::string SystemManager::Console_ListSystems() const
{
    std::ostringstream ss;
    ss << "=== ECS Systems (" << m_systems.size() << ") ===\n";
    for (const auto& system : m_systems)
    {
        ss << "  " << system->GetName()
           << " [" << (system->IsEnabled() ? "ENABLED" : "DISABLED") << "]\n";
    }
    return ss.str();
}

} // namespace Spark::ECS
#endif // SPARK_PLATFORM_WINDOWS
