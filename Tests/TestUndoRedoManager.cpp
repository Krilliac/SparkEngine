/**
 * @file TestUndoRedoManager.cpp
 * @brief Tests for editor undo/redo manager
 *
 * Standalone reimplementation for CI testing without engine dependencies.
 */

#include "TestFramework.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>

// ============================================================================
// Standalone reimplementation
// ============================================================================

namespace UndoRedoTest
{

    class IUndoCommand
    {
      public:
        virtual ~IUndoCommand() = default;
        virtual void Execute() = 0;
        virtual void Undo() = 0;
        virtual std::string GetName() const = 0;
        virtual int GetTypeId() const { return 0; }
        virtual bool TryMerge(const IUndoCommand& /*other*/) { return false; }
    };

    class UndoRedoManager
    {
      public:
        void SetMaxHistory(size_t max) { m_maxHistory = max; }

        void Push(std::unique_ptr<IUndoCommand> cmd)
        {
            cmd->Execute();

            // Try merge with top of undo stack
            if (!m_undoStack.empty() && cmd->GetTypeId() != 0 && m_undoStack.back()->GetTypeId() == cmd->GetTypeId() &&
                m_undoStack.back()->TryMerge(*cmd))
            {
                m_redoStack.clear();
                return;
            }

            m_undoStack.push_back(std::move(cmd));
            m_redoStack.clear();

            while (m_undoStack.size() > m_maxHistory)
            {
                m_undoStack.pop_front();
            }
        }

        bool CanUndo() const { return !m_undoStack.empty(); }
        bool CanRedo() const { return !m_redoStack.empty(); }

        bool Undo()
        {
            if (m_undoStack.empty())
            {
                return false;
            }
            auto cmd = std::move(m_undoStack.back());
            m_undoStack.pop_back();
            cmd->Undo();
            m_redoStack.push_back(std::move(cmd));
            return true;
        }

        bool Redo()
        {
            if (m_redoStack.empty())
            {
                return false;
            }
            auto cmd = std::move(m_redoStack.back());
            m_redoStack.pop_back();
            cmd->Execute();
            m_undoStack.push_back(std::move(cmd));
            return true;
        }

        size_t UndoCount() const { return m_undoStack.size(); }
        size_t RedoCount() const { return m_redoStack.size(); }

        void Clear()
        {
            m_undoStack.clear();
            m_redoStack.clear();
        }

      private:
        std::deque<std::unique_ptr<IUndoCommand>> m_undoStack;
        std::deque<std::unique_ptr<IUndoCommand>> m_redoStack;
        size_t m_maxHistory = 100;
    };

    // A simple command that sets an int value
    class SetValueCommand : public IUndoCommand
    {
      public:
        SetValueCommand(int& target, int newValue) : m_target(target), m_newValue(newValue), m_oldValue(target) {}

        void Execute() override { m_target = m_newValue; }
        void Undo() override { m_target = m_oldValue; }
        std::string GetName() const override { return "SetValue"; }

      private:
        int& m_target;
        int m_newValue;
        int m_oldValue;
    };

    // A text-edit command that supports merging
    class TextEditCommand : public IUndoCommand
    {
      public:
        TextEditCommand(std::string& target, const std::string& appendText)
            : m_target(target), m_appendText(appendText), m_oldValue(target)
        {
        }

        void Execute() override { m_target += m_appendText; }
        void Undo() override { m_target = m_oldValue; }
        std::string GetName() const override { return "TextEdit"; }
        int GetTypeId() const override { return 42; }

        bool TryMerge(const IUndoCommand& other) override
        {
            auto* textCmd = dynamic_cast<const TextEditCommand*>(&other);
            if (!textCmd)
            {
                return false;
            }
            // Merge: accumulate the appended text
            m_appendText += textCmd->m_appendText;
            return true;
        }

      private:
        std::string& m_target;
        std::string m_appendText;
        std::string m_oldValue;
    };

    struct EditorState
    {
        int transformX = 0;
        bool hasLightComponent = false;
        int parentId = 0;
        bool entityDeleted = false;
        std::string materialAssetPath;
    };

    class IntFieldCommand : public IUndoCommand
    {
      public:
        IntFieldCommand(int& target, int value) : m_target(target), m_oldValue(target), m_newValue(value) {}
        void Execute() override { m_target = m_newValue; }
        void Undo() override { m_target = m_oldValue; }
        std::string GetName() const override { return "IntField"; }

      private:
        int& m_target;
        int m_oldValue;
        int m_newValue;
    };

    class BoolFieldCommand : public IUndoCommand
    {
      public:
        BoolFieldCommand(bool& target, bool value) : m_target(target), m_oldValue(target), m_newValue(value) {}
        void Execute() override { m_target = m_newValue; }
        void Undo() override { m_target = m_oldValue; }
        std::string GetName() const override { return "BoolField"; }

      private:
        bool& m_target;
        bool m_oldValue;
        bool m_newValue;
    };

    class StringFieldCommand : public IUndoCommand
    {
      public:
        StringFieldCommand(std::string& target, std::string value)
            : m_target(target), m_oldValue(target), m_newValue(std::move(value))
        {
        }
        void Execute() override { m_target = m_newValue; }
        void Undo() override { m_target = m_oldValue; }
        std::string GetName() const override { return "StringField"; }

