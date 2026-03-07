/**
 * @file RampObject.h
 * @brief Inclined ramp primitive game object for slopes and traversal surfaces
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides an inclined ramp (wedge) primitive useful for slopes, stairs
 * approximations, and physics-driven sliding tests. The ramp is defined by
 * its horizontal @p length and vertical @p height.
 *
 * Typical usage:
 * @code
 *   auto ramp = std::make_unique<RampObject>(4.0f, 2.0f);
 *   ramp->Initialize(device, context);
 *   ramp->SetPosition({10, 0, 0});
 * @endcode
 *
 * @see GameObject, LoadOrPlaceholderMesh
 */

#pragma once
#include "Core/Platform.h"

#include "Game/GameObject.h"
#include "Game/PlaceholderMesh.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

/**
 * @brief Inclined ramp (wedge) primitive for slopes and physics testing
 *
 * RampObject wraps a triangular-cross-section ramp mesh. The ramp rises from
 * ground level at the front to @p height at the back over a horizontal
 * distance of @p length. This makes it ideal for testing character controllers,
 * vehicle physics, and projectile trajectories on inclined surfaces.
 *
 * The mesh is loaded from @c Assets/Models/Ramp.obj when available;
 * otherwise a procedural placeholder is generated at initialization.
 *
 * @note The ramp's inclined surface faces the positive Y direction and extends
 *       along the positive Z-axis.
 * @see CubeObject, PlaneObject, SphereObject, PyramidObject, WallObject
 */
class RampObject : public GameObject
{
public:
    /**
     * @brief Construct a ramp with the given dimensions
     * @param length Horizontal extent of the ramp in world units (default: 2.0f)
     * @param height Vertical rise of the ramp in world units (default: 1.0f)
     */
    RampObject(float length = 2.0f, float height = 1.0f);

    /** @brief Default virtual destructor */
    ~RampObject() override = default;

    /**
     * @brief Initialize the ramp's mesh and DirectX resources
     *
     * Loads the ramp mesh from disk or falls back to a procedural shape via
     * LoadOrPlaceholderMesh(). Must be called before Update() or Render().
     *
     * @param device  DirectX 11 device for GPU resource creation
     * @param context DirectX 11 device context for rendering commands
     * @return S_OK on success, or an error HRESULT on failure
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Per-frame update — delegates to GameObject::Update
     * @param dt Delta time in seconds since the last frame
     */
    void    Update(float dt) override { GameObject::Update(dt); }

    /**
     * @brief Render the ramp — delegates to GameObject::Render
     * @param v Camera view matrix
     * @param p Camera projection matrix
     */
    void    Render(const DirectX::XMMATRIX& v, const DirectX::XMMATRIX& p) override
    {
        GameObject::Render(v, p);
    }

    /**
     * @brief Collision callback when hit by another game object (no-op)
     * @param target The other game object involved in the collision
     */
    void    OnHit(GameObject*) override {}

    /**
     * @brief Collision callback when hitting world geometry (no-op)
     * @param hitPoint World-space position of the collision
     * @param normal   Surface normal at the collision point
     */
    void    OnHitWorld(const DirectX::XMFLOAT3&, const DirectX::XMFLOAT3&) override {}

protected:
    /**
     * @brief Create or load the ramp mesh geometry
     *
     * Called during Initialize(). Uses LoadOrPlaceholderMesh() to attempt
     * loading from m_modelPath, falling back to a procedural shape.
     */
    void CreateMesh() override;

private:
    float        m_length;                                       ///< Horizontal extent of the ramp in world units
    float        m_height;                                       ///< Vertical rise of the ramp in world units
    std::wstring m_modelPath{ L"Assets/Models/Ramp.obj" };    ///< Path to the ramp OBJ model on disk
};
