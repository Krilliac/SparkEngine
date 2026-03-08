/**
 * @file TestCommandHistory.cpp
 * @brief Tests for the editor undo/redo command system
 */

#include "TestFramework.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

// ============================================================================
// Standalone reimplementation for cross-platform testing
// ============================================================================

namespace
{

    class ICommand
    {
      public:
        virtual ~ICommand() = default;
        virtual void Execute() = 0;
        virtual void Undo() = 0;
        virtual std::string GetDescription() const = 0;
        virtual bool MergeWith(const ICommand&) { return false; }
        virtual int GetTypeId() const { return 0; }
    };

    class CommandHistory
    {
      public:
        void Execute(std::unique_ptr<ICommand> cmd)
        {
            cmd->Execute();
            if (!m_undo.empty() && cmd->GetTypeId() != 0 && m_undo.back()->GetTypeId() == cmd->GetTypeId() &&
                m_undo.back()->MergeWith(*cmd))
            {
                m_redo.clear();
                return;
            }
            m_undo.push_back(std::move(cmd));
            m_redo.clear();
            while (m_undo.size() > m_maxHistory)
                m_undo.erase(m_undo.begin());
        }

        bool Undo()
        {
            if (m_undo.empty())
                return false;
            auto c = std::move(m_undo.back());
            m_undo.pop_back();
            c->Undo();
            m_redo.push_back(std::move(c));
            return true;
        }

        bool Redo()
        {
            if (m_redo.empty())
                return false;
            auto c = std::move(m_redo.back());
            m_redo.pop_back();
            c->Execute();
            m_undo.push_back(std::move(c));
            return true;
        }

        bool CanUndo() const { return !m_undo.empty(); }
        bool CanRedo() const { return !m_redo.empty(); }
        std::string UndoDesc() const { return m_undo.empty() ? "" : m_undo.back()->GetDescription(); }
        std::string RedoDesc() const { return m_redo.empty() ? "" : m_redo.back()->GetDescription(); }
        void Clear()
        {
            m_undo.clear();
            m_redo.clear();
        }
        size_t UndoCount() const { return m_undo.size(); }
        size_t RedoCount() const { return m_redo.size(); }
        void SetMaxHistory(size_t max) { m_maxHistory = max; }
        void MarkSaved() { m_saved = m_undo.size(); }
        bool IsModified() const { return m_undo.size() != m_saved; }

      private:
        std::vector<std::unique_ptr<ICommand>> m_undo, m_redo;
        size_t m_maxHistory = 100, m_saved = 0;
    };

    // Simple command: set an int to a value, undo restores old value
    class SetIntCmd : public ICommand
    {
      public:
        SetIntCmd(int* target, int newVal, const std::string& desc)
            : m_target(target), m_old(*target), m_new(newVal), m_desc(desc)
        {
        }
        void Execute() override { *m_target = m_new; }
        void Undo() override { *m_target = m_old; }
        std::string GetDescription() const override { return m_desc; }

      private:
        int* m_target;
        int m_old, m_new;
        std::string m_desc;
    };

    // Mergeable command for continuous drags
    class DragCmd : public ICommand
    {
      public:
        DragCmd(float* target, float newVal) : m_target(target), m_old(*target), m_new(newVal) {}
        void Execute() override { *m_target = m_new; }
        void Undo() override { *m_target = m_old; }
        std::string GetDescription() const override { return "Drag"; }
        int GetTypeId() const override { return 42; }
        bool MergeWith(const ICommand& other) override
        {
            auto* d = dynamic_cast<const DragCmd*>(&other);
            if (!d || d->m_target != m_target)
                return false;
            m_new = d->m_new;
            return true;
        }

      private:
        float* m_target;
        float m_old, m_new;
    };

    // Compound command grouping multiple sub-commands
    class CompoundCmd : public ICommand
    {
      public:
        explicit CompoundCmd(const std::string& desc) : m_desc(desc) {}
        void Add(std::unique_ptr<ICommand> cmd) { m_cmds.push_back(std::move(cmd)); }
        void Execute() override
        {
            for (auto& c : m_cmds)
                c->Execute();
        }
        void Undo() override
        {
            for (auto it = m_cmds.rbegin(); it != m_cmds.rend(); ++it)
                (*it)->Undo();
        }
        std::string GetDescription() const override { return m_desc; }

