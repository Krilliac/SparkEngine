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