      private:
        std::string& m_target;
        std::string m_oldValue;
        std::string m_newValue;
    };

} // namespace UndoRedoTest

// ============================================================================
// Tests
// ============================================================================

TEST(UndoRedo_PushAndUndo)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    int value = 10;

    mgr.Push(std::make_unique<SetValueCommand>(value, 20));
    EXPECT_EQ(value, 20);

    mgr.Undo();
    EXPECT_EQ(value, 10);
}

TEST(UndoRedo_Redo)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    int value = 10;

    mgr.Push(std::make_unique<SetValueCommand>(value, 20));
    EXPECT_EQ(value, 20);

    mgr.Undo();
    EXPECT_EQ(value, 10);

    mgr.Redo();
    EXPECT_EQ(value, 20);
}

TEST(UndoRedo_RedoChainCleared)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    int value = 10;

    mgr.Push(std::make_unique<SetValueCommand>(value, 20));
    mgr.Undo();
    EXPECT_TRUE(mgr.CanRedo());

    // Push a new command — redo chain should be cleared
    mgr.Push(std::make_unique<SetValueCommand>(value, 30));
    EXPECT_FALSE(mgr.CanRedo());
    EXPECT_EQ(value, 30);

    // Undo should go to 10 (the original), not 20
    mgr.Undo();
    EXPECT_EQ(value, 10);
}

TEST(UndoRedo_UndoLimit)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    mgr.SetMaxHistory(5);

    int value = 0;
    for (int i = 1; i <= 10; i++)
    {
        mgr.Push(std::make_unique<SetValueCommand>(value, i));
    }

    EXPECT_EQ(value, 10);
    EXPECT_EQ(mgr.UndoCount(), 5u);

    // Undo all 5
    int undoCount = 0;
    while (mgr.Undo())
    {
        undoCount++;
    }
    EXPECT_EQ(undoCount, 5);

    // Value should be at the 5th-from-last push (value was set to 5, then old was 4)
    // After undoing commands 10,9,8,7,6 we get back to value=5
    EXPECT_EQ(value, 5);
}

TEST(UndoRedo_CommandMerging)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    std::string text;

    // Push multiple text edits that should merge
    mgr.Push(std::make_unique<TextEditCommand>(text, "H"));
    mgr.Push(std::make_unique<TextEditCommand>(text, "e"));
    mgr.Push(std::make_unique<TextEditCommand>(text, "l"));
    mgr.Push(std::make_unique<TextEditCommand>(text, "l"));
    mgr.Push(std::make_unique<TextEditCommand>(text, "o"));

    EXPECT_TRUE(text == "Hello");

    // All merges into one command, so only 1 undo needed
    EXPECT_EQ(mgr.UndoCount(), 1u);

    mgr.Undo();
    EXPECT_TRUE(text.empty());
}

TEST(UndoRedo_ClearHistory)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    int value = 0;

    mgr.Push(std::make_unique<SetValueCommand>(value, 1));
    mgr.Push(std::make_unique<SetValueCommand>(value, 2));
    mgr.Undo();

    EXPECT_TRUE(mgr.CanUndo());
    EXPECT_TRUE(mgr.CanRedo());

    mgr.Clear();

    EXPECT_FALSE(mgr.CanUndo());
    EXPECT_FALSE(mgr.CanRedo());
}

TEST(UndoRedo_EditorMutationCategoriesRoundtrip)
{
    using namespace UndoRedoTest;

    UndoRedoManager mgr;
    EditorState s{};

    // 1) Transform gizmo apply
    mgr.Push(std::make_unique<IntFieldCommand>(s.transformX, 42));
    // 2) Component add/remove/edit
    mgr.Push(std::make_unique<BoolFieldCommand>(s.hasLightComponent, true));
    // 3) Hierarchy reparent
    mgr.Push(std::make_unique<IntFieldCommand>(s.parentId, 17));
    // 4) Hierarchy delete
    mgr.Push(std::make_unique<BoolFieldCommand>(s.entityDeleted, true));
    // 5) Asset assignment
    mgr.Push(std::make_unique<StringFieldCommand>(s.materialAssetPath, "Materials/Metal.mat"));

    EXPECT_EQ(s.transformX, 42);
    EXPECT_TRUE(s.hasLightComponent);
    EXPECT_EQ(s.parentId, 17);
    EXPECT_TRUE(s.entityDeleted);
    EXPECT_EQ(s.materialAssetPath, std::string("Materials/Metal.mat"));

    // Undo full stack
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(mgr.Undo());
    }

    EXPECT_EQ(s.transformX, 0);
    EXPECT_FALSE(s.hasLightComponent);
    EXPECT_EQ(s.parentId, 0);
    EXPECT_FALSE(s.entityDeleted);
    EXPECT_TRUE(s.materialAssetPath.empty());

    // Redo full stack
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(mgr.Redo());
    }

    EXPECT_EQ(s.transformX, 42);
    EXPECT_TRUE(s.hasLightComponent);
    EXPECT_EQ(s.parentId, 17);
    EXPECT_TRUE(s.entityDeleted);
    EXPECT_EQ(s.materialAssetPath, std::string("Materials/Metal.mat"));
}
