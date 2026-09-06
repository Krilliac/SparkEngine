/**
 * @file TestEditorUndoHierarchyReal.cpp
 * @brief Real-class tests for the World-backed Hierarchy undo surface.
 *
 * The World-mode hierarchy used to offer create/delete only, with rename and
 * duplicate documented as "deferred" — a delete or rename could not be undone.
 * Rename now routes through SceneEditTools::CommitEntityRename and duplicate
 * through SceneEditTools::DuplicateEntity, so every hierarchy mutation is a
 * CommandHistory entry. These tests exercise those production helpers directly.
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Gizmos/SceneEditTools.h"

#include <string>

namespace
{
    std::string NameOf(::World& world, ::EntityID entity)
    {
        const ::NameComponent* name = world.GetRegistry().try_get<::NameComponent>(entity);
        return name ? name->name : std::string{};
    }
} // namespace

TEST(EditorUndoHierarchy_RenameIsUndoableAndRedoable)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Soldier");
    EXPECT_EQ(NameOf(world, entity), std::string("Soldier"));

    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Sniper"));
    EXPECT_EQ(NameOf(world, entity), std::string("Sniper"));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));
    EXPECT_STR_CONTAINS(Spark::Editor::CommandHistory::GetInstance().GetUndoDescription(), "Rename");

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(NameOf(world, entity), std::string("Soldier"));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_EQ(NameOf(world, entity), std::string("Sniper"));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameRejectsEmptyAndUnchangedNames)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Crate");

    // An empty inline-rename field and a no-op rename must not consume an undo step.
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, ""));
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Crate"));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));
    EXPECT_EQ(NameOf(world, entity), std::string("Crate"));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameOfUnnamedEntityUndoesBackToNoComponent)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    entt::registry& registry = world.GetRegistry();
    const ::EntityID entity = static_cast<::EntityID>(registry.create());
    EXPECT_FALSE(registry.try_get<::NameComponent>(entity) != nullptr);

    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Named"));
    EXPECT_EQ(NameOf(world, entity), std::string("Named"));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(registry.try_get<::NameComponent>(entity) != nullptr);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameOfDestroyedEntityIsRefused)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Gone");
    world.DestroyEntity(entity);

    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Ghost"));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_DuplicateIsUndoableAndRedoable)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID source = world.CreateEntity("Barrel");
    world.AddComponent<::Transform>(source);
    const size_t before = world.GetRegistry().storage<entt::entity>().size();

    const ::EntityID copy = SparkEditor::SceneEditTools::DuplicateEntity(world, source);
    EXPECT_TRUE(copy != entt::null);
    EXPECT_GT(world.GetRegistry().storage<entt::entity>().size(), before);
    EXPECT_TRUE(world.GetRegistry().valid(copy));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(world.GetRegistry().valid(copy));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_TRUE(world.GetRegistry().valid(copy));
    EXPECT_STR_CONTAINS(NameOf(world, copy), "Barrel");

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameThenDuplicateUndoInReverseOrder)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Alpha");
    world.AddComponent<::Transform>(entity);

    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Beta"));
    const ::EntityID copy = SparkEditor::SceneEditTools::DuplicateEntity(world, entity);
    EXPECT_TRUE(copy != entt::null);
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(2));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo()); // undo duplicate
    EXPECT_FALSE(world.GetRegistry().valid(copy));
    EXPECT_EQ(NameOf(world, entity), std::string("Beta"));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo()); // undo rename
    EXPECT_EQ(NameOf(world, entity), std::string("Alpha"));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}
