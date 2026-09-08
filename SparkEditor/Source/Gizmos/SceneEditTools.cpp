/**
 * @file SceneEditTools.cpp
 * @brief Implementation of the undoable scene-manipulation helpers (W9)
 * @author Spark Engine Team
 * @date 2026
 */

#include "SceneEditTools.h"
#include "../CommandHistory.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace DirectX;

namespace SparkEditor::SceneEditTools
{

    namespace
    {
        /// Matches the cycle guard used by ::Transform::GetWorldMatrix().
        constexpr int kMaxHierarchyDepth = 64;


        /// True when candidate == ancestor or candidate sits anywhere below
        /// ancestor in the Transform hierarchy.
        bool IsSelfOrDescendantOf(const entt::registry& registry, ::EntityID candidate, ::EntityID ancestor)
        {
            int depth = 0;
            ::EntityID current = candidate;
            while (current != entt::null && depth < kMaxHierarchyDepth)
            {
                if (current == ancestor)
                {
                    return true;
                }
                const ::Transform* transform = registry.try_get<::Transform>(current);
                if (!transform)
                {
                    break;
                }
                current = transform->parent;
                ++depth;
            }
            return false;
        }

        XMFLOAT3 GetWorldPosition(const entt::registry& registry, const ::Transform& transform)
        {
            const XMVECTOR position = XMVector3TransformCoord(XMVectorZero(), transform.GetWorldMatrix(registry));
            XMFLOAT3 out;
            XMStoreFloat3(&out, position);
            return out;
        }
    } // namespace


    // ========================================================================
    // Single-value undoable commits (viewport gizmos, hierarchy rename)
    // ========================================================================

