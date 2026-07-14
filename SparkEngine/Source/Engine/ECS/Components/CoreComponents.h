/**
 * @file CoreComponents.h
 * @brief Core ECS components: Transform, MeshRenderer, Camera, Script
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/OpaqueHandle.h"
#include "../../../Utils/Assert.h"
#include <entt/entt.hpp>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <functional>

using EntityID = entt::entity;

// =============================================================================
// NameComponent
// =============================================================================

/**
 * @brief Human-readable name for an entity, used in editor UI and debug logs.
 */
struct NameComponent
{
    std::string name; ///< Display name shown in the editor hierarchy and debug output.
};

// =============================================================================
// Transform
// =============================================================================

/**
 * @brief Spatial transform supporting hierarchical parent-child relationships.
 *
 * Every positioned entity has a Transform. When `parent` is set, the world
 * matrix is computed by walking the parent chain (local * parent.world).
 */
struct Transform
{
    DirectX::XMFLOAT3 position{0, 0, 0}; ///< Local-space position (meters).
    DirectX::XMFLOAT3 rotation{0, 0, 0}; ///< Euler angles in degrees (pitch, yaw, roll).
    DirectX::XMFLOAT3 scale{1, 1, 1};    ///< Non-uniform scale factors.
    EntityID parent = entt::null;        ///< Parent entity, or entt::null if root.
    std::vector<EntityID> children;      ///< Immediate children in the hierarchy.

    /**
     * @brief Compute the local matrix from position, rotation, and scale.
     * @return  The local-space transformation matrix (Scale * Rotation * Translation).
     */
    DirectX::XMMATRIX GetLocalMatrix() const;

    /**
     * @brief Compute the world matrix by walking the parent chain.
     *
     * If the transform has no parent (parent == entt::null), this returns
     * the local matrix. Otherwise it recursively multiplies with the
     * parent's world matrix: local * parent.GetWorldMatrix().
     *
     * @param registry  The EnTT registry used to look up parent Transform components.
     * @return          The world-space transformation matrix.
     */
    DirectX::XMMATRIX GetWorldMatrix(const entt::registry& registry) const;

    /**
     * @brief Compute the local matrix (legacy overload without registry).
     *
     * Returns the local matrix only (ignores parent hierarchy). Provided
     * for backward compatibility with code that does not need hierarchical
     * transforms.
     *
     * @return  The local-space transformation matrix.
     */
    DirectX::XMMATRIX GetWorldMatrix() const;
};

// =============================================================================
// MeshRenderer
// =============================================================================

/**
 * @brief Marks an entity for 3D mesh rendering via the deferred draw list.
 *
 * The RenderSystem reads this each frame and submits a MeshDrawCommand to
 * GraphicsEngine. The cached world matrix avoids redundant recalculation
 * when the transform hasn't changed.
 */
struct MeshRenderer
{
    std::string meshPath;                    ///< Asset path to the mesh resource (e.g. "models/crate.mesh").
    std::string materialPath;                ///< Asset path to the material definition (PBR or unlit).
    bool castShadows = true;                 ///< Include this mesh in the shadow map pass.
    bool receiveShadows = true;              ///< Apply shadow maps when shading this mesh.
    bool visible = true;                     ///< Runtime visibility toggle (false = skip rendering entirely).
    float emissive = 0.0f;                   ///< Unlit self-illumination (0 = off); glow strips, holo, warpgates.
    DirectX::XMFLOAT4X4 cachedWorldMatrix{}; ///< Last computed world matrix, updated by RenderSystem.
    bool worldMatrixDirty = true; ///< Cleared by RenderSystem after caching; other systems can set to force recompute.
};

// =============================================================================
// Camera
// =============================================================================

/**
 * @brief Perspective camera parameters used to build the projection matrix.
 */
struct Camera
{
    float fov = 60.0f;         ///< Vertical field of view in degrees.
    float nearPlane = 0.1f;    ///< Near clipping plane distance (meters).
    float farPlane = 1000.0f;  ///< Far clipping plane distance (meters).
    bool isMainCamera = false; ///< When true, this camera drives the main viewport.

    /**
     * @brief Validate that camera parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(fov > 0.0f && fov < 180.0f, "Camera FOV must be in (0, 180)");
        ASSERT_MSG(nearPlane > 0.0f, "Camera near plane must be positive");
        ASSERT_MSG(farPlane > nearPlane, "Camera far plane must exceed near plane");
        return fov > 0.0f && fov < 180.0f && nearPlane > 0.0f && farPlane > nearPlane;
    }
};

// =============================================================================
// Script
// =============================================================================

/**
 * @brief Binds an AngelScript class instance to an entity.
 *
 * The ScriptSystem locates the class in the named module, creates an instance,
 * and calls its Start()/Update() methods each frame while `enabled` is true.
 */
struct Script
{
    std::string scriptPath; ///< Filesystem path to the .as source file.
    std::string className;  ///< AngelScript class name to instantiate (e.g. "PlayerController").
    std::string moduleName; ///< Script module/context name (for hot-reload grouping).
    bool enabled = true;    ///< Whether the script receives Update() calls.
    bool started = false;   ///< Set to true after the first Start() call; prevents double-init.
};
