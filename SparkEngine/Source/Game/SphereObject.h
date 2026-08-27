/**
 * @file SphereObject.h
 * @brief Engine-owned UV-sphere primitive for prototyping and physics testing
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a sphere primitive whose tessellation level is controlled by the
 * @p slices (longitude) and @p stacks (latitude) parameters. Like all primitive
 * objects, it attempts to load from an OBJ file first and falls back to a
 * procedural mesh when the file is unavailable.
 *
 * Typical usage:
 * @code
 *   auto ball = std::make_unique<SphereObject>(0.5f, 32, 32);
 *   ball->Initialize(device, context);
 *   ball->SetPosition({3, 2, 0});
 * @endcode
 *
 * @see GameObject, Primitives::CreateSphere, LoadOrPlaceholderMesh
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
 * @brief UV-sphere primitive for physics testing, decoration, and prototyping
 *
 * SphereObject wraps a latitude/longitude (UV) sphere mesh whose smoothness
 * is determined by the @p slices and @p stacks parameters supplied at
 * construction time. Higher values produce a smoother sphere at the cost of
 * additional triangles.
 *
 * The sphere mesh is loaded from @c Assets/Models/Sphere.obj when available;
 * otherwise a procedural sphere placeholder is generated at initialization.
 *
 * @note The sphere is centered at the object's position with a uniform radius.
 *       Texture coordinates wrap once around the sphere using spherical UV mapping.
 * @see CubeObject, PlaneObject, PyramidObject, RampObject, WallObject
 */
class SphereObject : public GameObject
{
  public:
    /**
     * @brief Construct a sphere with the given radius and tessellation
     * @param radius Radius of the sphere in world units (default: 1.0f)
     * @param slices Number of vertical slices / longitude divisions (default: 20)
     * @param stacks Number of horizontal stacks / latitude divisions (default: 20)
     */
    SphereObject(float radius = 1.0f, int slices = 20, int stacks = 20);

    /** @brief Default virtual destructor */
    ~SphereObject() override = default;

    /**
     * @brief Initialize the sphere's mesh and DirectX resources
     *
     * Loads the sphere mesh from disk or falls back to a procedural sphere via
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
     * @brief Render the sphere — delegates to GameObject::Render
     * @param v Camera view matrix
     * @param p Camera projection matrix
     */
    void Render(const XMMATRIX& v, const XMMATRIX& p) override { GameObject::Render(v, p); }

    /**
     * @brief Collision callback when hit by another game object (no-op)
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
     * @brief Create or load the sphere mesh geometry
     *
     * Called during Initialize(). Uses LoadOrPlaceholderMesh() to attempt
     * loading from m_modelPath, falling back to a procedural sphere.
     */
    void CreateMesh() override;

  private:
    float m_radius;                                        ///< Sphere radius in world units
    int m_slices;                                          ///< Number of longitude divisions (vertical slices)
    int m_stacks;                                          ///< Number of latitude divisions (horizontal stacks)
    std::wstring m_modelPath{L"Assets/Models/Sphere.obj"}; ///< Path to the sphere OBJ model on disk
};
