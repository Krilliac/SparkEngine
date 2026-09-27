/**
 * @file InspectorWorldAssetDrop.h
 * @brief Asset drag/drop onto World-backed Inspector fields (EDT-210).
 *
 * The World-backed Inspector renders every component through reflection, so
 * asset-path fields (MeshRenderer.meshPath / materialPath) arrive as plain
 * string fields. This ImGui-free core decides which reflected fields accept an
 * asset drop, and applies an accepted drop to the live World as exactly one
 * CommandHistory entry through the same snapshot commit the Inspector uses for
 * field edits, so drop, undo, redo, save and reload all see the same document.
 */

#pragma once

// EditorAssetReference.h uses std::string without including <string>.
#include <string>

#include "../AssetPipeline/EditorAssetReference.h"
#include "InspectorPendingWorldEdit.h"

#include <functional>
#include <optional>
#include <string_view>

namespace SparkEditor
{
    /// Asset kind a reflected World component field accepts, or nullopt when it takes no asset drop.
    std::optional<EditorAssetKind> AssetKindForField(std::string_view componentType, std::string_view fieldName);

    enum class AssetDropResult
    {
        Applied,    ///< The field changed and one history entry was recorded.
        Unchanged,  ///< The field already held the reference; nothing recorded.
        Rejected,   ///< Not an asset field, wrong kind/unsafe reference, or no history entry could be made.
        NoComponent ///< The entity is gone or no longer has the component.
    };

    /**
     * @brief Assign @p reference to an asset field of a live World entity.
     *
     * The reference is validated against the field's asset kind before
     * anything is captured or written. On a real change the document snapshot
     * is taken, the field is written, and @p commit records the mutation as
     * one undo step ("Assign Mesh Asset" / "Assign Material Asset"). When no
     * snapshot is available or @p commit records nothing, the write is reverted
     * so the World never holds an edit that undo cannot reach.
     *
     * Recording replaces the World's registry (the command's redo restores the
     * post-edit snapshot), so callers must not reuse component pointers taken
     * before the call.
     */
    AssetDropResult ApplyWorldAssetDrop(::World& world, ::EntityID entity, std::string_view componentType,
                                        std::string_view fieldName, const std::string& reference,
                                        const std::function<std::string()>& snapshot,
                                        const InspectorPendingWorldEdit::CommitFn& commit);
} // namespace SparkEditor
