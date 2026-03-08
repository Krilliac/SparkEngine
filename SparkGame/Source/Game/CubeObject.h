/**
 * @file CubeObject.h
 * @brief Axis-aligned cube primitive game object for prototyping and level design
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a ready-to-use cube primitive that inherits from GameObject. The cube
 * attempts to load its mesh from an OBJ file on disk; if the file is missing or
 * corrupt, the LoadOrPlaceholderMesh() helper automatically generates a procedural
 * cube fallback so the object is always renderable.
 *
 * Typical usage:
 * @code
 *   auto cube = std::make_unique<CubeObject>(2.0f);  // 2-unit cube
 *   cube->Initialize(device, context);
 *   cube->SetPosition({0, 1, 0});
 * @endcode
 *
 * @see GameObject, Primitives::CreateCube, LoadOrPlaceholderMesh
 */

#pragma once
#include "Core/Platform.h"

#include "Game/GameObject.h"
#include "Game/PlaceholderMesh.h"
#include "Game/Primitives.h"
#include "Utils/Assert.h" // custom assert system
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief Axis-aligned cube primitive for prototyping and level blocking
 *
 * CubeObject is a lightweight GameObject specialization that wraps a unit or
 * custom-sized cube mesh. It delegates all transform, rendering, and lifecycle
 * logic to the base GameObject class and adds no additional gameplay behavior
 * (OnHit / OnHitWorld are intentional no-ops).
 *
 * The cube's visual representation is loaded from @c Assets/Models/Cube.obj
 * when available; otherwise a procedural cube placeholder is generated at
 * initialization time via LoadOrPlaceholderMesh().
 *
 * @note The cube is centered at the object's position with equal extents in
 *       all three axes determined by the @p size parameter.
 * @see PlaneObject, SphereObject, PyramidObject, RampObject, WallObject
 */
class CubeObject : public GameObject
{
  public:
    /**
     * @brief Construct a cube with the given edge length
     * @param size Edge length of the cube in world units (default: 1.0f)
     */
    explicit CubeObject(float size = 1.0f);

    /** @brief Default virtual destructor */
    ~CubeObject() override = default;

    /**
     * @brief Initialize the cube's mesh and DirectX resources
     *
     * Loads the cube mesh from disk or falls back to a procedural cube via
     * LoadOrPlaceholderMesh(). Must be called before Update() or Render().
     *
     * @param device  DirectX 11 device used for GPU resource creation
     * @param context DirectX 11 device context for immediate rendering commands
     * @return S_OK on success, or an error HRESULT if initialization fails
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Per-frame update — delegates to GameObject::Update
     * @param dt Delta time in seconds since the last frame
     */
    void Update(float dt) override { GameObject::Update(dt); }

    /**
     * @brief Render the cube — delegates to GameObject::Render
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
     * @brief Create or load the cube mesh geometry
     *
     * Called during Initialize(). Uses LoadOrPlaceholderMesh() to attempt
     * loading from m_modelPath, falling back to a procedural cube.
     */
    void CreateMesh() override;

  private:
    float m_size;                                         ///< Edge length of the cube in world units
    std::wstring m_modelPath = L"Assets/Models/Cube.obj"; ///< Path to the cube OBJ model on disk
};
