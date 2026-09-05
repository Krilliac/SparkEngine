/**
 * @file ProjectilePool.h
 * @brief Object pool system for efficient projectile management
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides an efficient object pool for managing projectiles to avoid
 * frequent memory allocations and deallocations during gameplay. It supports
 * multiple projectile types and provides convenient firing methods.
 */

#pragma once
#include "Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <memory>
#include <queue>
#include <vector>
#include "Projectile.h"

/**
 * @brief Enumeration of projectile types for factory creation
 */
enum class ProjectileType
{
    BULLET, ///< Fast, small projectiles for rifles and pistols
    ROCKET, ///< Explosive projectiles with area damage
    GRENADE ///< Lobbed explosive projectiles with gravity
};

/**
 * @brief Object pool for efficient projectile management
 * 
 * The ProjectilePool manages a fixed pool of projectile objects to avoid
 * memory allocation overhead during gameplay. It handles creation, updating,
 * rendering, and recycling of projectiles automatically.
 * 
 * Features include:
 * - Pre-allocated pool of reusable projectile objects
 * - Factory methods for different projectile types
 * - Automatic lifecycle management
 * - Performance tracking with active/available counts
 * 
 * @note Pool size should be large enough to handle peak projectile usage
 * @warning Initialize() must be called before firing any projectiles
 */
class PhysicsSystem;
class Enemy;

namespace Spark
{
    class EventBus;
}

class ProjectilePool
{
  public:
    /**
     * @brief Constructor with pool size specification
     * @param poolSize Maximum number of simultaneous projectiles
     */
    ProjectilePool(size_t poolSize);

    /**
     * @brief Destructor automatically cleans up all projectiles
     */
    ~ProjectilePool();

    /**
     * @brief Initialize the projectile pool with DirectX resources
     * @param device DirectX 11 device for projectile creation, or nullptr on a
     *               device-less (NullRHI / headless) host
     * @param context DirectX 11 device context for rendering, or nullptr
     * @return HRESULT indicating success or failure
     *
     * The pool is populated either way, so firing, flight, collision and recycling
     * behave identically on a headless host. Without a device the per-projectile GPU
     * mesh is skipped and Render() is a no-op; HasRenderResources() reports which
     * mode the pool is in.
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /** @brief Whether the pool owns GPU resources and can render its projectiles. */
    [[nodiscard]] bool HasRenderResources() const noexcept { return m_hasRenderResources; }

    /**
     * @brief Update all active projectiles
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void Update(float deltaTime);

    /**
     * @brief Render all active projectiles
     * @param view Camera view matrix
     * @param proj Camera projection matrix
     */
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

    /**
     * @brief Shutdown and clean up all resources
     */
    void Shutdown();

    /**
     * @brief Get an available projectile from the pool
     * @return Pointer to available projectile, or nullptr if pool is full
     */
    Projectile* GetProjectile();

    /** @brief Get an available projectile matching the requested weapon type. */
    Projectile* GetProjectile(ProjectileType type);

    /**
     * @brief Return a projectile to the available pool
     * @param p Pointer to the projectile to return
     */
    void ReturnProjectile(Projectile* p);

    /**
     * @brief Fire a bullet projectile
     * @param pos Starting position
     * @param dir Direction vector (should be normalized)
     * @param speed Initial speed
     */
    void FireBullet(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& dir, float speed);

    /**
     * @brief Fire a rocket projectile
     * @param pos Starting position
     * @param dir Direction vector (should be normalized)
     * @param speed Initial speed
     */
    void FireRocket(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& dir, float speed);

    /**
     * @brief Fire a grenade projectile
     * @param pos Starting position
     * @param dir Direction vector (should be normalized)
     * @param speed Initial speed
     */
    void FireGrenade(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& dir, float speed);

    /**
     * @brief Fire a projectile of specified type
     * @param type Type of projectile to fire
     * @param pos Starting position
     * @param dir Direction vector (should be normalized)
     * @param speed Initial speed
     */
    void FireProjectile(ProjectileType type, const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& dir, float speed,
                        float damage = -1.0f);

    /**
     * @brief Apply swept projectile hits to live arena enemies.
     * @return Number of projectiles that hit an enemy this frame.
     */
    size_t ResolveEnemyHits(const std::vector<Enemy*>& enemies, Spark::EventBus* eventBus);

    /**
     * @brief Set the physics system on all managed projectiles
     * @param ps Physics system pointer for area queries (explosions)
     */
    void SetPhysicsSystem(PhysicsSystem* ps);

    /**
     * @brief Get the number of currently active projectiles
     * @return Number of projectiles in use
     */
    size_t GetActiveCount() const;

    /**
     * @brief Get the number of available projectiles in the pool
     * @return Number of projectiles available for use
     */
    size_t GetAvailableCount() const;

  private:
    /**
     * @brief Create all projectile objects for the pool
     */
    void CreateProjectiles();

    PhysicsSystem* m_physicsSystem{nullptr};                ///< Cached physics system
    size_t m_poolSize;                                      ///< Maximum pool size
    ID3D11Device* m_device{nullptr};                        ///< DirectX device reference
    ID3D11DeviceContext* m_context{nullptr};                ///< DirectX context reference
    std::vector<std::unique_ptr<Projectile>> m_projectiles; ///< All projectile objects
    std::queue<Projectile*> m_availableProjectiles;         ///< Queue of available projectiles
    bool m_hasRenderResources{false};                       ///< True when the pool created GPU meshes
};
