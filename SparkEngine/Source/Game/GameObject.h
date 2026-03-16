/**
 * @file GameObject.h
 * @brief Base class for all interactive objects in the game world
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides the fundamental interface and functionality for all objects
 * that can exist in the game world. It handles transformation, rendering, collision
 * callbacks, and basic object management. All concrete game objects should inherit
 * from this base class.
 */

#pragma once

#include "../Core/framework.h" // XMFLOAT3, XMMATRIX, HRESULT
#include "../Graphics/Mesh.h"
#include "../Utils/Assert.h"
#include <atomic>
#include <memory>
#include <string>

// Forward‐declare Projectile to avoid include cycles
class Projectile;

/**
 * @brief Base class for all game objects in the world
 * 
 * GameObject provides the fundamental interface and functionality required by all
 * interactive objects in the game world. This includes 3D transformation, mesh
 * rendering, collision detection callbacks, and basic object lifecycle management.
 * 
 * All concrete game objects (players, enemies, items, etc.) should inherit from
 * this class and implement the pure virtual collision callback methods.
 * 
 * @note This is an abstract class that cannot be instantiated directly
 * @warning Derived classes must implement OnHit() and OnHitWorld() methods
 */
class GameObject
{
  public:
    /**
     * @brief Default constructor
     * 
     * Initializes the game object with default transform values and assigns
     * a unique ID for object identification.
     */
    GameObject();

    /**
     * @brief Virtual destructor
     * 
     * Ensures proper cleanup when derived objects are destroyed polymorphically.
     * Automatically calls Shutdown() to clean up resources.
     */
    virtual ~GameObject();

    /**
     * @brief Initialize the game object with DirectX resources
     * 
     * Sets up the object for rendering by storing DirectX device references
     * and calling the virtual CreateMesh() method for mesh setup.
     * 
     * @param device DirectX 11 device for resource creation
     * @param context DirectX 11 device context for rendering operations
     * @return HRESULT indicating success or failure of initialization
     */
    virtual HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Update the game object for the current frame
     * 
     * Called once per frame to update object-specific logic, animations,
     * physics, or other time-dependent behavior.
     * 
     * @param deltaTime Time elapsed since the last frame in seconds
     */
    virtual void Update(float deltaTime);

    /**
     * @brief Render the game object to the screen
     * 
     * Renders the object's mesh using the current world transform and the
     * provided view and projection matrices.
     * 
     * @param view Camera view transformation matrix
     * @param projection Camera projection transformation matrix
     */
    virtual void Render(const XMMATRIX& view, const XMMATRIX& projection);

    /**
     * @brief Clean up all resources used by the game object
     * 
     * Releases mesh resources and resets the object state. Safe to call
     * multiple times.
     */
    virtual void Shutdown();

    /**
     * @brief Collision callback when this object hits another game object
     * 
     * Pure virtual method that must be implemented by derived classes to handle
     * collision events with other game objects.
     * 
     * @param target Pointer to the game object that was hit
     */
    virtual void OnHit(GameObject* target) = 0;

