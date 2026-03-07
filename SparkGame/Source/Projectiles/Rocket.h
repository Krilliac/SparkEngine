/**
 * @file Rocket.h
 * @brief Rocket projectile with contact explosion and smoke trail effect
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a rocket projectile that travels in a straight line at moderate
 * speed and explodes on contact with any object or world geometry, dealing
 * area-of-effect damage. Rockets also emit a visible smoke trail that can be
 * used for particle rendering.
 *
 * Rockets are managed through the ProjectilePool for efficient reuse and should
 * not be allocated individually during gameplay.
 *
 * @code
 *   // Typically fired through the ProjectilePool:
 *   pool.FireRocket(launcherPosition, aimDirection, 100.0f);
 *
 *   // Trail positions can be queried for particle rendering:
 *   const auto& trail = rocket->GetTrailPositions();
 * @endcode
 *
 * @see Projectile, ProjectilePool, Bullet, Grenade
 */

#pragma once
#include "Core/Platform.h"

#include "Projectile.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief Contact-detonation rocket with area damage and visual smoke trail
 *
 * Rocket extends Projectile with contact-detonation mechanics, area-of-effect
 * explosion damage, and a smoke trail system that records the rocket's path
 * for visual effects rendering. Unlike grenades, rockets explode immediately
 * on impact rather than using a fuse timer.
 *
 * The trail positions are recorded at regular intervals controlled by
 * m_trailTimer and can be retrieved via GetTrailPositions() for rendering
 * smoke or particle effects along the rocket's flight path.
 *
 * @note Rockets travel in a straight line (no gravity by default) but at
 *       slower speed than bullets, making them visually trackable.
 * @warning The Explode() method should only be called once; m_hasExploded
 *          guards against double detonation.
 * @see Projectile, Bullet, Grenade, ProjectilePool
 */
class Rocket : public Projectile
{
public:
    /**
     * @brief Construct a rocket with default explosion and trail parameters
     *
     * Sets up rocket-specific defaults: moderate speed, high damage, large
     * explosion radius, and trail recording interval.
     */
    Rocket();

    /** @brief Default virtual destructor */
    ~Rocket() override = default;

    /**
     * @brief Initialize the rocket's mesh and DirectX resources
     *
     * Creates an elongated cylindrical mesh to represent the rocket visually.
     *
     * @param device  DirectX 11 device for GPU resource creation
     * @param context DirectX 11 device context for rendering commands
     * @return S_OK on success, or an error HRESULT on failure
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Update rocket position, trail recording, and collision detection
     *
     * Moves the rocket along its velocity vector, records trail positions at
     * regular intervals, and performs collision detection.
     *
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void    Update(float deltaTime) override;

    /**
     * @brief Render the rocket mesh
     * @param view       Camera view matrix
     * @param projection Camera projection matrix
     */
    void    Render(const XMMATRIX& view, const XMMATRIX& projection) override;

    /**
     * @brief Fire the rocket with initial velocity and clear trail state
     *
     * Overrides the base Fire() to reset the trail positions and explosion
     * state for a fresh launch.
     *
     * @param startPosition World position to launch from
     * @param direction     Normalized aim direction vector
     * @param speed         Initial speed in units per second
     */
    void    Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed) override;

    /**
     * @brief Handle collision with another game object — triggers explosion
     * @param target The game object that was hit
     */
    void OnHit(GameObject* target) override;

    /**
     * @brief Handle collision with world geometry — triggers explosion
     * @param hitPoint World-space position of the collision
     * @param normal   Surface normal at the collision point
     */
    void OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal) override;

    /**
     * @brief Get the recorded trail positions for particle/smoke rendering
     *
     * Returns a reference to the vector of world-space positions recorded
     * along the rocket's flight path. Useful for rendering smoke trails or
     * exhaust particle effects.
     *
     * @return Const reference to the trail position history
     */
    const std::vector<XMFLOAT3>& GetTrailPositions() const { return m_trailPositions; }

private:
    /**
     * @brief Trigger the rocket explosion at the given position
     *
     * Deals area-of-effect damage to all objects within m_explosionRadius
     * of the explosion center, spawns visual effects, and deactivates the rocket.
     *
     * @param position World-space center of the explosion
     */
    void Explode(const XMFLOAT3& position);

    float m_explosionRadius;              ///< Radius of the explosion's area-of-effect damage
    bool  m_hasExploded;                  ///< Guard flag to prevent double detonation

    // Trail effect state
    std::vector<XMFLOAT3> m_trailPositions;  ///< Recorded positions along the flight path for trail rendering
    float m_trailTimer;                       ///< Timer controlling trail position recording interval
};
