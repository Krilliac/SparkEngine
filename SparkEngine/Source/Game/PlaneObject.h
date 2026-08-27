/**
 * @file PlaneObject.h
 * @brief Engine-owned rectangular plane primitive for ground surfaces and level design
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a horizontal plane primitive that can serve as a ground surface,
 * platform, or any other flat rectangular area in the game world. The plane
 * is defined by its width (X-axis) and depth (Z-axis) and is centered at the
 * object's position.
 *
 * Typical usage:
 * @code
 *   auto ground = std::make_unique<PlaneObject>(50.0f, 50.0f);
 *   ground->Initialize(device, context);
 *   ground->SetPosition({0, 0, 0});
 * @endcode
 *
 * @see GameObject, Primitives::CreatePlane, LoadOrPlaceholderMesh
 */

#pragma once
#include "Core/Platform.h"

#include "Game/GameObject.h"
#include "Game/PlaceholderMesh.h"
#include "Game/Primitives.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief Flat rectangular plane primitive for floors, platforms, and surfaces
 *
 * PlaneObject is a lightweight GameObject specialization that wraps a flat
 * rectangular mesh oriented along the XZ-plane. It is commonly used for
 * ground surfaces, platforms, and other horizontal areas during prototyping.
 *
 * The plane mesh is loaded from @c Assets/Models/Plane.obj when available;
 * otherwise a procedural plane placeholder is generated at initialization.
 *
 * @note The plane's normal faces upward along the positive Y-axis.
 *       UV coordinates tile once across the entire surface.
 * @see CubeObject, SphereObject, PyramidObject, RampObject, WallObject
 */
class PlaneObject : public GameObject
{
  public:
    /**
     * @brief Construct a plane with the given dimensions
     * @param width  Extent along the X-axis in world units (default: 10.0f)
     * @param depth  Extent along the Z-axis in world units (default: 10.0f)
     */
    PlaneObject(float width = 10.0f, float depth = 10.0f);

    /** @brief Default virtual destructor */
    ~PlaneObject() override = default;

    /**
     * @brief Initialize the plane's mesh and DirectX resources
     *
     * Loads the plane mesh from disk or falls back to a procedural plane via
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
    void Update(float dt) override { GameObject::Update(dt); }

    /**
     * @brief Render the plane — delegates to GameObject::Render
     * @param v Camera view matrix
     * @param p Camera projection matrix
     */
    void Render(const XMMATRIX& v, const XMMATRIX& p) override { GameObject::Render(v, p); }

    /**
     * @brief Collision callback when hit by another game object (no-op)
     *
     * Basic planes do not react to collisions with other objects.
     *
     * @param target The other game object involved in the collision
     */
    void OnHit(GameObject*) override {}

    /**
     * @brief Collision callback when hitting world geometry (no-op)
     * @param hitPoint World-space position of the collision
     * @param normal   Surface normal at the collision point
     */
    void OnHitWorld(const XMFLOAT3&, const XMFLOAT3&) override {}

  protected:
    /**
     * @brief Create or load the plane mesh geometry
     *
     * Called during Initialize(). Uses LoadOrPlaceholderMesh() to attempt
     * loading from m_modelPath, falling back to a procedural plane.
     */
    void CreateMesh() override;

  private:
    float m_width;                                        ///< Width of the plane along the X-axis
    float m_depth;                                        ///< Depth of the plane along the Z-axis
    std::wstring m_modelPath{L"Assets/Models/Plane.obj"}; ///< Path to the plane OBJ model on disk
};
