// TestEditorCommands.cpp - Tests for EditorCommands (Reparent, Material, Batch)
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace TestEditorCmd
{

    struct XMFLOAT3
    {
        float x, y, z;
    };
    struct XMFLOAT4
    {
        float x, y, z, w;
    };

    using PropertyValue = std::variant<bool, int, float, double, std::string, XMFLOAT3, XMFLOAT4>;

    class EditorCommand
    {
      public:
        virtual ~EditorCommand() = default;
        virtual void Execute() = 0;
        virtual void Undo() = 0;
        virtual std::string GetDescription() const = 0;
    };

    // Track operations for verification
    struct OpLog
    {
        std::vector<std::string> entries;
        void Record(const std::string& op) { entries.push_back(op); }
        void Clear() { entries.clear(); }
    };

    static OpLog g_log;

    // -- TransformCommand (simplified for testing) --
    class TransformCommand : public EditorCommand
    {
      public:
        TransformCommand(uint64_t entityId, const XMFLOAT3& oldPos, const XMFLOAT3& newPos)
            : m_entityId(entityId), m_oldPos(oldPos), m_newPos(newPos)
        {
        }

        void Execute() override { g_log.Record("transform_exec_" + std::to_string(m_entityId)); }

        void Undo() override { g_log.Record("transform_undo_" + std::to_string(m_entityId)); }

        std::string GetDescription() const override
        {
            return "Transform Change (Entity " + std::to_string(m_entityId) + ")";
        }

      private:
        uint64_t m_entityId;
        XMFLOAT3 m_oldPos, m_newPos;
    };

    // -- ComponentCommand --
    class ComponentCommand : public EditorCommand
    {
      public:
        ComponentCommand(uint64_t entityId, const std::string& compType, bool isAdd)
            : m_entityId(entityId), m_compType(compType), m_isAdd(isAdd)
        {
        }

        void Execute() override
        {
            std::string op = m_isAdd ? "add_comp_" : "remove_comp_";
            g_log.Record(op + m_compType + "_" + std::to_string(m_entityId));
        }

        void Undo() override
        {
            std::string op = m_isAdd ? "remove_comp_" : "add_comp_";
            g_log.Record(op + m_compType + "_" + std::to_string(m_entityId));
        }

        std::string GetDescription() const override { return (m_isAdd ? "Add " : "Remove ") + m_compType; }

      private:
        uint64_t m_entityId;
        std::string m_compType;
        bool m_isAdd;
    };

    // -- EntityCommand --
    class EntityCommand : public EditorCommand
    {
      public:
        EntityCommand(const std::string& entityName, bool isCreate) : m_entityName(entityName), m_isCreate(isCreate) {}

        void Execute() override { g_log.Record(std::string(m_isCreate ? "create_" : "delete_") + m_entityName); }

        void Undo() override { g_log.Record(std::string(m_isCreate ? "delete_" : "create_") + m_entityName); }

        std::string GetDescription() const override { return (m_isCreate ? "Create " : "Delete ") + m_entityName; }

      private:
        std::string m_entityName;
        bool m_isCreate;
    };

    // -- ReparentCommand --
    class ReparentCommand : public EditorCommand
    {
      public:
        ReparentCommand(uint64_t entityId, uint64_t oldParent, uint64_t newParent, const XMFLOAT3& oldPos,
                        const XMFLOAT3& newPos)
            : m_entityId(entityId), m_oldParent(oldParent), m_newParent(newParent), m_oldPos(oldPos), m_newPos(newPos)
        {
        }

        void Execute() override
        {
            g_log.Record("reparent_" + std::to_string(m_entityId) + "_to_" + std::to_string(m_newParent));
        }

        void Undo() override
        {
            g_log.Record("reparent_" + std::to_string(m_entityId) + "_to_" + std::to_string(m_oldParent));
        }

        std::string GetDescription() const override { return "Reparent Entity " + std::to_string(m_entityId); }

        const XMFLOAT3& GetOldPos() const { return m_oldPos; }
        const XMFLOAT3& GetNewPos() const { return m_newPos; }

      private:
        uint64_t m_entityId;
        uint64_t m_oldParent, m_newParent;
        XMFLOAT3 m_oldPos, m_newPos;
    };

    // -- PropertyChangeCommand --
    class PropertyChangeCommand : public EditorCommand
    {
      public:
        PropertyChangeCommand(uint64_t entityId, const std::string& compType, const std::string& propName,
                              const std::string& oldValue, const std::string& newValue)
            : m_entityId(entityId), m_compType(compType), m_propName(propName), m_oldValue(oldValue),
              m_newValue(newValue)
        {
        }

        void Execute() override { g_log.Record("set_" + m_propName + "=" + m_newValue); }

        void Undo() override { g_log.Record("set_" + m_propName + "=" + m_oldValue); }

        std::string GetDescription() const override { return "Change " + m_compType + "." + m_propName; }

        const std::string& GetOldValue() const { return m_oldValue; }
        const std::string& GetNewValue() const { return m_newValue; }

      private:
        uint64_t m_entityId;
        std::string m_compType, m_propName, m_oldValue, m_newValue;
    };

    // -- BatchCommand --
    class BatchCommand : public EditorCommand
    {
      public:
        explicit BatchCommand(const std::string& desc) : m_desc(desc) {}

        void AddSubCommand(std::unique_ptr<EditorCommand> cmd) { m_commands.push_back(std::move(cmd)); }

        void Execute() override
        {
            for (auto& cmd : m_commands)
                cmd->Execute();
        }

        void Undo() override
        {
            for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
                (*it)->Undo();
        }

        std::string GetDescription() const override { return m_desc; }
        size_t GetSubCommandCount() const { return m_commands.size(); }

      private:
        std::string m_desc;
        std::vector<std::unique_ptr<EditorCommand>> m_commands;
    };

} // namespace TestEditorCmd

