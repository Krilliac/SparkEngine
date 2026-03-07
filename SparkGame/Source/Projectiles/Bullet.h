/**
 * @file Bullet.h
 * @brief High-speed hitscan-style bullet projectile for rifles and pistols
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a lightweight bullet projectile that travels in a straight line at
 * high speed. Bullets are the simplest projectile type: they have no gravity,
 * no area-of-effect damage, and no special trail effects. They are used by
 * pistols, rifles, and similar rapid-fire weapons.
 *
 * Bullets are managed through the ProjectilePool for efficient reuse and should
 * not be allocated individually during gameplay.
 *
 * @code
 *   // Typically fired through the ProjectilePool:
 *   pool.FireBullet(muzzlePosition, aimDirection, 500.0f);
 * @endcode
 *
 * @see Projectile, ProjectilePool, Grenade, Rocket
 */

#pragma once
#include "Core/Platform.h"

#include "Projectile.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

using DirectX::XMMATRIX;
using DirectX::XMFLOAT3;

/**
 * @brief Fast straight-line projectile for conventional firearms
 *
 * Bullet is the simplest Projectile specialization. It overrides Initialize(),
 * Update(), and Render() to provide bullet-specific mesh creation, linear
 * trajectory movement (no gravity), and minimal rendering geometry.
 *
 * Bullet inherits all collision detection, lifetime management, and damage
 * logic from the base Projectile class.
 *
 * @note Bullets do not apply gravity by default. They travel in a perfectly
 *       straight line until they hit something or their lifetime expires.
 * @see Projectile, Grenade, Rocket, ProjectilePool
 */
class Bullet : public Projectile
{
public:
    /**
     * @brief Construct a bullet with default weapon statistics
     *
     * Sets up bullet-specific defaults: high speed, low damage per round,
     * short lifetime, and no gravity.
     */
    Bullet();

    /** @brief Default virtual destructor */
    ~Bullet() override = default;

    /**
     * @brief Initialize the bullet's mesh and DirectX resources
     *
     * Creates a small elongated mesh to represent the bullet visually.
     *
     * @param device  DirectX 11 device for GPU resource creation
     * @param context DirectX 11 device context for rendering commands
     * @return S_OK on success, or an error HRESULT on failure
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Update bullet position and check for collisions
     *
     * Moves the bullet along its velocity vector and performs collision
     * detection. Deactivates the bullet if its lifetime expires.
     *
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void    Update(float deltaTime) override;

    /**
     * @brief Render the bullet mesh
     * @param view       Camera view matrix
     * @param projection Camera projection matrix
     */
    void    Render(const XMMATRIX& view, const XMMATRIX& projection) override;
};
