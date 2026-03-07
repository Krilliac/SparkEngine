/**
 * @file PyramidObject.h
 * @brief Four-sided pyramid primitive game object for decorative and test geometry
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a square-base pyramid primitive that can be used for decorative
 * elements, physics tests, or level-design blocking. The pyramid's base size
 * is configurable at construction time.
 *
 * Typical usage:
 * @code
 *   auto pyramid = std::make_unique<PyramidObject>(3.0f);
 *   pyramid->Initialize(device, context);
 *   pyramid->SetPosition({5, 0, 5});
 * @endcode
 *
 * @see GameObject, LoadOrPlaceholderMesh
 */

#pragma once
#include "Core/Platform.h"

#include "Game/GameObject.h"
#include "PlaceholderMesh.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

/**
 * @brief Four-sided pyramid (square base) primitive game object
 *
 * PyramidObject wraps a pyramid mesh with a square base. The @p size parameter
 * controls the base edge length; the pyramid height equals the base size by
 * default (the aspect ratio is baked into the OBJ or procedural fallback).
 *
 * The mesh is loaded from @c Assets/Models/Pyramid.obj when available;
 * otherwise a procedural placeholder is generated at initialization.
 *
 * @note The apex of the pyramid is aligned with the positive Y-axis.
 * @see CubeObject, PlaneObject, SphereObject, RampObject, WallObject
 */
class PyramidObject : public GameObject
{
public:
    /**
     * @brief Construct a pyramid with the given base size
     * @param size Base edge length in world units (default: 1.0f)
     */
    PyramidObject(float size = 1.0f);

    /** @brief Default virtual destructor */
    ~PyramidObject() override = default;

    /**
     * @brief Initialize the pyramid's mesh and DirectX resources
     *
     * Loads the pyramid mesh from disk or falls back to a procedural shape via
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
     * @brief Render the pyramid — delegates to GameObject::Render
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
     * @brief Create or load the pyramid mesh geometry
     *
     * Called during Initialize(). Uses LoadOrPlaceholderMesh() to attempt
     * loading from m_modelPath, falling back to a procedural shape.
     */
    void CreateMesh() override;

private:
    float        m_size;                                            ///< Base edge length in world units
    std::wstring m_modelPath{ L"Assets/Models/Pyramid.obj" };    ///< Path to the pyramid OBJ model on disk
};
