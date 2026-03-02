// ECSystems.cpp
#include "ECSystems.h"
#include "Graphics/GraphicsEngine.h"
#include "Physics/PhysicsSystem.h"
#include "Audio/AudioEngine.h"
#include <sstream>

namespace Spark::ECS {

// ============================================================================
// RenderSystem
// ============================================================================

void RenderSystem::Update(World& world, float deltaTime)
{
    m_renderedCount = 0;

    auto view = world.GetEntitiesWith<Transform, MeshRenderer>();
    for (auto entity : view)
    {
        auto& transform = view.get<Transform>(entity);
        auto& renderer = view.get<MeshRenderer>(entity);

        if (!renderer.visible) continue;

        // Check if entity is active
        auto* active = world.GetComponent<ActiveComponent>(entity);
        if (active && !active->active) continue;

        // The GraphicsEngine handles actual rendering - we just ensure
        // transform data is ready. In a full integration, this would
        // submit draw calls to the graphics engine's render queue.
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

        auto* physBody = static_cast<PhysicsBody*>(rb.physicsBodyHandle);

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

        // Update audio source position from entity transform
        auto* source = static_cast<AudioSource*>(audio.audioSourceHandle);
        source->Position = transform.position;
    }
}

// ============================================================================
// LifecycleSystem
// ============================================================================

void LifecycleSystem::Update(World& world, float deltaTime)
{
    // Process health/death
    auto healthView = world.GetEntitiesWith<HealthComponent>();
    for (auto entity : healthView)
    {
        auto& health = healthView.get<HealthComponent>(entity);
        if (health.isDead && m_onDeath)
        {
            m_onDeath(entity);
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
