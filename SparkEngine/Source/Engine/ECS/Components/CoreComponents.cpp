// CoreComponents.cpp
#include "../../../Core/Platform.h"
#include "CoreComponents.h"

using namespace DirectX;

// ============================================================================
// Transform
// ============================================================================

XMMATRIX Transform::GetLocalMatrix() const
{
    XMMATRIX scaleMtx = XMMatrixScaling(scale.x, scale.y, scale.z);
    XMMATRIX rotMtx = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(rotation.x),
        XMConvertToRadians(rotation.y),
        XMConvertToRadians(rotation.z));
    XMMATRIX transMtx = XMMatrixTranslation(position.x, position.y, position.z);
    return scaleMtx * rotMtx * transMtx;
}

XMMATRIX Transform::GetWorldMatrix(const entt::registry& registry) const
{
    XMMATRIX localMtx = GetLocalMatrix();

    if (parent == entt::null)
    {
        return localMtx;
    }

    // Walk up the parent chain to accumulate world transforms.
    // Guard against cycles with a maximum depth.
    constexpr int kMaxHierarchyDepth = 64;
    int depth = 0;

    XMMATRIX worldMtx = localMtx;
    EntityID current = parent;

    while (current != entt::null && depth < kMaxHierarchyDepth)
    {
        const auto* parentTransform = registry.try_get<Transform>(current);
        if (!parentTransform)
        {
            break;
        }

        XMMATRIX parentLocal = parentTransform->GetLocalMatrix();
        worldMtx = worldMtx * parentLocal;
        current = parentTransform->parent;
        ++depth;
    }

    return worldMtx;
}

XMMATRIX Transform::GetWorldMatrix() const
{
    // Legacy overload: returns local matrix only (no hierarchy).
    return GetLocalMatrix();
}
