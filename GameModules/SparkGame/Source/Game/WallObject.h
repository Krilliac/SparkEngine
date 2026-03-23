/**
 * @file WallObject.h
 * @brief Vertical wall primitive game object for barriers and level boundaries
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a vertical wall (thin box) primitive used for barriers, room
 * boundaries, and cover geometry during level design and prototyping. The
 * wall is defined by its @p width (X-axis) and @p height (Y-axis) and has a
 * minimal depth to keep it visually thin.
 *
 * Typical usage:
 * @code
 *   auto wall = std::make_unique<WallObject>(8.0f, 4.0f);
 *   wall->Initialize(device, context);
 *   wall->SetPosition({0, 2, -10});
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
 * @brief Vertical wall (thin box) primitive for barriers and boundaries
 *
 * WallObject wraps a thin rectangular mesh oriented vertically. It is
 * commonly used for blocking player movement, defining room boundaries,
 * and providing cover in FPS gameplay tests.
 *
 * The mesh is loaded from @c Assets/Models/Wall.obj when available;
 * otherwise a procedural placeholder is generated at initialization.
 *
 * @note The wall's front face is aligned with the XY-plane with the surface
 *       normal pointing along the positive Z-axis.
 * @see CubeObject, PlaneObject, SphereObject, PyramidObject, RampObject
 */
class WallObject : public GameObject
{
  public:
    /**
     * @brief Construct a wall with the given dimensions
     * @param width  Horizontal extent of the wall in world units (default: 5.0f)
     * @param height Vertical extent of the wall in world units (default: 3.0f)
     */
    WallObject(float width = 5.0f, float height = 3.0f);

    /** @brief Default virtual destructor */
    ~WallObject() override = default;

    /**
     * @brief Initialize the wall's mesh and DirectX resources
     *
     * Loads the wall mesh from disk or falls back to a procedural shape via
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
     * @brief Render the wall — delegates to GameObject::Render
     * @param v Camera view matrix
     * @param p Camera projection matrix
     */
    void Render(const DirectX::XMMATRIX& v, const DirectX::XMMATRIX& p) override { GameObject::Render(v, p); }

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
    void OnHitWorld(const DirectX::XMFLOAT3&, const DirectX::XMFLOAT3&) override {}

  protected:
    /**
     * @brief Create or load the wall mesh geometry
     *
     * Called during Initialize(). Uses LoadOrPlaceholderMesh() to attempt
     * loading from m_modelPath, falling back to a procedural shape.
     */
    void CreateMesh() override;

  private:
    float m_width;                                       ///< Horizontal extent of the wall in world units
    float m_height;                                      ///< Vertical extent of the wall in world units
    std::wstring m_modelPath{L"Assets/Models/Wall.obj"}; ///< Path to the wall OBJ model on disk
};
