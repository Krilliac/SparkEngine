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

        /**
         * @brief Clone every copy-constructible component of source onto a
         * fresh entity via EnTT's type-erased storage API.
         *
         * Non-copy-constructible components are skipped (EnTT's type-erased
         * push safely no-ops for them). When hint != entt::null the registry
         * is asked to reuse that exact identifier (redo path).
         */
        ::EntityID CloneComponents(entt::registry& registry, ::EntityID source, ::EntityID hint)
        {
            const ::EntityID destination = (hint == entt::null) ? registry.create() : registry.create(hint);
            for (auto&& [id, storage] : registry.storage())
            {
                (void)id;
                if (storage.contains(source))
                {
                    storage.push(destination, storage.value(source));
                }
            }
            return destination;
        }

        /**
         * @brief Recursive deep clone (pre-order). Appends every created
         * entity to outCreated; consumes hints in the same pre-order so a
         * redo recreates identical entity ids.
         */
        ::EntityID CloneRecursive(entt::registry& registry, ::EntityID source, ::EntityID parent,
                                  std::vector<::EntityID>& outCreated, const std::vector<::EntityID>& hints,
                                  size_t& hintCursor, int depth)
        {
            if (depth > kMaxHierarchyDepth)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "DuplicateEntity: hierarchy depth limit hit — truncating");
                return entt::null;
            }

            const ::EntityID hint =
                (hintCursor < hints.size()) ? hints[hintCursor] : static_cast<::EntityID>(entt::null);
            ++hintCursor;

            const ::EntityID destination = CloneComponents(registry, source, hint);
            outCreated.push_back(destination);

            if (::Transform* destTransform = registry.try_get<::Transform>(destination))
            {
                // The raw component copy aliased the SOURCE's children list —
                // the duplicate gets its own freshly-cloned children instead.
                const std::vector<::EntityID> sourceChildren = destTransform->children;
                destTransform->children.clear();
                destTransform->parent = parent;

                for (::EntityID child : sourceChildren)
                {
                    if (child == entt::null || !registry.valid(child) || !registry.all_of<::Transform>(child))
                    {
                        continue;
                    }
                    const ::EntityID newChild =
                        CloneRecursive(registry, child, destination, outCreated, hints, hintCursor, depth + 1);
                    // Re-fetch: the recursive clone may have grown the
                    // Transform storage and invalidated destTransform.
                    destTransform = registry.try_get<::Transform>(destination);
                    if (destTransform && newChild != entt::null)
                    {
                        destTransform->children.push_back(newChild);
                    }
                }
            }
            return destination;
        }

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
    // DuplicateEntity
    // ========================================================================

    ::EntityID DuplicateEntity(::World& world, ::EntityID source)
    {
        entt::registry& registry = world.GetRegistry();
        if (source == entt::null || !registry.valid(source))
        {
            return entt::null;
        }

        std::string description = "Duplicate Entity";
        if (const ::NameComponent* nameComponent = registry.try_get<::NameComponent>(source))
        {
            if (!nameComponent->name.empty())
            {
                description = "Duplicate '" + nameComponent->name + "'";
            }
        }

        // Shared between Execute/Undo: the entities created by the last
        // Execute, in pre-order. Undo destroys them but KEEPS the ids so a
        // Redo can recreate the exact same identifiers via create(hint) —
        // later commands that captured those ids keep resolving.
        auto created = std::make_shared<std::vector<::EntityID>>();

        // Raw World pointer is safe here: SwapWorld() clears the
        // CommandHistory BEFORE freeing the old World, so this command can
        // never outlive the World it captured.
        ::World* worldPtr = &world;

        auto redo = [worldPtr, source, created]()
        {
            entt::registry& reg = worldPtr->GetRegistry();
            if (source == entt::null || !reg.valid(source))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "DuplicateEntity: source entity no longer valid — skipping");
                created->clear();
                return;
            }

            const std::vector<::EntityID> hints = *created; // empty on the first execute
            created->clear();
            size_t hintCursor = 0;

            const ::Transform* sourceTransform = reg.try_get<::Transform>(source);
            const ::EntityID parent = sourceTransform ? sourceTransform->parent : static_cast<::EntityID>(entt::null);

            const ::EntityID root = CloneRecursive(reg, source, parent, *created, hints, hintCursor, 0);
            if (root == entt::null)
            {
                return;
            }

            // Offset the duplicate +1m on X so it doesn't z-fight the
            // original, and tag its display name.
            if (::Transform* rootTransform = reg.try_get<::Transform>(root))
            {
                rootTransform->position.x += 1.0f;
            }
            if (::NameComponent* rootName = reg.try_get<::NameComponent>(root))
            {
                rootName->name += " (Copy)";
            }

            // Register the duplicate with its parent's child list. Deep
            // children were wired inside CloneRecursive; only the root
            // crosses into a pre-existing entity.
            if (parent != entt::null && reg.valid(parent))
            {
                if (::Transform* parentTransform = reg.try_get<::Transform>(parent))
                {
                    parentTransform->children.push_back(root);
                }
            }

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Duplicated entity %u as %u (%zu total entities cloned)",
                           static_cast<uint32_t>(source), static_cast<uint32_t>(root), created->size());
        };

        auto undo = [worldPtr, created]()
        {
            entt::registry& reg = worldPtr->GetRegistry();
            if (created->empty())
            {
                return;
            }

            // Detach the duplicated root from its parent's child list.
            const ::EntityID root = created->front();
            if (reg.valid(root))
            {
                if (const ::Transform* rootTransform = reg.try_get<::Transform>(root))
                {
                    const ::EntityID parent = rootTransform->parent;
                    if (parent != entt::null && reg.valid(parent))
                    {
                        if (::Transform* parentTransform = reg.try_get<::Transform>(parent))
                        {
                            std::erase(parentTransform->children, root);
                        }
                    }
                }
            }

            // Destroy children before parents (reverse pre-order). The ids
            // stay in 'created' as create(hint) seeds for a subsequent Redo.
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

        return created->empty() ? static_cast<::EntityID>(entt::null) : created->front();
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
