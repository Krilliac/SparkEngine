/**
 * @file Grenade.h
 * @brief Gravity-affected grenade projectile with timed fuse and area explosion
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a grenade projectile that follows a parabolic arc (affected by
 * gravity) and detonates after a configurable fuse time, dealing area-of-effect
 * damage within its explosion radius.
 *
 * Grenades are managed through the ProjectilePool for efficient reuse and should
 * not be allocated individually during gameplay.
 *
 * @code
 *   // Typically fired through the ProjectilePool:
 *   pool.FireGrenade(throwPosition, throwDirection, 15.0f);
 * @endcode
 *
 * @see Projectile, ProjectilePool, Bullet, Rocket
 */

#pragma once
#include "Core/Platform.h"

#include "Projectile.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief Gravity-affected explosive projectile with timed detonation
 *
 * Grenade extends Projectile with gravity-based physics, a fuse timer, and
 * area-of-effect explosion mechanics. Unlike bullets, grenades follow a
 * parabolic trajectory and can bounce off surfaces before detonating.
 *
 * The grenade detonates either when the fuse timer expires or when it
 * collides with a target (depending on configuration). The explosion deals
 * damage to all objects within m_explosionRadius.
 *
 * @note Grenades have gravity enabled by default, producing an arcing trajectory.
 * @warning The Explode() method should only be called once; m_hasExploded
 *          guards against double detonation.
 * @see Projectile, Bullet, Rocket, ProjectilePool
 */
class Grenade : public Projectile
{
  public:
    /**
     * @brief Construct a grenade with default fuse and explosion parameters
     *
     * Sets up grenade-specific defaults: moderate speed, gravity enabled,
     * configurable fuse time, and area explosion radius.
     */
    Grenade();

    /** @brief Default virtual destructor */
    ~Grenade() override = default;

    /**
     * @brief Initialize the grenade's mesh and DirectX resources
     *
     * Creates a small spherical mesh to represent the grenade visually.
     *
     * @param device  DirectX 11 device for GPU resource creation
     * @param context DirectX 11 device context for rendering commands
     * @return S_OK on success, or an error HRESULT on failure
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Update grenade physics, fuse timer, and collision detection
     *
     * Applies gravity to the velocity, moves the grenade, decrements the fuse
     * timer, and triggers Explode() when the fuse expires. Also performs
     * collision detection with world geometry and other objects.
     *
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void Update(float deltaTime) override;

    /**
     * @brief Render the grenade mesh
     * @param view       Camera view matrix
     * @param projection Camera projection matrix
     */
    void Render(const XMMATRIX& view, const XMMATRIX& projection) override;

    /**
     * @brief Fire the grenade with an initial arcing trajectory
     *
     * Overrides the base Fire() to enable gravity and set up the initial
     * fuse timer and explosion state.
     *
     * @param startPosition World position to launch from
     * @param direction     Normalized throw direction vector
     * @param speed         Initial throw speed in units per second
     */
    void Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed) override;

  private:
    /**
     * @brief Trigger the grenade explosion
     *
     * Deals area-of-effect damage to all objects within m_explosionRadius,
     * spawns explosion visual effects, and deactivates the grenade.
     * Guarded by m_hasExploded to prevent double detonation.
     */
    void Explode();

    float m_fuseTime;        ///< Remaining fuse time in seconds before detonation
    float m_explosionRadius; ///< Radius of the explosion's area-of-effect damage
    bool m_hasExploded;      ///< Guard flag to prevent double detonation
};