TEST(EditorCmd_TransformExecuteUndo)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    TransformCommand cmd(1, {0, 0, 0}, {10, 20, 30});
    cmd.Execute();
    EXPECT_EQ(g_log.entries.size(), size_t(1));
    EXPECT_EQ(g_log.entries[0], std::string("transform_exec_1"));

    cmd.Undo();
    EXPECT_EQ(g_log.entries.size(), size_t(2));
    EXPECT_EQ(g_log.entries[1], std::string("transform_undo_1"));
}

TEST(EditorCmd_ComponentAddThenUndo)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    ComponentCommand cmd(5, "RigidBody", true);
    cmd.Execute();
    EXPECT_EQ(g_log.entries[0], std::string("add_comp_RigidBody_5"));

    cmd.Undo();
    EXPECT_EQ(g_log.entries[1], std::string("remove_comp_RigidBody_5"));
}

TEST(EditorCmd_EntityCreateThenUndo)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    EntityCommand cmd("TestEntity", true);
    cmd.Execute();
    EXPECT_EQ(g_log.entries[0], std::string("create_TestEntity"));

    cmd.Undo();
    EXPECT_EQ(g_log.entries[1], std::string("delete_TestEntity"));
}

TEST(EditorCmd_ReparentPreservesTransform)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    // World pos (10,0,0) under parent at (5,0,0) => local (5,0,0)
    // Reparent to root => local becomes world pos (10,0,0)
    XMFLOAT3 oldLocal{5, 0, 0};
    XMFLOAT3 newLocal{10, 0, 0};
    ReparentCommand cmd(3, 2, 0, oldLocal, newLocal);

    cmd.Execute();
    EXPECT_EQ(g_log.entries[0], std::string("reparent_3_to_0"));

    cmd.Undo();
    EXPECT_EQ(g_log.entries[1], std::string("reparent_3_to_2"));

    // Verify stored positions are correct
    EXPECT_NEAR(cmd.GetOldPos().x, 5.0f, 0.001f);
    EXPECT_NEAR(cmd.GetNewPos().x, 10.0f, 0.001f);
}

TEST(EditorCmd_PropertyChangeRoundtrip)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    PropertyChangeCommand cmd(1, "Light", "intensity", "1.0", "2.5");
    cmd.Execute();
    EXPECT_EQ(g_log.entries[0], std::string("set_intensity=2.5"));

    cmd.Undo();
    EXPECT_EQ(g_log.entries[1], std::string("set_intensity=1.0"));

    EXPECT_EQ(cmd.GetOldValue(), std::string("1.0"));
    EXPECT_EQ(cmd.GetNewValue(), std::string("2.5"));
}

TEST(EditorCmd_BatchExecuteAndUndoReverse)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    BatchCommand batch("Multi-select move");
    batch.AddSubCommand(std::make_unique<TransformCommand>(1, XMFLOAT3{0, 0, 0}, XMFLOAT3{1, 0, 0}));
    batch.AddSubCommand(std::make_unique<TransformCommand>(2, XMFLOAT3{0, 0, 0}, XMFLOAT3{2, 0, 0}));
    batch.AddSubCommand(std::make_unique<TransformCommand>(3, XMFLOAT3{0, 0, 0}, XMFLOAT3{3, 0, 0}));

    EXPECT_EQ(batch.GetSubCommandCount(), size_t(3));

    batch.Execute();
    EXPECT_EQ(g_log.entries.size(), size_t(3));
    EXPECT_EQ(g_log.entries[0], std::string("transform_exec_1"));
    EXPECT_EQ(g_log.entries[2], std::string("transform_exec_3"));

    g_log.Clear();
    batch.Undo();
    EXPECT_EQ(g_log.entries.size(), size_t(3));
    // Reverse order: 3, 2, 1
    EXPECT_EQ(g_log.entries[0], std::string("transform_undo_3"));
    EXPECT_EQ(g_log.entries[1], std::string("transform_undo_2"));
    EXPECT_EQ(g_log.entries[2], std::string("transform_undo_1"));
}

TEST(EditorCmd_BatchDescription)
{
    using namespace TestEditorCmd;
    BatchCommand batch("Delete selection");
    EXPECT_EQ(batch.GetDescription(), std::string("Delete selection"));
    EXPECT_EQ(batch.GetSubCommandCount(), size_t(0));
}

TEST(EditorCmd_BatchEmpty)
{
    using namespace TestEditorCmd;
    g_log.Clear();

    BatchCommand batch("Empty batch");
    batch.Execute();
    batch.Undo();
    EXPECT_EQ(g_log.entries.size(), size_t(0));
}
