/**
 * @file GizmoSystem.h
 * @brief 3D object manipulation gizmo system for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a professional 3D gizmo system for object manipulation,
 * supporting translation, rotation, and scaling operations with visual feedback
 * and precise interaction handling.
 */

#pragma once

#include "../SceneSystem/SceneFile.h"
#include <DirectXMath.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <memory>

using namespace DirectX;

namespace SparkEditor {

/**
 * @brief Gizmo operation modes
 */
enum class GizmoMode {
    TRANSLATE = 0,      ///< Translation gizmo (move objects)
    ROTATE = 1,         ///< Rotation gizmo (rotate objects)
    SCALE = 2,          ///< Scale gizmo (scale objects)
    UNIVERSAL = 3       ///< Universal gizmo (all operations)
};

/**
 * @brief Gizmo coordinate space
 */
enum class GizmoSpace {
    WORLD = 0,          ///< World coordinate space
    LOCAL = 1           ///< Local coordinate space
};

/**
 * @brief Gizmo axis identification
 */
enum class GizmoAxis {
    NONE = 0,
    X = 1,              ///< X-axis (red)
    Y = 2,              ///< Y-axis (green)
    Z = 4,              ///< Z-axis (blue)
    XY = 3,             ///< XY plane (X | Y)
    XZ = 5,             ///< XZ plane (X | Z)
    YZ = 6,             ///< YZ plane (Y | Z)
    XYZ = 7,            ///< All axes (X | Y | Z)
    SCREEN = 8          ///< Screen space manipulation
};

/**
 * @brief Ray structure for mouse picking
 */
struct Ray {
    XMFLOAT3 origin;    ///< Ray origin point
    XMFLOAT3 direction; ///< Ray direction (normalized)
    
    /**
     * @brief Create ray from screen coordinates
     * @param screenX Screen X coordinate
     * @param screenY Screen Y coordinate
     * @param viewMatrix View matrix
     * @param projMatrix Projection matrix
     * @param viewport Viewport dimensions
     * @return Ray in world space
     */
    static Ray ScreenToWorldRay(float screenX, float screenY, 
                               const XMMATRIX& viewMatrix, 
                               const XMMATRIX& projMatrix,
                               const XMFLOAT4& viewport);
};

/**
 * @brief Gizmo interaction result
 */
struct GizmoInteraction {
    bool isActive = false;          ///< Whether interaction is active
    GizmoAxis activeAxis = GizmoAxis::NONE; ///< Currently active axis
    XMFLOAT3 startPosition = {0, 0, 0}; ///< Starting position of interaction
    XMFLOAT3 currentDelta = {0, 0, 0};  ///< Current delta from start
    float totalDelta = 0.0f;        ///< Total magnitude of change
    bool isDragging = false;        ///< Whether currently dragging
};

/**
 * @brief Professional 3D gizmo system
 * 
 * Provides industry-standard 3D object manipulation tools with:
 * - Translation gizmos with axis constraints and plane manipulation
 * - Rotation gizmos with arc visualization and snap-to-angle
 * - Scale gizmos with uniform and non-uniform scaling
 * - Multi-object editing support
 * - Precise snapping and grid alignment
 * - Visual feedback and highlight states
 * - Undo/redo integration
 * 
 * Inspired by Unity, Maya, 3ds Max, and Blender manipulation tools.
 */
class GizmoSystem {
public:
    /**
     * @brief Constructor
     */
    GizmoSystem();

    /**
     * @brief Destructor
     */
    ~GizmoSystem();

    /**
     * @brief Initialize the gizmo system
     * @param device DirectX device for rendering
     * @param context DirectX context for rendering
     * @return true if initialization succeeded
     */
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the gizmo system
     */
    void Shutdown();

    /**
     * @brief Update gizmo system
     * @param deltaTime Time elapsed since last update
     */
    void Update(float deltaTime);

    /**
     * @brief Render gizmos for selected objects
     * @param selectedObjects List of selected object transforms
     * @param viewMatrix Camera view matrix
     * @param projMatrix Camera projection matrix
     * @param viewport Viewport dimensions
     */
    void Render(const std::vector<Transform*>& selectedObjects,
               const XMMATRIX& viewMatrix,
               const XMMATRIX& projMatrix,
               const XMFLOAT4& viewport);

    /**
     * @brief Handle mouse input for gizmo interaction
     * @param mouseX Screen mouse X coordinate
     * @param mouseY Screen mouse Y coordinate
     * @param isMouseDown Whether mouse button is pressed
     * @param viewMatrix Camera view matrix
     * @param projMatrix Camera projection matrix
     * @param viewport Viewport dimensions
     * @param selectedObjects Objects being manipulated
     * @return true if gizmo handled the input
     */
    bool HandleMouseInput(float mouseX, float mouseY, bool isMouseDown,
                         const XMMATRIX& viewMatrix,
                         const XMMATRIX& projMatrix,
                         const XMFLOAT4& viewport,
                         std::vector<Transform*>& selectedObjects);