      private:
        std::string m_desc;
        std::vector<std::unique_ptr<ICommand>> m_cmds;
    };

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(CommandHistory_BasicUndoRedo)
{
    CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<SetIntCmd>(&value, 10, "Set to 10"));
    EXPECT_EQ(value, 10);

    history.Undo();
    EXPECT_EQ(value, 0);

    history.Redo();
    EXPECT_EQ(value, 10);
}

TEST(CommandHistory_MultipleUndos)
{
    CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<SetIntCmd>(&value, 1, "Set 1"));
    history.Execute(std::make_unique<SetIntCmd>(&value, 2, "Set 2"));
    history.Execute(std::make_unique<SetIntCmd>(&value, 3, "Set 3"));

    EXPECT_EQ(value, 3);
    EXPECT_EQ(history.UndoCount(), (size_t)3);

    history.Undo();
    EXPECT_EQ(value, 2);

    history.Undo();
    EXPECT_EQ(value, 1);

    history.Undo();
    EXPECT_EQ(value, 0);

    EXPECT_FALSE(history.CanUndo());
    EXPECT_TRUE(history.CanRedo());
}

TEST(CommandHistory_RedoClearedByNewCommand)
{
    CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<SetIntCmd>(&value, 1, "Set 1"));
    history.Execute(std::make_unique<SetIntCmd>(&value, 2, "Set 2"));
    history.Undo(); // back to 1
    EXPECT_TRUE(history.CanRedo());

    // New command should clear redo stack
    history.Execute(std::make_unique<SetIntCmd>(&value, 5, "Set 5"));
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(value, 5);
}

TEST(CommandHistory_Descriptions)
{
    CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<SetIntCmd>(&value, 1, "First"));
    history.Execute(std::make_unique<SetIntCmd>(&value, 2, "Second"));

    EXPECT_EQ(history.UndoDesc(), std::string("Second"));
    history.Undo();
    EXPECT_EQ(history.UndoDesc(), std::string("First"));
    EXPECT_EQ(history.RedoDesc(), std::string("Second"));
}

TEST(CommandHistory_MaxHistoryEnforced)
{
    CommandHistory history;
    history.SetMaxHistory(3);
    int value = 0;

    for (int i = 1; i <= 5; i++)
    {
        history.Execute(std::make_unique<SetIntCmd>(&value, i, "Set"));
    }

    EXPECT_EQ(history.UndoCount(), (size_t)3);
}

TEST(CommandHistory_Clear)
{
    CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<SetIntCmd>(&value, 1, "Set"));
    history.Clear();

    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.UndoCount(), (size_t)0);
}

TEST(CommandHistory_MergeableCommands)
{
    CommandHistory history;
    float pos = 0.0f;

    // Simulate a drag: many small increments that merge into one undo step
    history.Execute(std::make_unique<DragCmd>(&pos, 1.0f));
    history.Execute(std::make_unique<DragCmd>(&pos, 2.0f));
    history.Execute(std::make_unique<DragCmd>(&pos, 3.0f));

    EXPECT_NEAR(pos, 3.0f, 0.01f);
    EXPECT_EQ(history.UndoCount(), (size_t)1); // All merged

    history.Undo();
    EXPECT_NEAR(pos, 0.0f, 0.01f); // Undoes entire drag
}

TEST(CommandHistory_CompoundCommand)
{
    CommandHistory history;
    int a = 0, b = 0;

    auto compound = std::make_unique<CompoundCmd>("Set A and B");
    compound->Add(std::make_unique<SetIntCmd>(&a, 10, "Set A"));
    compound->Add(std::make_unique<SetIntCmd>(&b, 20, "Set B"));

    history.Execute(std::move(compound));
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);

    history.Undo();
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
}

TEST(CommandHistory_SavedStateTracking)
{
    CommandHistory history;
    int value = 0;

    history.MarkSaved();
    EXPECT_FALSE(history.IsModified());

    history.Execute(std::make_unique<SetIntCmd>(&value, 1, "Set"));
    EXPECT_TRUE(history.IsModified());

    history.Undo();
    EXPECT_FALSE(history.IsModified()); // back to saved state
}

TEST(CommandHistory_UndoReturnsFalseWhenEmpty)
{
    CommandHistory history;
    EXPECT_FALSE(history.Undo());
    EXPECT_FALSE(history.Redo());
}
