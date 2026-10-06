/**
 * @file InspectorPendingWorldEdit.h
 * @brief Commit policy for World-backed Inspector field edits (EDT-210).
 *
 * The World-backed Inspector writes reflected fields straight into live
 * component memory, so an edit gesture (a drag, or typing into a field) is
 * turned into one CommandHistory entry after the fact: the document snapshot
 * taken before the first change is held as a pending baseline and handed to
 * EditorUI::RecordAppliedDocumentMutation when the gesture ends.
 *
 * This class owns the "when does the gesture end" decision. It is ImGui-free
 * so the policy can be exercised against a real World and CommandHistory.
 */

#pragma once

#include "Engine/ECS/Components.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace SparkEditor
{
    class InspectorPendingWorldEdit
    {
      public:
        /// Records the applied mutation; returns true when a history entry was made.
        using CommitFn = std::function<bool(const std::string& before, const std::string& description)>;

        enum class Outcome
        {
            None,      ///< No edit gesture was pending.
            Pending,   ///< The gesture is still in progress; nothing recorded yet.
            Committed, ///< The gesture ended and was handed to the commit function.
            Discarded  ///< The baseline was stale and was dropped instead of replayed.
        };

        [[nodiscard]] bool HasPending() const { return m_pending; }
        [[nodiscard]] ::EntityID PendingEntity() const { return m_entity; }

        /**
         * @brief Note that a field changed during this frame.
         *
         * The first change of a gesture captures the pre-edit document
         * snapshot; later changes (including other fields changed while the
         * gesture is open) extend the same gesture so it undoes as one step.
         *
         * @param document       Identity of the edited document (the live World).
         * @param entity         Entity whose component changed.
         * @param before         Document snapshot captured before this frame's change.
         * @param description    Undo label, e.g. "Edit Transform".
         * @param activeItem     ImGui active item id after the change (0 if none).
         * @param historySequence CommandHistory edit sequence when the change was made.
         */
        void NoteChange(const void* document, ::EntityID entity, std::string before, std::string description,
                        uint32_t activeItem, uint64_t historySequence)
        {
            if (m_pending || before.empty())
                return;
            m_pending = true;
            m_document = document;
            m_entity = entity;
            m_before = std::move(before);
            m_description = std::move(description);
            m_activeItem = activeItem;
            m_historySequence = historySequence;
        }

        /**
         * @brief Decide, once per Inspector frame, whether the gesture ended.
         *
         * @param document        Current document identity.
         * @param renderedEntity  Entity the World-backed Inspector rendered this
         *                        frame, or entt::null when it did not render one.
         * @param activeItem      Current ImGui active item id (0 if none).
         * @param historySequence Current CommandHistory edit sequence.
         * @param commit          Records the mutation (EditorUI in production).
         */
        Outcome Settle(const void* document, ::EntityID renderedEntity, uint32_t activeItem, uint64_t historySequence,
                       const CommitFn& commit)
        {
            if (!m_pending)
                return Outcome::None;
            if (IsStale(document, historySequence))
            {
                Reset();
                return Outcome::Discarded;
            }
            // The gesture continues only while the Inspector still shows the
            // same entity and the widget that made the change still owns the
            // active id. Selection moving, the panel not rendering this
            // entity, release, or another widget activating all end it.
            const bool sameEntity = renderedEntity != entt::null && renderedEntity == m_entity;
            const bool sameWidgetActive = activeItem != 0 && activeItem == m_activeItem;
            if (sameEntity && sameWidgetActive)
                return Outcome::Pending;
            return CommitNow(commit);
        }

        /**
         * @brief End the gesture now (before an immediate Add/Remove command),
         * so no other command is recorded ahead of it.
         */
        Outcome Flush(const void* document, uint64_t historySequence, const CommitFn& commit)
        {
            if (!m_pending)
                return Outcome::None;
            if (IsStale(document, historySequence))
            {
                Reset();
                return Outcome::Discarded;
            }
            return CommitNow(commit);
        }

        void Discard() { Reset(); }

      private:
        /// A baseline taken from another document (File -> Open swapped the
        /// World) or before another history operation ran (undo/redo, a
        /// command from another panel) would, if replayed, make this entry's
        /// undo restore state that history already owns. Drop it instead.
        [[nodiscard]] bool IsStale(const void* document, uint64_t historySequence) const
        {
            return document != m_document || historySequence != m_historySequence;
        }

        Outcome CommitNow(const CommitFn& commit)
        {
            std::string before = std::move(m_before);
            std::string description = std::move(m_description);
            Reset();
            if (commit)
                commit(before, description);
            return Outcome::Committed;
        }

        void Reset()
        {
            m_pending = false;
            m_document = nullptr;
            m_entity = entt::null;
            m_before.clear();
            m_description.clear();
            m_activeItem = 0;
            m_historySequence = 0;
        }

        bool m_pending = false;
        const void* m_document = nullptr;
        ::EntityID m_entity = entt::null;
        std::string m_before;
        std::string m_description;
        uint32_t m_activeItem = 0;
        uint64_t m_historySequence = 0;
    };
} // namespace SparkEditor
