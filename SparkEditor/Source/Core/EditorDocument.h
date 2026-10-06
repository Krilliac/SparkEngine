/**
 * @file EditorDocument.h
 * @brief The edited scene document: the live ::World, its selection, and its
 *        snapshot-based undoable mutations.
 * @author Spark Engine Team
 * @date 2026
 *
 * EditorUI used to build every document-level undo command inline, three
 * times over, in a translation unit that pulls in all of ImGui and every
 * panel. EditorDocument is the ImGui-free core those paths share, so the
 * create/delete/applied-edit commands the editor records are the same code the
 * tests drive.
 *
 * Contract:
 * - Thread affinity: game (editor UI) thread only.
 * - Ownership: owns the document ::World. Commands it records capture `this`
 *   and snapshot text, never component pointers; the owner must clear
 *   Spark::Editor::CommandHistory before replacing the World or destroying the
 *   document (EditorUI::SwapWorld does).
 * - Allocation: each recorded mutation allocates its before/after snapshot
 *   strings and one LambdaCommand; nothing is allocated per frame.
 * - Scalability: snapshot cost is linear in document size; intended for
 *   editor-authored scenes, not streamed open worlds.
 */

#pragma once

#include "Engine/ECS/Components.h"

#include <functional>
#include <memory>
#include <string>

namespace SparkEditor
{

    class EditorDocument
    {
      public:
        /// Called after a mutation was recorded on CommandHistory, with its description.
        using RecordedHook = std::function<void(const std::string& description)>;
        /// Called after a snapshot replaced the World's contents (undo, redo, play-mode restore).
        using RestoredHook = std::function<void()>;

        EditorDocument() = default;
        EditorDocument(const EditorDocument&) = delete;
        EditorDocument& operator=(const EditorDocument&) = delete;

        /// Install the owner's bookkeeping hooks. Either may be empty.
        void SetHooks(RecordedHook onRecorded, RestoredHook onRestored);

        ::World* GetWorld() const { return m_world.get(); }

        /// Replace the document World (nullptr retires it) and clear the selection.
        /// The caller must have cleared CommandHistory first.
        void ReplaceWorld(std::unique_ptr<::World> world);

        ::EntityID GetSelectedEntity() const { return m_selectedEntity; }
        void SetSelectedEntity(::EntityID entity) { m_selectedEntity = entity; }

        /// Serialized snapshot of the whole document; empty when there is no World.
        std::string Capture() const;

        /// Replace the World's contents with a snapshot this process produced,
        /// keeping the World object's address. Selects `selection` when it is
        /// valid in the restored document. Returns false, leaving the World
        /// untouched, when the snapshot does not parse.
        bool Restore(const std::string& json, ::EntityID selection);

        /// Create an entity from a GameObject-menu name ("Empty", "Cube",
        /// "Camera", "Point Light", "Sprite", ...) as one undoable step and
        /// select it. An unknown name, or one whose components are not
        /// registered, leaves no entity and no history entry.
        bool CreateEntity(const std::string& menuName);

        /// Delete the selected entity as one undoable step.
        bool DeleteSelected();

        /// Record an edit that was already applied to the World (the Inspector
        /// commit path) as one undoable step. A no-op edit records nothing.
        bool RecordApplied(const std::string& before, const std::string& description);

      private:
        /// The only place document commands reach CommandHistory::Execute.
        bool CommitSnapshot(const std::string& before, const std::string& after, ::EntityID redoSelection,
                            ::EntityID undoSelection, const std::string& description);

        std::unique_ptr<::World> m_world;
        ::EntityID m_selectedEntity = entt::null;
        RecordedHook m_onRecorded;
        RestoredHook m_onRestored;
    };

} // namespace SparkEditor