    /**
     * @brief Set current gizmo mode
     * @param mode Gizmo operation mode
     */
    void SetGizmoMode(GizmoMode mode) { m_currentMode = mode; }

    /**
     * @brief Get current gizmo mode
     * @return Current gizmo mode
     */
    GizmoMode GetGizmoMode() const { return m_currentMode; }

    /**
     * @brief Set gizmo coordinate space
     * @param space Coordinate space (world or local)
     */
    void SetGizmoSpace(GizmoSpace space) { m_coordinateSpace = space; }

    /**
     * @brief Get gizmo coordinate space
     * @return Current coordinate space
     */
    GizmoSpace GetGizmoSpace() const { return m_coordinateSpace; }

    /**
     * @brief Enable/disable grid snapping
     * @param enabled Whether grid snapping is enabled
     */
    void SetSnapToGrid(bool enabled) { m_snapToGrid = enabled; }

    /**
     * @brief Check if grid snapping is enabled
     * @return true if grid snapping is enabled
     */
    bool IsSnapToGrid() const { return m_snapToGrid; }

    /**
     * @brief Set grid snap size
     * @param snapSize Grid snap size in world units
     */
    void SetSnapSize(float snapSize) { m_snapSize = snapSize; }

    /**
     * @brief Get grid snap size
     * @return Current grid snap size
     */
    float GetSnapSize() const { return m_snapSize; }

    /**
     * @brief Set rotation snap angle
     * @param snapAngle Rotation snap angle in degrees
     */
    void SetRotationSnapAngle(float snapAngle) { m_rotationSnapAngle = snapAngle; }

    /**
     * @brief Get rotation snap angle
     * @return Current rotation snap angle in degrees
     */
    float GetRotationSnapAngle() const { return m_rotationSnapAngle; }

    /**
     * @brief Enable/disable gizmo visibility
     * @param visible Whether gizmos should be visible
     */
    void SetVisible(bool visible) { m_isVisible = visible; }

    /**
     * @brief Check if gizmos are visible
     * @return true if gizmos are visible
     */
    bool IsVisible() const { return m_isVisible; }

    /**
     * @brief Set gizmo size scale
     * @param scale Size scale multiplier
     */
    void SetGizmoSize(float scale) { m_gizmoSize = scale; }

    /**
     * @brief Get gizmo size scale
     * @return Current gizmo size scale
     */
    float GetGizmoSize() const { return m_gizmoSize; }

    /**
     * @brief Get current interaction state
     * @return Current gizmo interaction information
     */
    const GizmoInteraction& GetInteraction() const { return m_interaction; }

    /**
     * @brief Check if any gizmo is currently being interacted with
     * @return true if gizmo interaction is active
     */
    bool IsInteracting() const { return m_interaction.isActive; }

private:
    /**
     * @brief Render translation gizmo
     * @param transform Object transform
     * @param viewMatrix View matrix
     * @param projMatrix Projection matrix
     */
    void RenderTranslationGizmo(const Transform& transform,
                               const XMMATRIX& viewMatrix,
                               const XMMATRIX& projMatrix);

    /**
     * @brief Render rotation gizmo
     * @param transform Object transform
     * @param viewMatrix View matrix
     * @param projMatrix Projection matrix
     */
    void RenderRotationGizmo(const Transform& transform,
                           const XMMATRIX& viewMatrix,
                           const XMMATRIX& projMatrix);

    /**
     * @brief Render scale gizmo
     * @param transform Object transform
     * @param viewMatrix View matrix
     * @param projMatrix Projection matrix
     */
    void RenderScaleGizmo(const Transform& transform,
                        const XMMATRIX& viewMatrix,
                        const XMMATRIX& projMatrix);

    /**
     * @brief Test ray intersection with translation gizmo
     * @param ray Mouse ray in world space
     * @param transform Object transform
     * @return Hit axis or NONE
     */
    GizmoAxis TestTranslationGizmoHit(const Ray& ray, const Transform& transform);

    /**
     * @brief Test ray intersection with rotation gizmo
     * @param ray Mouse ray in world space
     * @param transform Object transform
     * @return Hit axis or NONE
     */
    GizmoAxis TestRotationGizmoHit(const Ray& ray, const Transform& transform);

    /**
     * @brief Test ray intersection with scale gizmo
     * @param ray Mouse ray in world space
     * @param transform Object transform
     * @return Hit axis or NONE
     */
    GizmoAxis TestScaleGizmoHit(const Ray& ray, const Transform& transform);

    /**
     * @brief Apply translation delta to transforms
     * @param delta Translation delta
     * @param transforms Objects to transform
     */
    void ApplyTranslation(const XMFLOAT3& delta, std::vector<Transform*>& transforms);

