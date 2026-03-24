/**
 * @file Projectile.h
 * @brief Base class for all projectile objects in the game
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides the fundamental functionality for projectile objects including
 * physics simulation, collision detection, lifetime management, and rendering.
 * All specific projectile types (bullets, rockets, grenades) inherit from this base class.
 */

#pragma once

#include "Core/framework.h"          // XMFLOAT3, XMMATRIX, HRESULT
#include "Physics/CollisionSystem.h" // BoundingSphere
#include "Game/GameObject.h"
#include "Utils/Assert.h"

class PhysicsSystem;

/**
 * @brief Base class for all projectile objects
 * 
 * The Projectile class provides the core functionality needed by all projectile
 * types in the game. It handles physics simulation, collision detection, lifetime
 * management, and basic rendering. Derived classes implement specific behaviors
 * for different projectile types like bullets, rockets, and grenades.
 * 
 * Features include:
 * - Physics-based movement with velocity and optional gravity
 * - Collision detection using bounding spheres
 * - Automatic lifetime management and deactivation
 * - Damage system integration
 * - Object pooling support for performance
 * 
 * @note This class inherits from GameObject and implements the collision callbacks
 * @warning Derived classes should call the base class methods when overriding
 */
class Projectile : public GameObject
{
  protected:
    // Motion
    DirectX::XMFLOAT3 m_velocity; ///< Current velocity vector
    float m_speed;                ///< Base speed magnitude
    float m_lifeTime;             ///< Current lifetime counter
    float m_maxLifeTime;          ///< Maximum lifetime before auto-deactivation
    float m_damage;               ///< Damage dealt to targets
    bool m_active;                ///< Whether projectile is currently active

    // Physics
    BoundingSphere m_boundingSphere;         ///< Collision bounds
    bool m_hasGravity;                       ///< Whether gravity affects this projectile
    float m_gravityScale;                    ///< Multiplier for gravity effect
    PhysicsSystem* m_physicsSystem{nullptr}; ///< Physics system for queries (e.g. explosions)

  public:
    /**
     * @brief Default constructor
     * 
     * Initializes projectile with default values suitable for most projectile types.
     */
    Projectile();

    /**
     * @brief Virtual destructor for proper polymorphic cleanup
     */
    ~Projectile() override;

    /**
     * @brief Initialize the projectile with DirectX resources
     * @param device DirectX 11 device for resource creation
     * @param context DirectX 11 device context for rendering
     * @return HRESULT indicating success or failure
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Update projectile physics and lifetime
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void Update(float deltaTime) override;

    /**
     * @brief Render the projectile
     * @param view Camera view matrix
     * @param projection Camera projection matrix
     */
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection) override;

    /**
     * @brief Fire the projectile with initial parameters
     * @param startPosition World position to start from
     * @param direction Normalized direction vector
     * @param speed Initial speed magnitude
     */
    virtual void Fire(const DirectX::XMFLOAT3& startPosition, const DirectX::XMFLOAT3& direction, float speed);

    /**
     * @brief Deactivate the projectile for object pool return
     */
    void Deactivate();

    /**
     * @brief Reset projectile state for reuse
     */
    void Reset();

    /**
     * @brief Collision callback when projectile hits another object
     * @param target Pointer to the hit game object
     */
    void OnHit(GameObject* target) override;

    /**
     * @brief Collision callback when projectile hits world geometry
     * @param hitPoint World position of collision
     * @param normal Surface normal at collision point
     */
    void OnHitWorld(const DirectX::XMFLOAT3& hitPoint, const DirectX::XMFLOAT3& normal) override;

    /**
     * @brief Configure gravity settings for the projectile
     * @param enabled Whether gravity should affect this projectile
     * @param scale Gravity scale multiplier (default: 1.0)
     */
    void SetGravity(bool enabled, float scale = 1.0f);

    /**
     * @brief Apply an external force to the projectile
     * @param force Force vector to apply
     */
    void ApplyForce(const DirectX::XMFLOAT3& force);

    /**
     * @brief Check if projectile is currently active
     * @return true if active, false if available for reuse
     */
    bool IsActive() const { return m_active; }

    /**
     * @brief Get the damage value of this projectile
     * @return Damage amount
     */
    float GetDamage() const { return m_damage; }

    /**
     * @brief Get the current velocity vector
     * @return Current velocity
     */
    const DirectX::XMFLOAT3& GetVelocity() const { return m_velocity; }

    /**
     * @brief Get the collision bounding sphere
     * @return Bounding sphere for collision detection
     */
    const BoundingSphere& GetBoundingSphere() const { return m_boundingSphere; }

    /**
     * @brief Set the damage amount
     * @param damage New damage value (must be non-negative)
     */
    void SetDamage(float damage)
    {
        ASSERT_MSG(damage >= 0, "Damage must be non-negative");
        m_damage = damage;
    }

    /**
     * @brief Set the maximum lifetime
     * @param lifeTime New lifetime in seconds (must be positive)
     */
    void SetLifeTime(float lifeTime)
    {
        ASSERT_MSG(lifeTime > 0, "LifeTime must be positive");
        m_maxLifeTime = lifeTime;
    }

    /**
     * @brief Set the physics system for area queries (explosions, etc.)
     * @param ps Physics system pointer
     */
    void SetPhysicsSystem(PhysicsSystem* ps) { m_physicsSystem = ps; }

  protected:
    /**
     * @brief Create the mesh for this projectile type
     * 
     * Virtual method that derived classes should override to create
     * their specific mesh geometry.
     */
    void CreateMesh() override;

    /**
     * @brief Check for collisions with world and other objects
     */
    void CheckCollisions();

    /**
     * @brief Update physics simulation
     * @param deltaTime Time step for physics integration
     */
    void UpdatePhysics(float deltaTime);

    /**
     * @brief Update the bounding sphere position
     */
    void UpdateBoundingSphere();
};
