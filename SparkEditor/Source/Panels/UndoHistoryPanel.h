/**
 * @file UndoHistoryPanel.h
 * @brief Panel displaying the undo/redo command history
 * @author Spark Engine Team
 * @date 2025
 *
 * Shows a visual timeline of all executed commands, allowing users
 * to click on any point in the history to jump to that state.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../UndoRedo/UndoRedoManager.h"

namespace SparkEditor
{

    /**
     * @brief Panel displaying the undo/redo command history
     *
     * Shows all executed commands in a scrollable list with the current
     * position highlighted. Users can click on entries to jump to any
     * point in history.
     */
    class UndoHistoryPanel : public EditorPanel
    {
      public:
        /**
         * @brief Construct the undo history panel
         * @param undoRedoManager Pointer to the undo/redo manager (non-owning)
         */
        explicit UndoHistoryPanel(UndoRedoManager* undoRedoManager);

        ~UndoHistoryPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "UndoHistoryPanel"; }

      private:
        void RenderHistoryList();
        void RenderToolbar();

        UndoRedoManager* m_undoRedoManager = nullptr;
        bool m_autoScroll = true;
    };

} // namespace SparkEditor
