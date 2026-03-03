/**
 * @file Components.h
 * @brief ECS components and World class for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides all ECS components used by the engine systems,
 * plus the World class that wraps the EnTT registry.
 */

#pragma once
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include <string>
#include <vector>
#include <functional>

using EntityID = entt::entity;

class World {
public:
    EntityID CreateEntity(const std::string& name = "");
    void DestroyEntity(EntityID entity);

    template<typename T, typename... Args>
    T& AddComponent(EntityID entity, Args&&... args) {
        return m_registry.emplace<T>(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    T* GetComponent(EntityID entity) {
        return m_registry.try_get<T>(entity);
    }

    template<typename T>
    bool HasComponent(EntityID entity) {
        return m_registry.all_of<T>(entity);
    }

    template<typename T>
    void RemoveComponent(EntityID entity) {
        m_registry.remove<T>(entity);
    }

    template<typename... Components>
    auto GetEntitiesWith() {
        return m_registry.view<Components...>();
    }

    size_t GetEntityCount() const { return m_registry.alive(); }
    entt::registry& GetRegistry() { return m_registry; }
    const entt::registry& GetRegistry() const { return m_registry; }

private:
    entt::registry m_registry;
};

// ============================================================================
// Core Components (existing)
// ============================================================================

struct NameComponent {
    std::string name;
};

struct Transform {
    DirectX::XMFLOAT3 position{0,0,0};
    DirectX::XMFLOAT3 rotation{0,0,0};
    DirectX::XMFLOAT3 scale{1,1,1};
    EntityID parent = entt::null;

    DirectX::XMMATRIX GetWorldMatrix() const;
};

struct MeshRenderer {
    std::string meshPath;
    std::string materialPath;
    bool castShadows = true;
    bool receiveShadows = true;
    bool visible = true;
};

struct Camera {
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool isMainCamera = false;
};

struct Script {
    std::string scriptPath;
    std::string className;
    std::string moduleName;
    bool enabled = true;
    bool started = false;
};

// ============================================================================
// New Components - Physics
// ============================================================================

struct RigidBodyComponent {
    enum class Type { Static, Kinematic, Dynamic };
    Type type = Type::Dynamic;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.1f;
    float angularDamping = 0.1f;
    bool isTrigger = false;
    DirectX::XMFLOAT3 linearVelocity{0,0,0};
    DirectX::XMFLOAT3 angularVelocity{0,0,0};
    void* physicsBodyHandle = nullptr;  ///< Opaque handle to PhysicsBody
};

struct ColliderComponent {
    enum class Shape { Box, Sphere, Capsule, Mesh };
    Shape shape = Shape::Box;
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f;
    float height = 1.0f;
    DirectX::XMFLOAT3 offset{0,0,0};
};

// ============================================================================
// New Components - Audio
// ============================================================================

struct AudioSourceComponent {
    std::string soundName;
    float volume = 1.0f;
    float pitch = 1.0f;
    float minDistance = 1.0f;
    float maxDistance = 50.0f;
    bool is3D = true;
    bool loop = false;
    bool playOnAwake = false;
    bool isPlaying = false;
    void* audioSourceHandle = nullptr;  ///< Opaque handle to AudioSource
};

// ============================================================================
// New Components - Lighting
// ============================================================================

struct LightComponent {
    enum class Type { Directional, Point, Spot };
    Type type = Type::Point;
    DirectX::XMFLOAT3 color{1,1,1};
    float intensity = 1.0f;
    float range = 10.0f;            ///< Point/spot light range
    float spotAngle = 45.0f;       ///< Spot light cone angle in degrees
    float spotInnerAngle = 30.0f;  ///< Spot light inner cone angle
    bool castShadows = false;
    int shadowMapResolution = 1024;
};

// ============================================================================
// New Components - Particles
// ============================================================================

struct ParticleEmitterComponent {
    std::string effectName;         ///< Name of particle effect preset
    bool autoPlay = true;
    bool isPlaying = false;
    float emissionRate = 10.0f;
    float lifetime = 1.0f;
    DirectX::XMFLOAT4 startColor{1,1,1,1};
    float startSize = 0.1f;
    float startSpeed = 1.0f;
    void* emitterHandle = nullptr;  ///< Opaque handle to ParticleEmitter
};

// ============================================================================
// New Components - Animation
// ============================================================================

struct AnimationController {
    std::string currentAnimation;
    std::string defaultAnimation;
    float playbackSpeed = 1.0f;
    float currentTime = 0.0f;
    bool playing = true;
    bool loop = true;
    std::vector<std::string> availableAnimations;
    void* animInstanceHandle = nullptr;  ///< Opaque handle to Spark::Animation::AnimationInstance
};

// ============================================================================
// New Components - AI
// ============================================================================

struct AIComponent {
    enum class State { Idle, Patrolling, Alert, Combat, Fleeing, Dead };
    State state = State::Idle;

    std::string behaviorTreeName;
    void* behaviorTreeHandle = nullptr;  ///< Opaque handle to BehaviorTree instance

    EntityID targetEntity = entt::null;
    DirectX::XMFLOAT3 lastKnownTargetPos{0, 0, 0};
    float timeSinceLastSeen = 0.0f;
    float alertTimer = 0.0f;

    std::vector<DirectX::XMFLOAT3> currentPath;
    size_t currentPathIndex = 0;

    struct Config {
        float detectionRange = 30.0f;
        float attackRange = 15.0f;
        float moveSpeed = 5.0f;
        float reactionTime = 0.5f;
        float accuracy = 0.7f;
    } config;
};

// ============================================================================
// New Components - Networking
// ============================================================================

struct NetworkIdentity {
    uint32_t networkID = 0;           ///< Unique network entity ID
    uint32_t ownerClientID = 0;       ///< Client that owns this entity
    bool isLocalAuthority = false;     ///< Whether this client has authority
    bool replicateTransform = true;
    bool replicateHealth = true;
};

// ============================================================================
// New Components - Tags & Metadata
// ============================================================================

struct TagComponent {
    std::vector<std::string> tags;

    bool HasTag(const std::string& tag) const {
        for (const auto& t : tags)
            if (t == tag) return true;
        return false;
    }

    void AddTag(const std::string& tag) {
        if (!HasTag(tag))
            tags.push_back(tag);
    }
};

struct ActiveComponent {
    bool active = true;
};

struct HealthComponent {
    float health = 100.0f;
    float maxHealth = 100.0f;
    bool isDead = false;

    void TakeDamage(float amount) {
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    void Heal(float amount) {
        health = (std::min)(health + amount, maxHealth);
        isDead = false;
    }
};

