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
#include <cstddef>
#include <cstdint>
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

        /// Pointer to one of Transform's XMFLOAT3 members. The declarator is parenthesized
        /// because `XMFLOAT3 ::Transform::*` lexes as the qualified name `XMFLOAT3::Transform`.
        using TransformVectorMember = XMFLOAT3(::Transform::*);

        /// One undoable change of a Transform vector (position, rotation or scale).
        /// Captures the entity id and the member, never a component pointer.
        bool CommitTransformVector(::World& world, ::EntityID entity, TransformVectorMember member,
                                   const XMFLOAT3& oldValue, const XMFLOAT3& newValue, const char* description)
        {
            entt::registry& registry = world.GetRegistry();
            if (entity == entt::null || !registry.valid(entity) || !registry.try_get<::Transform>(entity))
            {
                return false;
            }
            if (oldValue.x == newValue.x && oldValue.y == newValue.y && oldValue.z == newValue.z)
            {
                return false;
            }

            ::World* worldPtr = &world; // safe: SwapWorld clears the history before freeing the World
            auto assign = [worldPtr, entity, member](const XMFLOAT3& value)
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (!reg.valid(entity))
                    return;
                if (::Transform* transform = reg.try_get<::Transform>(entity))
                    transform->*member = value;
            };
            Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                [assign, newValue]() { assign(newValue); }, [assign, oldValue]() { assign(oldValue); }, description));
            return true;
        }
    } // namespace


    // ========================================================================
    // Single-value undoable commits (viewport gizmos, hierarchy rename)
    // ========================================================================

    bool CommitEntityPosition(::World& world, ::EntityID entity, const XMFLOAT3& oldPosition,
                              const XMFLOAT3& newPosition)
    {
        return CommitTransformVector(world, entity, &::Transform::position, oldPosition, newPosition, "Move Entity");
    }

    bool CommitEntityRotation(::World& world, ::EntityID entity, const XMFLOAT3& oldRotation,
                              const XMFLOAT3& newRotation)
    {
        return CommitTransformVector(world, entity, &::Transform::rotation, oldRotation, newRotation, "Rotate Entity");
    }

    bool CommitEntityScale(::World& world, ::EntityID entity, const XMFLOAT3& oldScale, const XMFLOAT3& newScale)
    {
        return CommitTransformVector(world, entity, &::Transform::scale, oldScale, newScale, "Scale Entity");
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

    bool CommitEntityReparent(::World& world, ::EntityID child, ::EntityID newParent)
    {
        entt::registry& registry = world.GetRegistry();
        if (child == entt::null || !registry.valid(child) || child == newParent)
        {
            return false;
        }
        if (newParent != entt::null && !registry.valid(newParent))
        {
            return false;
        }
        if (newParent != entt::null && IsSelfOrDescendantOf(registry, newParent, child))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Reparent refused: entity %u is an ancestor of target parent %u",
                           static_cast<uint32_t>(child), static_cast<uint32_t>(newParent));
            return false;
        }

        // The exact prior state, so undo reproduces the document byte for byte:
        // the raw parent link, the child's slot in the old parent's list, and
        // which Transforms the redo will have to add.
        const ::Transform* childTransform = registry.try_get<::Transform>(child);
        const bool childHadTransform = childTransform != nullptr;
        const ::EntityID oldParentLink = childTransform ? childTransform->parent : static_cast<::EntityID>(entt::null);
        const ::EntityID oldParent =
            (oldParentLink != entt::null && registry.valid(oldParentLink)) ? oldParentLink : entt::null;
        if (oldParent == newParent)
        {
            return false;
        }
        size_t oldIndex = 0;
        if (oldParent != entt::null)
        {
            if (const ::Transform* oldParentTransform = registry.try_get<::Transform>(oldParent))
            {
                const auto& siblings = oldParentTransform->children;
                oldIndex = static_cast<size_t>(std::find(siblings.begin(), siblings.end(), child) - siblings.begin());
            }
        }
        const bool newParentHadTransform = newParent == entt::null || registry.all_of<::Transform>(newParent);

        ::World* worldPtr = &world;
        Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [worldPtr, child, newParent]() { worldPtr->SetParent(child, newParent); },
            [worldPtr, child, newParent, oldParentLink, oldParent, oldIndex, childHadTransform, newParentHadTransform]()
            {
                entt::registry& reg = worldPtr->GetRegistry();
                if (!reg.valid(child))
                    return;
                if (newParent != entt::null && reg.valid(newParent))
                {
                    if (::Transform* parentTransform = reg.try_get<::Transform>(newParent))
                        std::erase(parentTransform->children, child);
                    if (!newParentHadTransform)
                        reg.remove<::Transform>(newParent);
                }
                if (!childHadTransform)
                {
                    reg.remove<::Transform>(child);
                    return;
                }
                if (::Transform* transform = reg.try_get<::Transform>(child))
                    transform->parent = oldParentLink;
                if (oldParent != entt::null && reg.valid(oldParent))
                {
                    if (::Transform* oldParentTransform = reg.try_get<::Transform>(oldParent))
                    {
                        auto& siblings = oldParentTransform->children;
                        const size_t slot = std::min(oldIndex, siblings.size());
                        siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(slot), child);
                    }
                }
            },
            "Reparent Entity"));
        return true;
    }

    // ========================================================================
    // CommitSceneImport
    // ========================================================================

    std::vector<::EntityID> CommitSceneImport(::World& world, std::vector<SceneObjectRecord> records,
                                              const std::string& description)
    {
        if (records.empty())
        {
            return {};
        }

        // Shared between Execute/Undo: the entities created by the last
        // Execute. Undo destroys them but KEEPS the ids so Redo recreates the
        // exact same identifiers via create(hint).
        auto shared = std::make_shared<const std::vector<SceneObjectRecord>>(std::move(records));
        auto created = std::make_shared<std::vector<::EntityID>>();
        ::World* worldPtr = &world; // safe: SwapWorld clears the history before freeing the World

        auto redo = [worldPtr, shared, created]()
        {
            entt::registry& reg = worldPtr->GetRegistry();
            const std::vector<::EntityID> hints = *created; // empty on the first execute
            created->clear();
            created->reserve(shared->size());

            size_t index = 0;
            for (const SceneObjectRecord& record : *shared)
            {
                const ::EntityID hint = (index < hints.size()) ? hints[index] : static_cast<::EntityID>(entt::null);
                ++index;
                const ::EntityID entity = (hint == entt::null) ? reg.create() : reg.create(hint);
                created->push_back(entity);

                reg.emplace<::NameComponent>(entity, ::NameComponent{record.name});

                ::Transform& transform = reg.emplace<::Transform>(entity);
                transform.position = {record.position[0], record.position[1], record.position[2]};
                // .scene rotations are authored in degrees; ::Transform stores Euler degrees.
                transform.rotation = {record.rotationDeg[0], record.rotationDeg[1], record.rotationDeg[2]};
                transform.scale = {record.scale[0], record.scale[1], record.scale[2]};

                // A cube has no model file: the reserved primitive resolves to the
                // same centered unit cube the game instantiates for cube [Object]s.
                ::MeshRenderer& meshRenderer = reg.emplace<::MeshRenderer>(entity);
                meshRenderer.meshPath = record.model.empty() ? std::string("__spark_primitive_Cube.obj") : record.model;
                meshRenderer.materialPath = record.material;
            }
        };

        auto undo = [worldPtr, created]()
        {
            entt::registry& reg = worldPtr->GetRegistry();
            // Destroy in reverse creation order; the ids stay as create(hint) seeds.
            for (auto it = created->rbegin(); it != created->rend(); ++it)
            {
                if (*it != entt::null && reg.valid(*it))
                {
                    worldPtr->DestroyEntity(*it);
                }
            }
        };

        Spark::Editor::CommandHistory::GetInstance().Execute(
            std::make_unique<Spark::Editor::LambdaCommand>(redo, undo, description));
        return *created;
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

        return CommitTransformVector(world, entity, &::Transform::position, oldPosition, newPosition,
                                     "Align Entity to Ground");
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