    bool CommitEntityRotation(::World& world, ::EntityID entity, const XMFLOAT3& oldRotation,
                              const XMFLOAT3& newRotation)
    {
        entt::registry& registry = world.GetRegistry();
        if (entity == entt::null || !registry.valid(entity) || !registry.try_get<::Transform>(entity))
        {
            return false;
        }
        if (oldRotation.x == newRotation.x && oldRotation.y == newRotation.y && oldRotation.z == newRotation.z)
        {
            return false;
        }

        ::World* worldPtr = &world;
        Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [worldPtr, entity, newRotation]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (reg.valid(entity))
                {
                    if (::Transform* transform = reg.try_get<::Transform>(entity))
                        transform->rotation = newRotation;
                }
            },
            [worldPtr, entity, oldRotation]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (reg.valid(entity))
                {
                    if (::Transform* transform = reg.try_get<::Transform>(entity))
                        transform->rotation = oldRotation;
                }
            },
            "Rotate Entity"));
        return true;
    }

    bool CommitEntityScale(::World& world, ::EntityID entity, const XMFLOAT3& oldScale, const XMFLOAT3& newScale)
    {
        entt::registry& registry = world.GetRegistry();
        if (entity == entt::null || !registry.valid(entity) || !registry.try_get<::Transform>(entity))
        {
            return false;
        }
        if (oldScale.x == newScale.x && oldScale.y == newScale.y && oldScale.z == newScale.z)
        {
            return false;
        }

        ::World* worldPtr = &world;
        Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [worldPtr, entity, newScale]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (reg.valid(entity))
                {
                    if (::Transform* transform = reg.try_get<::Transform>(entity))
                        transform->scale = newScale;
                }
            },
            [worldPtr, entity, oldScale]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (reg.valid(entity))
                {
                    if (::Transform* transform = reg.try_get<::Transform>(entity))
                        transform->scale = oldScale;
                }
            },
            "Scale Entity"));
        return true;
    }

    bool CommitEntityRename(::World& world, ::EntityID entity, const std::string& newName)
    {
        entt::registry& registry = world.GetRegistry();
        if (entity == entt::null || !registry.valid(entity) || newName.empty())
        {
            return false;
        }

        const ::NameComponent* existing = registry.try_get<::NameComponent>(entity);
        const std::string oldName = existing ? existing->name : std::string{};
        if (oldName == newName)
        {
            return false;
        }

        ::World* worldPtr = &world;
        const bool hadComponent = existing != nullptr;
        Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [worldPtr, entity, newName]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (!reg.valid(entity))
                    return;
                if (::NameComponent* name = reg.try_get<::NameComponent>(entity))
                    name->name = newName;
                else
                    reg.emplace<::NameComponent>(entity, ::NameComponent{newName});
            },
            [worldPtr, entity, oldName, hadComponent]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (!reg.valid(entity))
                    return;
                if (!hadComponent)
                {
                    reg.remove<::NameComponent>(entity);
                    return;
                }
                if (::NameComponent* name = reg.try_get<::NameComponent>(entity))
                    name->name = oldName;
            },
            "Rename Entity"));
        return true;
    }

    // ========================================================================
    // AlignEntityToGround
    // ========================================================================

    bool AlignEntityToGround(::World& world, ::EntityID entity)
    {
        entt::registry& registry = world.GetRegistry();
        if (entity == entt::null || !registry.valid(entity))
        {
            return false;
        }
        ::Transform* transform = registry.try_get<::Transform>(entity);
        if (!transform)
        {
            return false;
        }

        // No physics world exists in the editor (Jolt runs only in the game
        // runtime) and CPU-side mesh bounds are unavailable, so every visible
        // Transform+MeshRenderer entity is approximated as a unit cube scaled
        // by |Transform.scale|, centred on its world position (axis-aligned;
        // rotation ignored).
        const XMFLOAT3 worldPos = GetWorldPosition(registry, *transform);
        const float halfX = 0.5f * std::abs(transform->scale.x);
        const float halfY = 0.5f * std::abs(transform->scale.y);
        const float halfZ = 0.5f * std::abs(transform->scale.z);
        const float bottom = worldPos.y - halfY;

        constexpr float kSurfaceEpsilon = 1e-3f;
        float bestTop = 0.0f;
        bool foundSurface = false;

        auto meshView = registry.view<::Transform, ::MeshRenderer>();
        for (::EntityID other : meshView)
        {
            if (other == entity || IsSelfOrDescendantOf(registry, other, entity))
            {
                continue;
            }
            if (!meshView.get<::MeshRenderer>(other).visible)
            {
                continue;
            }

            const ::Transform& otherTransform = meshView.get<::Transform>(other);
            const XMFLOAT3 otherPos = GetWorldPosition(registry, otherTransform);
            const float otherHalfX = 0.5f * std::abs(otherTransform.scale.x);
            const float otherHalfY = 0.5f * std::abs(otherTransform.scale.y);
            const float otherHalfZ = 0.5f * std::abs(otherTransform.scale.z);

            // Require XZ footprint overlap.
            if (std::abs(otherPos.x - worldPos.x) > halfX + otherHalfX)
            {
                continue;
            }
            if (std::abs(otherPos.z - worldPos.z) > halfZ + otherHalfZ)
            {
                continue;
            }

            const float top = otherPos.y + otherHalfY;
            if (top <= bottom + kSurfaceEpsilon && (!foundSurface || top > bestTop))
            {
                bestTop = top;
                foundSurface = true;
            }
        }

        // Fall back to the y=0 ground plane; this also lifts an entity that
        // has sunk below it.
        const float targetBottom = foundSurface ? bestTop : 0.0f;
        const float worldDeltaY = (targetBottom + halfY) - worldPos.y;
        if (std::abs(worldDeltaY) < 1e-5f)
        {
            return false;
        }

        const XMFLOAT3 localDelta = WorldDeltaToParentLocal(registry, *transform, XMFLOAT3{0.0f, worldDeltaY, 0.0f});
        const XMFLOAT3 oldPosition = transform->position;
        const XMFLOAT3 newPosition = {oldPosition.x + localDelta.x, oldPosition.y + localDelta.y,
                                      oldPosition.z + localDelta.z};

        ::World* worldPtr = &world; // safe: SwapWorld clears the history before freeing the World
        Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [worldPtr, entity, newPosition]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (!reg.valid(entity))
                {
                    return;
                }
                if (::Transform* t = reg.try_get<::Transform>(entity))
                {
                    t->position = newPosition;
                }
            },
            [worldPtr, entity, oldPosition]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (!reg.valid(entity))
                {
                    return;
                }
                if (::Transform* t = reg.try_get<::Transform>(entity))
                {
                    t->position = oldPosition;
                }
            },
            "Align Entity to Ground"));
        return true;
    }

    // ========================================================================
    // WorldDeltaToParentLocal
    // ========================================================================

    XMFLOAT3 WorldDeltaToParentLocal(const entt::registry& registry, const ::Transform& transform,
                                     const XMFLOAT3& worldDelta)
    {
        if (transform.parent == entt::null || !registry.valid(transform.parent))
        {
            return worldDelta;
        }
        const ::Transform* parentTransform = registry.try_get<::Transform>(transform.parent);
        if (!parentTransform)
        {
            return worldDelta;
        }

        const XMMATRIX parentWorld = parentTransform->GetWorldMatrix(registry);
        XMVECTOR determinant;
        const XMMATRIX invParent = XMMatrixInverse(&determinant, parentWorld);

        // TransformNormal equivalent that also exists in the Linux math stub:
        // apply the affine inverse to the delta endpoint and subtract the
        // transformed origin.
        const XMVECTOR origin = XMVector3TransformCoord(XMVectorZero(), invParent);
        const XMVECTOR tip = XMVector3TransformCoord(XMLoadFloat3(&worldDelta), invParent);
        XMFLOAT3 local;
        XMStoreFloat3(&local, XMVectorSubtract(tip, origin));
        return local;
    }

} // namespace SparkEditor::SceneEditTools
