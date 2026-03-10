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
#include <entt/entt.hpp>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <functional>

using EntityID = entt::entity;

// =============================================================================
// NameComponent
// =============================================================================

struct NameComponent
{
    std::string name;
};

// =============================================================================
// Transform
// =============================================================================

struct Transform
{
    DirectX::XMFLOAT3 position{0, 0, 0};
    DirectX::XMFLOAT3 rotation{0, 0, 0};
    DirectX::XMFLOAT3 scale{1, 1, 1};
    EntityID parent = entt::null;
    std::vector<EntityID> children;

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

struct MeshRenderer
{
    std::string meshPath;
    std::string materialPath;
    bool castShadows = true;
    bool receiveShadows = true;
    bool visible = true;
    DirectX::XMFLOAT4X4 cachedWorldMatrix{};
    bool worldMatrixDirty = true;
};

// =============================================================================
// Camera
// =============================================================================

struct Camera
{
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool isMainCamera = false;
};

// =============================================================================
// Script
// =============================================================================

struct Script
{
    std::string scriptPath;
    std::string className;
    std::string moduleName;
    bool enabled = true;
    bool started = false;
};
