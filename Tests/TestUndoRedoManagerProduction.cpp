// TestUndoRedoManagerProduction.cpp - tests the editor's real history manager

#include "TestFramework.h"
#include "UndoRedo/UndoRedoManager.h"

#include <stdexcept>

namespace
{
    std::unique_ptr<SparkEditor::EditorCommand> SetValue(int& value, int next)
    {
        const int previous = value;
        return std::make_unique<SparkEditor::LambdaEditorCommand>(
            [&value, next]() { value = next; }, [&value, previous]() { value = previous; }, "Set value");
    }
} // namespace

TEST(UndoRedoProduction_TransientRollbackPreservesDocumentUndo)
{
    SparkEditor::UndoRedoManager history;
    int value = 0;
    history.ExecuteCommand(SetValue(value, 1));
    EXPECT_EQ(value, 1);

    EXPECT_TRUE(history.BeginTransientSession());
    history.ExecuteCommand(SetValue(value, 2));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(history.Undo());
    EXPECT_EQ(value, 1);
    EXPECT_FALSE(history.Undo()); // cannot cross the pre-play barrier

    value = 1; // the caller restores its serialized document snapshot first
    EXPECT_TRUE(history.RollbackTransientSession());
    EXPECT_TRUE(history.CanUndo());
    EXPECT_TRUE(history.Undo());
    EXPECT_EQ(value, 0);
}

TEST(UndoRedoProduction_TransientRollbackRestoresPreexistingRedo)
{
    SparkEditor::UndoRedoManager history;
    int value = 0;
    history.ExecuteCommand(SetValue(value, 1));
    EXPECT_TRUE(history.Undo());
    EXPECT_TRUE(history.CanRedo());

    EXPECT_TRUE(history.BeginTransientSession());
    history.ExecuteCommand(SetValue(value, 2));
    EXPECT_FALSE(history.CanRedo());

    value = 0;
    EXPECT_TRUE(history.RollbackTransientSession());
    EXPECT_TRUE(history.CanRedo());
    EXPECT_TRUE(history.Redo());
    EXPECT_EQ(value, 1);
}

TEST(UndoRedoProduction_CommandExceptionRestoresDispatchBoundary)
{
    SparkEditor::UndoRedoManager history;
    auto throwingCommand = std::make_unique<SparkEditor::LambdaEditorCommand>(
        []() { throw std::runtime_error("editor command failed"); }, []() {}, "Throwing command");

    EXPECT_FALSE(SparkEditor::UndoRedoManager::IsDispatchingCommand());
    try
    {
        history.ExecuteCommand(std::move(throwingCommand));
    }
    catch (const std::runtime_error&)
    {
    }

    EXPECT_FALSE(SparkEditor::UndoRedoManager::IsDispatchingCommand());
    EXPECT_FALSE(history.CanUndo());
}