    /**
     * @brief Collision callback when this object hits the world geometry
     * 
     * Pure virtual method that must be implemented by derived classes to handle
     * collision events with static world geometry.
     * 
     * @param hitPoint World position where the collision occurred
     * @param normal Surface normal at the collision point
     */
    virtual void OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal) = 0;

    /**
     * @brief Set the world position of the object
     * @param pos New position in world coordinates
     */
    void SetPosition(const XMFLOAT3& pos);

    /**
     * @brief Set the rotation of the object
     * @param rot New rotation in Euler angles (radians)
     */
    void SetRotation(const XMFLOAT3& rot);

    /**
     * @brief Set the scale of the object
     * @param scale New scale factors for X, Y, and Z axes
     */
    void SetScale(const XMFLOAT3& scale);

    /**
     * @brief Move the object by a relative offset
     * @param delta Position offset to apply
     */
    void Translate(const XMFLOAT3& delta);

    /**
     * @brief Rotate the object by additional rotation
     * @param delta Rotation offset to apply (radians)
     */
    void Rotate(const XMFLOAT3& delta);

    /**
     * @brief Scale the object by additional factors
     * @param delta Scale factors to multiply with current scale
     */
    void Scale(const XMFLOAT3& delta);

    /**
     * @brief Get the current world position
     * @return Current position in world coordinates
     */
    const XMFLOAT3& GetPosition() const { return m_position; }

    /**
     * @brief Get the current rotation
     * @return Current rotation in Euler angles (radians)
     */
    const XMFLOAT3& GetRotation() const { return m_rotation; }

    /**
     * @brief Get the current scale
     * @return Current scale factors for X, Y, and Z axes
     */
    const XMFLOAT3& GetScale() const { return m_scale; }

    /**
     * @brief Get the world transformation matrix
     * 
     * Computes and returns the world matrix combining position, rotation, and scale.
     * The matrix is cached and only recomputed when the transform changes.
     * 
     * @return 4x4 world transformation matrix
     */
    XMMATRIX GetWorldMatrix() const;

    /**
     * @brief Get the object's forward direction vector
     * @return Normalized forward vector in world space
     */
    XMFLOAT3 GetForward() const;

    /**
     * @brief Get the object's right direction vector
     * @return Normalized right vector in world space
     */
    XMFLOAT3 GetRight() const;

    /**
     * @brief Get the object's up direction vector
     * @return Normalized up vector in world space
     */
    XMFLOAT3 GetUp() const;

    /**
     * @brief Check if the object is active
     * @return true if object is active and should be updated
     */
    bool IsActive() const { return m_active; }

    /**
     * @brief Check if the object is visible
     * @return true if object is visible and should be rendered
     */
    bool IsVisible() const { return m_visible; }

    /**
     * @brief Set the active state of the object
     * @param v true to activate, false to deactivate
     */
    void SetActive(bool v) { m_active = v; }

    /**
     * @brief Set the visibility state of the object
     * @param v true to make visible, false to hide
     */
    void SetVisible(bool v) { m_visible = v; }

    /**
     * @brief Get the unique identifier of the object
     * @return Unique ID assigned during construction
     */
    UINT GetID() const { return m_id; }

    /**
     * @brief Get the name of the object
     * @return Current object name string
     */
    const std::string& GetName() const { return m_name; }

    /**
     * @brief Set the name of the object
     * @param n New name for the object
     */
    void SetName(const std::string& n) { m_name = n; }

    /**
     * @brief Get the mesh associated with this object
     * @return Pointer to the object's mesh, or nullptr if no mesh is set
     */
    Mesh* GetMesh() const { return m_mesh.get(); }

    /**
     * @brief Calculate distance to another game object
     * @param o Other game object to measure distance to
     * @return Distance in world units
     */
    float GetDistanceFrom(const GameObject& o) const;

    /**
     * @brief Calculate distance to a world position
     * @param p World position to measure distance to
     * @return Distance in world units
     */
    float GetDistanceFrom(const XMFLOAT3& p) const;

  protected:
    /**
     * @brief Create or set up the mesh for this object
     * 
     * Virtual method that derived classes should override to create their
     * specific mesh geometry. Called during initialization.
     */
    virtual void CreateMesh();

    /**
     * @brief Update the cached world transformation matrix
     * 
     * Recalculates the world matrix from current position, rotation, and scale
     * values. Called automatically when transform properties change.
     */
    void UpdateWorldMatrix() const;

    // Transform state
    XMFLOAT3 m_position{};         ///< World position
    XMFLOAT3 m_rotation{};         ///< Rotation in Euler angles (radians)
    XMFLOAT3 m_scale{1, 1, 1};     ///< Scale factors for each axis
    mutable XMMATRIX m_worldMatrix{};      ///< Cached world transformation matrix
    mutable bool m_worldMatrixDirty{true}; ///< Flag indicating if world matrix needs recalculation

    // Rendering
    std::unique_ptr<Mesh> m_mesh;            ///< 3D mesh for rendering
    ID3D11Device* m_device{nullptr};         ///< DirectX device reference
    ID3D11DeviceContext* m_context{nullptr}; ///< DirectX context reference

    // Visibility/activation
    bool m_active{true};  ///< Whether object should be updated
    bool m_visible{true}; ///< Whether object should be rendered

    // Identification
    static std::atomic<UINT> s_nextID; ///< Static counter for unique ID generation
    UINT m_id{0};                      ///< Unique identifier for this object
    std::string m_name;                ///< Human-readable name for debugging

    /**
     * @brief Path to model file for mesh loading
     */
    std::wstring m_modelPath{}; // Path to model file for mesh loading

  private:
    /**
     * @brief Copy constructor (deleted)
     * 
     * GameObjects cannot be copied to prevent resource management issues.
     */
    GameObject(const GameObject&) = delete;

    /**
     * @brief Assignment operator (deleted)
     * 
     * GameObjects cannot be assigned to prevent resource management issues.
     */
    GameObject& operator=(const GameObject&) = delete;
};