    /**
     * @brief Apply rotation delta to transforms
     * @param axis Rotation axis
     * @param angleDelta Rotation angle delta in radians
     * @param transforms Objects to transform
     */
    void ApplyRotation(GizmoAxis axis, float angleDelta, std::vector<Transform*>& transforms);

    /**
     * @brief Apply scale delta to transforms
     * @param scale Scale multiplier
     * @param axis Scale axis constraint
     * @param transforms Objects to transform
     */
    void ApplyScale(const XMFLOAT3& scale, GizmoAxis axis, std::vector<Transform*>& transforms);

    /**
     * @brief Snap value to grid
     * @param value Input value
     * @return Snapped value
     */
    float SnapToGrid(float value) const;

    /**
     * @brief Snap angle to rotation increment
     * @param angle Input angle in radians
     * @return Snapped angle in radians
     */
    float SnapToRotation(float angle) const;

    /**
     * @brief Calculate gizmo center from multiple transforms
     * @param transforms List of transforms
     * @return Center position
     */
    XMFLOAT3 CalculateGizmoCenter(const std::vector<Transform*>& transforms) const;

    /**
     * @brief Calculate adaptive gizmo size based on camera distance
     * @param gizmoPosition Gizmo world position
     * @param viewMatrix Camera view matrix
     * @return Adaptive gizmo size
     */
    float CalculateAdaptiveSize(const XMFLOAT3& gizmoPosition, const XMMATRIX& viewMatrix) const;

    /**
     * @brief Get color for gizmo axis
     * @param axis Gizmo axis
     * @param isHighlighted Whether axis is highlighted
     * @param isSelected Whether axis is selected
     * @return RGBA color
     */
    XMFLOAT4 GetAxisColor(GizmoAxis axis, bool isHighlighted, bool isSelected) const;

    /**
     * @brief Create gizmo geometry
     * @return true if geometry creation succeeded
     */
    bool CreateGizmoGeometry();

    /**
     * @brief Create gizmo shaders
     * @return true if shader creation succeeded
     */
    bool CreateGizmoShaders();

private:
    // DirectX resources
    ID3D11Device* m_device = nullptr;           ///< DirectX device
    ID3D11DeviceContext* m_context = nullptr;   ///< DirectX context
    
    // Gizmo state
    GizmoMode m_currentMode = GizmoMode::TRANSLATE; ///< Current gizmo mode
    GizmoSpace m_coordinateSpace = GizmoSpace::WORLD; ///< Coordinate space
    GizmoInteraction m_interaction;              ///< Current interaction state
    
    // Gizmo settings
    bool m_isVisible = true;                    ///< Gizmo visibility
    bool m_snapToGrid = false;                  ///< Grid snapping enabled
    float m_snapSize = 1.0f;                    ///< Grid snap size
    float m_rotationSnapAngle = 15.0f;          ///< Rotation snap angle (degrees)
    float m_gizmoSize = 1.0f;                   ///< Gizmo size scale
    
    // Interaction state
    XMFLOAT3 m_lastMouseWorldPos = {0, 0, 0};    ///< Last mouse world position
    XMFLOAT3 m_interactionStartPos = {0, 0, 0}; ///< Interaction start position
    bool m_isDragging = false;                  ///< Currently dragging
    GizmoAxis m_hoveredAxis = GizmoAxis::NONE;  ///< Currently hovered axis
    
    // Rendering resources
    struct GizmoGeometry {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
    };
    
    GizmoGeometry m_arrowGeometry;              ///< Arrow geometry for translation
    GizmoGeometry m_ringGeometry;               ///< Ring geometry for rotation
    GizmoGeometry m_cubeGeometry;               ///< Cube geometry for scale
    GizmoGeometry m_lineGeometry;               ///< Line geometry for axes
    
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;   ///< Gizmo vertex shader
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;     ///< Gizmo pixel shader
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;     ///< Vertex input layout
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;       ///< Shader constants
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState; ///< Rasterizer state
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState; ///< Depth stencil state
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;       ///< Blend state
    
    // Shader constants
    struct GizmoConstants {
        XMMATRIX worldViewProj;
        XMFLOAT4 color;
        float gizmoSize;
        float padding[3];
    };
    
    // Colors
    static constexpr XMFLOAT4 COLOR_X_AXIS = {1.0f, 0.3f, 0.3f, 1.0f};     ///< X-axis color (red)
    static constexpr XMFLOAT4 COLOR_Y_AXIS = {0.3f, 1.0f, 0.3f, 1.0f};     ///< Y-axis color (green)
    static constexpr XMFLOAT4 COLOR_Z_AXIS = {0.3f, 0.3f, 1.0f, 1.0f};     ///< Z-axis color (blue)
    static constexpr XMFLOAT4 COLOR_SELECTED = {1.0f, 1.0f, 0.0f, 1.0f};   ///< Selected color (yellow)
    static constexpr XMFLOAT4 COLOR_HIGHLIGHTED = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Highlighted color (white)
};

} // namespace SparkEditor