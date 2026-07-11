/**
 * @file SceneEditTools.h
 * @brief Undoable scene-manipulation helpers for the editor viewport (W9)
 * @author Spark Engine Team
 * @date 2026
 *
 * Quality-of-life editing operations on the live ECS World document:
 * - DuplicateEntity: deep-clone an entity (every copy-constructible engine
 *   component; children recursively), offset +1m on X.
 * - AlignEntityToGround: snap an entity down onto the highest AABB surface
 *   below it, or the y=0 ground plane when nothing is underneath.
 *
 * Every mutation executes through Spark::Editor::CommandHistory — the single
 * W6-converged undo surface — and captures entity IDs, never component
 * pointers (EnTT invalidates component pointers on storage growth).
 *
 * Honesty notes on ground alignment: the editor has NO physics world (Jolt
 * only runs in the game runtime) and no CPU-side mesh bounds are available
 * from WorldMeshCache, so alignment approximates every visible
 * Transform+MeshRenderer entity as a unit cube scaled by |Transform.scale|,
 * centred on its world position (axis-aligned; rotation is ignored).
 */

#pragma once

#include "Engine/ECS/Components.h"

namespace SparkEditor::SceneEditTools
{

    /**
     * @brief Deep-duplicate an entity through CommandHistory.
     *
     * Clones every copy-constructible engine component via EnTT's type-erased
     * storage API, recursively clones parented children, re-wires the
     * duplicate's Transform parent/children links (the raw copy would alias
     * the source's children), offsets the duplicate root +1m on X, and
     * appends " (Copy)" to its NameComponent.
     *
     * Undo destroys the created entities; redo recreates them with the SAME
     * entity identifiers (registry create-with-hint), so later commands that
     * captured those ids keep resolving across undo/redo cycles.
     *
     * @param world  The live document World.
     * @param source Entity to duplicate.
     * @return The duplicated root entity, or entt::null on failure.
     */
    ::EntityID DuplicateEntity(::World& world, ::EntityID source);

    /**
     * @brief Snap an entity down to the surface below it, undoably.
     *
     * Searches other visible Transform+MeshRenderer entities whose scaled
     * unit-cube AABB overlaps this entity's XZ footprint and whose top face
     * lies at or below this entity's bottom face; rests the entity on the
     * highest such surface. Falls back to the y=0 ground plane (which also
     * lifts an entity that has sunk below it). Descendants of the entity are
     * ignored as support candidates (they would move with it).
     *
     * @param world  The live document World.
     * @param entity Entity to align.
     * @return true if the entity moved (a command was executed).
     */
    bool AlignEntityToGround(::World& world, ::EntityID entity);

    /**
     * @brief Convert a world-space translation delta into the local space of
     * the transform's parent (identity when unparented).
     *
     * Shared by AlignEntityToGround and the SceneViewPanel translate-gizmo
     * drag so world-space manipulation stays correct under parented (and
     * rotated/scaled-parent) entities.
     */
    DirectX::XMFLOAT3 WorldDeltaToParentLocal(const entt::registry& registry, const ::Transform& transform,
                                              const DirectX::XMFLOAT3& worldDelta);

} // namespace SparkEditor::SceneEditTools
